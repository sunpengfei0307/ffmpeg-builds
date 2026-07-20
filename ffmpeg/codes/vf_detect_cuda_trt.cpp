/*
 * TensorRT wrapper for CUDA object detection filter.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "vf_detect_cuda_trt.h"

#include <stdio.h>
#include <string.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

using namespace nvinfer1;

class DetectLogger final : public ILogger {
public:
    void log(Severity severity, const char *msg) noexcept override
    {
        if (severity <= Severity::kWARNING && msg)
            last = msg;
    }

    std::string last;
};

template<typename T>
struct TrtDestroy {
    void operator()(T *p) const
    {
        if (p)
            p->destroy();
    }
};

template<typename T>
using TrtPtr = std::unique_ptr<T, TrtDestroy<T> >;

namespace {

static void set_error(char *errbuf, int errbuf_size, const char *msg)
{
    if (errbuf && errbuf_size > 0)
        snprintf(errbuf, errbuf_size, "%s", msg ? msg : "unknown TensorRT error");
}

static bool read_file(const char *path, std::vector<char> &data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    file.seekg(0, std::ios::beg);
    data.resize((size_t)size);
    file.read(data.data(), size);
    return (bool)file;
}

/* SM: major*10+minor (e.g. 8.6 -> 86). TRT tag: getInferLibVersion()/10 (e.g. 8601 -> 860). */
static int get_gpu_sm_tag(int cuda_device, int *sm_tag, char *errbuf, int errbuf_size)
{
    cudaDeviceProp prop;
    int dev = cuda_device >= 0 ? cuda_device : 0;
    cudaError_t err;

    err = cudaGetDeviceProperties(&prop, dev);
    if (err != cudaSuccess) {
        set_error(errbuf, errbuf_size, cudaGetErrorString(err));
        return -1;
    }

    *sm_tag = prop.major * 10 + prop.minor;
    return 0;
}

static int get_trt_version_tag(void)
{
    int ver = getInferLibVersion(); /* major*1000 + minor*100 + patch */
    return ver / 10;                /* e.g. 8601 -> 860, filename trt860 */
}

static std::string cache_path_for_onnx(const char *path, int input_w, int input_h,
                                       int fp16, int sm_tag, int trt_tag)
{
    char suffix[160];
    snprintf(suffix, sizeof(suffix), ".%dx%d.%s.sm%d.trt%d.trt",
             input_w, input_h, fp16 ? "fp16" : "fp32", sm_tag, trt_tag);
    return std::string(path) + suffix;
}

static int resolve_onnx_cache_path(const FFDetectTRTConfig *cfg,
                                   std::string &cache_path,
                                   char *errbuf, int errbuf_size)
{
    int sm_tag = 0;
    int trt_tag;

    if (get_gpu_sm_tag(cfg->cuda_device, &sm_tag, errbuf, errbuf_size) < 0)
        return -1;

    trt_tag = get_trt_version_tag();
    cache_path = cache_path_for_onnx(cfg->engine_path, cfg->input_w, cfg->input_h,
                                     cfg->fp16, sm_tag, trt_tag);
    return 0;
}

static bool has_suffix(const char *path, const char *suffix)
{
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    return plen >= slen && !strcmp(path + plen - slen, suffix);
}

static int ensure_cuda_ready(int cuda_device, char *errbuf, int errbuf_size)
{
    int dev = cuda_device >= 0 ? cuda_device : 0;
    cudaError_t err;

    /*
     * IMPORTANT: do NOT call cudaSetDevice() here.
     *
     * FFmpeg (e.g. -hwaccel cuvid) creates its own non-primary CUDA driver
     * context and allocates all device buffers / frames inside it. That
     * context is pushed current (cuCtxPushCurrent) by the caller before both
     * engine creation and execution. Calling cudaSetDevice() would bind the
     * CUDA runtime to the device's PRIMARY context instead, so TensorRT would
     * run in a different context than the one owning our input/output buffers,
     * producing "Cask convolution execution" errors at enqueue time.
     *
     * cudaFree(0) simply forces lazy runtime initialization against whatever
     * context is currently current (FFmpeg's), which is exactly what we want.
     */
    (void)dev;

    err = cudaFree(0);
    if (err != cudaSuccess) {
        set_error(errbuf, errbuf_size, cudaGetErrorString(err));
        return -1;
    }

    return 0;
}

static TrtPtr<ICudaEngine> build_engine_from_onnx(const char *onnx_path,
                                                  const std::string &cache_path,
                                                  const FFDetectTRTConfig *cfg,
                                                  IRuntime *runtime,
                                                  DetectLogger &logger,
                                                  char *errbuf, int errbuf_size)
{
    TrtPtr<IBuilder> builder(createInferBuilder(logger));
    if (!builder) {
        set_error(errbuf, errbuf_size, "createInferBuilder failed");
        return nullptr;
    }

    const uint32_t flags = 1U << (uint32_t)NetworkDefinitionCreationFlag::kEXPLICIT_BATCH;
    TrtPtr<INetworkDefinition> network(builder->createNetworkV2(flags));
    if (!network) {
        set_error(errbuf, errbuf_size, "createNetworkV2 failed");
        return nullptr;
    }

    TrtPtr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger));
    if (!parser) {
        set_error(errbuf, errbuf_size, "create ONNX parser failed");
        return nullptr;
    }

    if (!parser->parseFromFile(onnx_path, (int)ILogger::Severity::kWARNING)) {
        set_error(errbuf, errbuf_size, logger.last.empty() ? "parse ONNX failed"
                                                           : logger.last.c_str());
        return nullptr;
    }

    TrtPtr<IBuilderConfig> build_cfg(builder->createBuilderConfig());
    if (!build_cfg) {
        set_error(errbuf, errbuf_size, "createBuilderConfig failed");
        return nullptr;
    }

    build_cfg->setMaxWorkspaceSize(1ULL << 30);
    if (cfg->fp16 && builder->platformHasFastFp16())
        build_cfg->setFlag(BuilderFlag::kFP16);

    if (network->getNbInputs() > 0) {
        ITensor *input = network->getInput(0);
        input->setType(DataType::kFLOAT);
        Dims dims = input->getDimensions();
        if (dims.nbDims == 4) {
            dims.d[0] = 1;
            dims.d[2] = cfg->input_h;
            dims.d[3] = cfg->input_w;
            input->setDimensions(dims);
        }
    }

    for (int i = 0; i < network->getNbOutputs(); i++) {
        ITensor *output = network->getOutput(i);
        if (output)
            output->setType(DataType::kFLOAT);
    }

    TrtPtr<IHostMemory> serialized(builder->buildSerializedNetwork(*network, *build_cfg));
    if (!serialized) {
        set_error(errbuf, errbuf_size, logger.last.empty() ? "buildSerializedNetwork failed"
                                                           : logger.last.c_str());
        return nullptr;
    }

    std::ofstream cache(cache_path, std::ios::binary);
    if (cache)
        cache.write(static_cast<const char *>(serialized->data()), serialized->size());

    if (!runtime) {
        set_error(errbuf, errbuf_size, "TensorRT runtime is not initialized");
        return nullptr;
    }

    return TrtPtr<ICudaEngine>(runtime->deserializeCudaEngine(serialized->data(),
                                                              serialized->size()));
}

static const char *find_tensor(ICudaEngine *engine, TensorIOMode mode,
                               const char *a, const char *b, const char *c)
{
    int nb = engine->getNbIOTensors();
    for (int i = 0; i < nb; i++) {
        const char *name = engine->getIOTensorName(i);
        if (!name || engine->getTensorIOMode(name) != mode)
            continue;
        if ((a && strstr(name, a)) || (b && strstr(name, b)) || (c && strstr(name, c)))
            return name;
    }

    for (int i = 0; i < nb; i++) {
        const char *name = engine->getIOTensorName(i);
        if (name && engine->getTensorIOMode(name) == mode)
            return name;
    }

    return nullptr;
}

static int count_io_tensors(ICudaEngine *engine, TensorIOMode mode)
{
    int nb = engine->getNbIOTensors();
    int count = 0;
    for (int i = 0; i < nb; i++) {
        const char *name = engine->getIOTensorName(i);
        if (name && engine->getTensorIOMode(name) == mode)
            count++;
    }
    return count;
}

static DetectModelType detect_model_type_from_dims(Dims dims, DetectModelType requested)
{
    if (requested != DETECT_MODEL_AUTO)
        return requested;

    if (dims.nbDims == 3 && dims.d[0] == 1) {
        int a = dims.d[1] > 0 ? dims.d[1] : 0;
        int b = dims.d[2] > 0 ? dims.d[2] : 0;
        if ((a >= 5 && b >= 100) || (b >= 5 && a >= 100))
            return DETECT_MODEL_YOLOV8;
    }

    return DETECT_MODEL_AUTO;
}

static void fill_yolo_info(Dims dims, FFDetectTRTModelInfo *info)
{
    int a = dims.d[1];
    int b = dims.d[2];

    info->yolo_transposed = 0;
    info->num_classes = 0;
    info->num_anchors = 0;

    if (dims.nbDims != 3 || dims.d[0] != 1 || a <= 0 || b <= 0)
        return;

    if (a >= 5 && b >= 100 && a < b) {
        info->num_classes = a - 4;
        info->num_anchors = b;
    } else if (b >= 5 && a >= 100 && b < a) {
        info->num_classes = b - 4;
        info->num_anchors = a;
        info->yolo_transposed = 1;
    }
}

static size_t tensor_volume(const Dims &dims)
{
    size_t vol = 1;
    int i;

    for (i = 0; i < dims.nbDims; i++) {
        if (dims.d[i] <= 0)
            return 0;
        vol *= (size_t)dims.d[i];
    }
    return vol;
}

static void format_dims(char *buf, int buf_size, const Dims &dims)
{
    int pos = 0;
    int i;

    if (!buf || buf_size <= 0)
        return;

    buf[0] = '\0';
    for (i = 0; i < dims.nbDims && pos < buf_size - 1; i++) {
        pos += snprintf(buf + pos, buf_size - pos, "%s%d",
                        i ? "x" : "[", dims.d[i]);
    }
    if (pos < buf_size - 1)
        snprintf(buf + pos, buf_size - pos, "]");
}

struct FaceHeadCandidate {
    const char *name;
    int grid;
    size_t bytes;
};

static int compare_face_head_desc(const void *a, const void *b)
{
    const FaceHeadCandidate *ha = (const FaceHeadCandidate *)a;
    const FaceHeadCandidate *hb = (const FaceHeadCandidate *)b;
    return hb->grid - ha->grid;
}

static size_t tensor_nbytes(ICudaEngine *engine, IExecutionContext *context,
                            const char *name)
{
    Dims dims = context->getTensorShape(name);
    size_t vol = tensor_volume(dims);
    size_t esize = 4;

    if (!engine || !name || vol == 0)
        return 0;

    switch (engine->getTensorDataType(name)) {
    case DataType::kHALF:
        esize = 2;
        break;
    case DataType::kINT32:
    case DataType::kINT8:
    case DataType::kUINT8:
    case DataType::kBOOL:
    case DataType::kFLOAT:
    default:
        esize = 4;
        break;
    }

    return vol * esize;
}

static void fill_model_info(IExecutionContext *context, ICudaEngine *engine,
                            DetectModelType type, const char *output_name,
                            FFDetectTRTModelInfo *info)
{
    Dims dims = output_name ? context->getTensorShape(output_name) : Dims{};

    info->model_type = type;
    info->num_classes = 0;
    info->num_anchors = 0;
    info->yolo_transposed = 0;
    info->raw_output_bytes = 0;

    if (type == DETECT_MODEL_YOLOV8) {
        fill_yolo_info(dims, info);
        info->raw_output_bytes = tensor_volume(dims) * sizeof(float);
    }
}

static int configure_input_shape(IExecutionContext *context, ICudaEngine *engine,
                                 const char *input_name, int input_w, int input_h,
                                 char *errbuf, int errbuf_size)
{
    Dims dims = engine->getTensorShape(input_name);

    if (dims.nbDims != 4)
        return 0;

    dims.d[0] = 1;
    dims.d[1] = 3;
    if (dims.d[2] <= 0)
        dims.d[2] = input_h;
    if (dims.d[3] <= 0)
        dims.d[3] = input_w;

    if (!context->setInputShape(input_name, dims)) {
        set_error(errbuf, errbuf_size, "setInputShape failed for detector input");
        return -1;
    }

    return 0;
}

} // namespace

struct FFDetectTRTContext {
    DetectLogger logger;
    TrtPtr<IRuntime> runtime;
    TrtPtr<ICudaEngine> engine;
    TrtPtr<IExecutionContext> context;
    DetectModelType model_type;
    FFDetectTRTModelInfo info;
    int num_face_heads;
    int input_w;
    int input_h;
    const char *face_head_names[FF_DETECT_FACE_HEADS];
    const char *input_name;
    const char *output_name;
};

static int try_init_yolov8_face_heads(FFDetectTRTContext *ctx,
                                      IExecutionContext *context,
                                      ICudaEngine *engine,
                                      int input_w,
                                      const FFDetectTRTConfig *cfg)
{
    FaceHeadCandidate cands[FF_DETECT_FACE_HEADS];
    int nb = engine->getNbIOTensors();
    int n = 0;
    int out_count = 0;
    int i;

    out_count = count_io_tensors(engine, TensorIOMode::kOUTPUT);
    if (out_count != FF_DETECT_FACE_HEADS)
        return 0;

    for (i = 0; i < nb; i++) {
        const char *name = engine->getIOTensorName(i);
        Dims dims;

        if (!name || engine->getTensorIOMode(name) != TensorIOMode::kOUTPUT)
            continue;

        dims = context->getTensorShape(name);
        if (dims.nbDims == 4 && dims.d[0] == 1 && dims.d[1] == 80 &&
            dims.d[2] > 0 && dims.d[2] == dims.d[3]) {
            if (n >= FF_DETECT_FACE_HEADS)
                return 0;
            cands[n].name  = name;
            cands[n].grid  = dims.d[2];
            cands[n].bytes = tensor_nbytes(engine, context, name);
            n++;
        }
    }

    if (n != FF_DETECT_FACE_HEADS)
        return 0;

    qsort(cands, FF_DETECT_FACE_HEADS, sizeof(cands[0]), compare_face_head_desc);

    ctx->num_face_heads = FF_DETECT_FACE_HEADS;
    ctx->info.num_face_heads = FF_DETECT_FACE_HEADS;
    ctx->info.face_heads_total_bytes = 0;

    for (i = 0; i < FF_DETECT_FACE_HEADS; i++) {
        ctx->face_head_names[i] = cands[i].name;
        ctx->info.face_heads[i].grid_size = cands[i].grid;
        ctx->info.face_heads[i].stride    = input_w / cands[i].grid;
        ctx->info.face_heads[i].channels  = 80;
        ctx->info.face_heads[i].bytes     = cands[i].bytes;
        ctx->info.face_heads_total_bytes += cands[i].bytes;
    }

    return 1;
}

extern "C" int ff_detect_trt_resolve_cache_path(const FFDetectTRTConfig *cfg,
                                                char *out_path, int out_path_size,
                                                char *errbuf, int errbuf_size)
{
    std::string cache_path;

    if (!cfg || !cfg->engine_path || !cfg->engine_path[0] ||
        !out_path || out_path_size <= 0) {
        set_error(errbuf, errbuf_size, "invalid arguments for resolve_cache_path");
        return -1;
    }

    if (!has_suffix(cfg->engine_path, ".onnx")) {
        snprintf(out_path, out_path_size, "%s", cfg->engine_path);
        return 0;
    }

    if (ensure_cuda_ready(cfg->cuda_device, errbuf, errbuf_size) < 0)
        return -1;

    if (resolve_onnx_cache_path(cfg, cache_path, errbuf, errbuf_size) < 0)
        return -1;

    if ((int)cache_path.size() >= out_path_size) {
        set_error(errbuf, errbuf_size, "TRT cache path too long");
        return -1;
    }

    snprintf(out_path, out_path_size, "%s", cache_path.c_str());
    return 0;
}

extern "C" int ff_detect_trt_create(FFDetectTRTContext **trt,
                                     const FFDetectTRTConfig *cfg,
                                     char *errbuf, int errbuf_size)
{
    std::vector<char> engine_data;
    std::string cache_path;
    FFDetectTRTContext *ctx = nullptr;

    if (!trt || !cfg || !cfg->engine_path || !cfg->engine_path[0]) {
        set_error(errbuf, errbuf_size, "missing TensorRT engine path");
        return -1;
    }

    ctx = new FFDetectTRTContext();
    if (!ctx) {
        set_error(errbuf, errbuf_size, "out of memory");
        return -1;
    }
    ctx->model_type = DETECT_MODEL_AUTO;
    ctx->num_face_heads = 0;
    ctx->input_w = cfg->input_w;
    ctx->input_h = cfg->input_h;

    if (ensure_cuda_ready(cfg->cuda_device, errbuf, errbuf_size) < 0) {
        delete ctx;
        return -1;
    }

    ctx->runtime.reset(createInferRuntime(ctx->logger));
    if (!ctx->runtime) {
        set_error(errbuf, errbuf_size, "createInferRuntime failed");
        delete ctx;
        return -1;
    }

    if (has_suffix(cfg->engine_path, ".onnx")) {
        if (resolve_onnx_cache_path(cfg, cache_path, errbuf, errbuf_size) < 0) {
            delete ctx;
            return -1;
        }
        if (read_file(cache_path.c_str(), engine_data)) {
            ctx->engine.reset(ctx->runtime->deserializeCudaEngine(engine_data.data(),
                                                                  engine_data.size()));
            if (!ctx->engine) {
                /* Corrupt or incompatible cache: remove and rebuild. */
                remove(cache_path.c_str());
                engine_data.clear();
            }
        }
        if (!ctx->engine) {
            ctx->engine = build_engine_from_onnx(cfg->engine_path, cache_path,
                                                cfg, ctx->runtime.get(), ctx->logger,
                                                errbuf, errbuf_size);
        }
    } else {
        read_file(cfg->engine_path, engine_data);
        if (!engine_data.empty()) {
            ctx->engine.reset(ctx->runtime->deserializeCudaEngine(engine_data.data(),
                                                                  engine_data.size()));
        }
    }

    if (!ctx->engine) {
        set_error(errbuf, errbuf_size, ctx->logger.last.empty() ? "load TensorRT engine failed"
                                                               : ctx->logger.last.c_str());
        delete ctx;
        return -1;
    }

    ctx->context.reset(ctx->engine->createExecutionContext());
    if (!ctx->context) {
        set_error(errbuf, errbuf_size, "createExecutionContext failed");
        delete ctx;
        return -1;
    }

    ctx->input_name = find_tensor(ctx->engine.get(), TensorIOMode::kINPUT,
                                  "images", "input", "data");
    ctx->output_name = find_tensor(ctx->engine.get(), TensorIOMode::kOUTPUT,
                                   "output", "output0", nullptr);

    if (!ctx->input_name) {
        set_error(errbuf, errbuf_size, "engine input tensor not found");
        delete ctx;
        return -1;
    }

    if (configure_input_shape(ctx->context.get(), ctx->engine.get(), ctx->input_name,
                              cfg->input_w, cfg->input_h, errbuf, errbuf_size) < 0) {
        delete ctx;
        return -1;
    }

    if ((cfg->model_type == DETECT_MODEL_AUTO ||
         cfg->model_type == DETECT_MODEL_YOLOV8 ||
         cfg->model_type == DETECT_MODEL_YOLOV8_FACE) &&
        try_init_yolov8_face_heads(ctx, ctx->context.get(), ctx->engine.get(),
                                   cfg->input_w, cfg)) {
        ctx->model_type = DETECT_MODEL_YOLOV8_FACE;
        ctx->info.model_type = DETECT_MODEL_YOLOV8_FACE;
    } else if (cfg->model_type == DETECT_MODEL_YOLOV8_FACE) {
        char dimbuf[128];
        if (ctx->output_name) {
            Dims out_dims = ctx->context->getTensorShape(ctx->output_name);
            format_dims(dimbuf, sizeof(dimbuf), out_dims);
        } else {
            snprintf(dimbuf, sizeof(dimbuf), "unknown");
        }
        set_error(errbuf, errbuf_size,
                  "model_type=yolov8_face requires 3 outputs shaped [1,80,H,W]");
        delete ctx;
        return -1;
    } else if (cfg->model_type == DETECT_MODEL_AUTO && ctx->output_name) {
        Dims out_dims = ctx->context->getTensorShape(ctx->output_name);
        ctx->model_type = detect_model_type_from_dims(out_dims, cfg->model_type);
        if (ctx->model_type == DETECT_MODEL_AUTO) {
            char dimbuf[128];
            format_dims(dimbuf, sizeof(dimbuf), out_dims);
            snprintf(errbuf, errbuf_size,
                     "unsupported output shape (got %s); use YOLOv8 or YOLOv8-Face ONNX",
                     dimbuf);
            delete ctx;
            return -1;
        }
    } else if (cfg->model_type == DETECT_MODEL_AUTO) {
        set_error(errbuf, errbuf_size,
                  "could not detect model type; specify model_type=yolov8 or yolov8_face");
        delete ctx;
        return -1;
    } else {
        ctx->model_type = cfg->model_type;
    }

    if (ctx->model_type != DETECT_MODEL_YOLOV8_FACE)
        fill_model_info(ctx->context.get(), ctx->engine.get(), ctx->model_type,
                        ctx->output_name, &ctx->info);

    if (!ctx->output_name && ctx->model_type != DETECT_MODEL_YOLOV8_FACE) {
        set_error(errbuf, errbuf_size, "engine output tensor not found");
        delete ctx;
        return -1;
    } else if (ctx->model_type == DETECT_MODEL_YOLOV8_FACE &&
               (ctx->info.num_face_heads != FF_DETECT_FACE_HEADS ||
                ctx->info.face_heads_total_bytes == 0)) {
        set_error(errbuf, errbuf_size, "invalid YOLOv8-Face head configuration");
        delete ctx;
        return -1;
    } else if (ctx->model_type == DETECT_MODEL_YOLOV8 &&
               (ctx->info.num_anchors <= 0 || ctx->info.num_classes <= 0 ||
                ctx->info.raw_output_bytes == 0)) {
        char dimbuf[128];
        Dims out_dims = ctx->output_name ?
            ctx->context->getTensorShape(ctx->output_name) : Dims{};
        format_dims(dimbuf, sizeof(dimbuf), out_dims);
        snprintf(errbuf, errbuf_size,
                 "invalid YOLOv8 output shape after setInputShape (got %s); "
                 "yakhyo yolov8n-face.onnx uses 3-head DFL format and is auto-detected",
                 dimbuf);
        delete ctx;
        return -1;
    }

    *trt = ctx;
    return 0;
}

extern "C" int ff_detect_trt_get_info(const FFDetectTRTContext *trt,
                                       FFDetectTRTModelInfo *info)
{
    if (!trt || !info)
        return -1;
    *info = trt->info;
    info->model_type = trt->model_type;
    return 0;
}

extern "C" int ff_detect_trt_execute(FFDetectTRTContext *trt,
                                      const FFDetectTRTExecuteParams *params,
                                      char *errbuf, int errbuf_size)
{
    if (!trt || !trt->context || !params) {
        set_error(errbuf, errbuf_size, "TensorRT context is not initialized");
        return -1;
    }

    if (trt->input_name) {
        Dims in_dims = trt->engine->getTensorShape(trt->input_name);
        if (in_dims.nbDims == 4) {
            in_dims.d[0] = 1;
            in_dims.d[1] = 3;
            in_dims.d[2] = trt->input_h;
            in_dims.d[3] = trt->input_w;
            if (!trt->context->setInputShape(trt->input_name, in_dims)) {
                set_error(errbuf, errbuf_size, "setInputShape failed before inference");
                return -1;
            }
        }
    }

    trt->context->setTensorAddress(trt->input_name, (void *)params->input);

    switch (trt->model_type) {
    case DETECT_MODEL_YOLOV8:
        if (!params->raw_output) {
            set_error(errbuf, errbuf_size, "missing raw output buffer");
            return -1;
        }
        trt->context->setTensorAddress(trt->output_name, (void *)params->raw_output);
        break;
    case DETECT_MODEL_YOLOV8_FACE:
        if (!params->num_raw_outputs || params->num_raw_outputs != trt->num_face_heads) {
            set_error(errbuf, errbuf_size, "missing YOLOv8-Face head output buffers");
            return -1;
        }
        for (int i = 0; i < trt->num_face_heads; i++) {
            if (!params->raw_outputs[i]) {
                set_error(errbuf, errbuf_size, "missing YOLOv8-Face head output buffer");
                return -1;
            }
            trt->context->setTensorAddress(trt->face_head_names[i],
                                            (void *)params->raw_outputs[i]);
        }
        break;
    default:
        set_error(errbuf, errbuf_size, "unsupported model type");
        return -1;
    }

    {
        int nb = trt->engine->getNbIOTensors();
        int i;

        for (i = 0; i < nb; i++) {
            const char *name = trt->engine->getIOTensorName(i);
            if (name && !trt->context->getTensorAddress(name)) {
                set_error(errbuf, errbuf_size, "TensorRT tensor address not set");
                return -1;
            }
        }
    }

    if (!trt->context->enqueueV3((cudaStream_t)params->stream)) {
        set_error(errbuf, errbuf_size, trt->logger.last.empty() ? "enqueueV3 failed"
                                                               : trt->logger.last.c_str());
        return -1;
    }

    return 0;
}

extern "C" void ff_detect_trt_destroy(FFDetectTRTContext **trt)
{
    if (trt && *trt) {
        delete *trt;
        *trt = nullptr;
    }
}
