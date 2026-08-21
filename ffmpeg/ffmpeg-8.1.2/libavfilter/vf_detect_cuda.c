/*
 * CUDA/TensorRT object detection filter (detect_cuda).
 *
 * Detect persons and/or faces on CUDA hwframes, optionally draw boxes, and
 * attach AV_FRAME_DATA_REGIONS_OF_INTEREST side data for NVENC/x264 ROI QP.
 *
 * Workflow (per frame):
 *
 *   [CUDA frame in]  NV12/P010/YUV420P, arbitrary resolution (e.g. 4K)
 *         |
 *         v
 *   copy_cuda_frame()          full-resolution passthrough buffer
 *         |
 *         v
 *   launch_preproc()           YUV -> RGB letterbox -> input_dev [1,3,S,S]
 *         |                      S = input_size (default 640); cost is O(S^2),
 *         |                      independent of source width/height
 *         v
 *   ff_detect_trt_execute()      TensorRT inference (vf_detect_cuda_trt.cpp)
 *         |
 *         +-- YOLOv8 (person)    GPU detect_yolov8_decode -> boxes_dev
 *         +-- YOLOv8-Face        CPU DFL decode (3-head yakhyo format)
 *         |
 *         v
 *   CPU NMS + filter_targets()   merge person/face, apply iou threshold
 *         |
 *         +-- draw=1             GPU box overlay on output frame
 *         +-- always             write_roi_side_data() with qoffset per class
 *         |
 *         v
 *   [CUDA frame out]  same resolution as input + ROI side data
 *
 * Dual-engine mode (targets=person+face):
 *   engine=yolov8n.onnx + face_engine=yolov8n-face.onnx share one preproc/TRT
 *   input buffer; results are merged before NMS.
 *
 * stride=N skips TRT on intermediate frames and reuses last host_boxes; when
 * last_count==0 inference is forced so empty scenes recover quickly.
 *
 * Related: vf_detect_cuda.cu (CUDA kernels), vf_detect_cuda_trt.cpp (TRT build/exec).
 * TRT must run inside FFmpeg's cuCtx (see create_trt_context / filter_frame).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/dict.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "libavutil/internal.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

#include "cuda/load_helper.h"
#include "vf_detect_cuda_trt.h"

#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCKX 16
#define BLOCKY 16
#define DETECT_ERR_SIZE 512

#define DETECT_FMT_NV12     0
#define DETECT_FMT_P010     1
#define DETECT_FMT_YUV420P  2

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)

/* ------------------------------------------------------------------------- */
/* Types and filter private context                                          */
/* ------------------------------------------------------------------------- */

typedef struct DetectBox {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
    int class_id;
} DetectBox;

typedef struct CUDADetectContext {
    const AVClass *class;

    /* CUDA device / kernels (PTX module loaded at init) */
    AVCUDADeviceContext *hwctx;
    CUmodule cu_module;
    CUfunction cu_func_preproc;
    CUfunction cu_func_yolo_decode;
    CUfunction cu_func_draw_y;
    CUfunction cu_func_draw_uv;
    CUfunction cu_func_bilateral_y;
    CUfunction cu_func_bilateral_uv;
    CUstream cu_stream;

    /* Input sw_format from hw_frames_ctx */
    enum AVPixelFormat sw_format;
    const AVPixFmtDescriptor *desc;
    int detect_format;
    int bit_depth;

    AVBufferRef *frames_ctx;

    /* User options (AVOption) */
    char *engine;
    char *face_engine;
    char *labels;
    char *targets;
    char *dump_path;
    FILE *dump_file;
    DetectModelType model_type;
    DetectModelType face_model_type;
    float score;
    float iou;
    int max_det;
    int input_size;
    int fp16;
    int draw;
    int thickness;
    int face_qoffset;    /* ROI QP delta numerator; actual = value/100 */
    int person_qoffset;
    int face_class_id;   /* remapped class id written to ROI metadata */
    int person_class_id;
    int person_filter_class; /* YOLOv8 COCO class 0 when detecting person */
    int stride;
    int pool_size;
    int want_person;
    int want_face;
    int bilateral; /* 0=off, 1/2/3=weak/medium/strong face beauty */

    /* Per-frame detection state (cached across stride skips) */
    int frame_index;
    int last_count;
    DetectBox *host_boxes;

    /* GPU buffers */
    CUdeviceptr input_dev;       /* TRT input: 3*input_size^2 float RGB */
    CUdeviceptr raw_output_dev;  /* YOLOv8 raw tensor output */
    CUdeviceptr boxes_dev;
    CUdeviceptr count_dev;
    CUdeviceptr matrix_dev;      /* YUV->RGB 3x3 + bias for preproc kernel */
    size_t raw_output_bytes;

    /* YOLOv8-Face 3-head DFL: GPU staging + CPU decode buffer */
    CUdeviceptr yolov8_face_dev[FF_DETECT_FACE_HEADS];
    float *yolov8_face_host;
    size_t yolov8_face_host_bytes;

    /* TensorRT engines (dlopen'd libavfilter_detect_cuda_trt.so) */
    FFDetectTRTContext *trt;
    FFDetectTRTContext *face_trt;
    FFDetectTRTModelInfo model_info;
    FFDetectTRTModelInfo face_model_info;
} CUDADetectContext;

/* ------------------------------------------------------------------------- */
/* Pixel format queries and option parsing (targets / labels)                */
/* ------------------------------------------------------------------------- */

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_YUV420P,
};

static int format_is_supported(enum AVPixelFormat fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

static int detect_format_code(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:    return DETECT_FMT_NV12;
    case AV_PIX_FMT_P010:    return DETECT_FMT_P010;
    case AV_PIX_FMT_YUV420P: return DETECT_FMT_YUV420P;
    default:                 return -1;
    }
}

static int parse_labels(AVFilterContext *ctx, CUDADetectContext *s)
{
    const char *l = s->labels;

    s->person_class_id = 0;
    s->face_class_id = 1;
    s->person_filter_class = 0;

    if (!l || !l[0])
        return 0;

    if (strchr(l, ',')) {
        av_log(ctx, AV_LOG_ERROR,
               "invalid labels '%s': use + separator (person+face), not comma\n", l);
        return AVERROR(EINVAL);
    }

    if (!av_strcasecmp(l, "person")) {
        s->person_class_id = 0;
        s->face_class_id = 1;
    } else if (!av_strcasecmp(l, "face")) {
        s->person_class_id = 1;
        s->face_class_id = 0;
    } else if (!av_strcasecmp(l, "person+face")) {
        s->person_class_id = 0;
        s->face_class_id = 1;
    } else if (!av_strcasecmp(l, "face+person")) {
        s->person_class_id = 1;
        s->face_class_id = 0;
    } else {
        av_log(ctx, AV_LOG_ERROR,
               "invalid labels '%s'; allowed: person, face, person+face\n", l);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int parse_targets(AVFilterContext *ctx, CUDADetectContext *s)
{
    const char *t = s->targets;

    s->want_person = 0;
    s->want_face = 0;

    if (!t || !t[0]) {
        av_log(ctx, AV_LOG_ERROR,
               "targets is required; allowed: person, face, person+face\n");
        return AVERROR(EINVAL);
    }

    if (strchr(t, ',')) {
        av_log(ctx, AV_LOG_ERROR,
               "invalid targets '%s': use + separator (person+face), not comma\n",
               t);
        return AVERROR(EINVAL);
    }

    if (!av_strcasecmp(t, "person")) {
        s->want_person = 1;
    } else if (!av_strcasecmp(t, "face")) {
        s->want_face = 1;
    } else if (!av_strcasecmp(t, "person+face") || !av_strcasecmp(t, "face+person")) {
        s->want_person = 1;
        s->want_face = 1;
    } else {
        av_log(ctx, AV_LOG_ERROR,
               "invalid targets '%s'; allowed: person, face, person+face\n", t);
        return AVERROR(EINVAL);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Host-side NMS, YOLOv8-Face DFL decode, target filtering                 */
/* ------------------------------------------------------------------------- */

static float detect_box_iou(const DetectBox *a, const DetectBox *b)
{
    float inter_x1 = FFMAX(a->x1, b->x1);
    float inter_y1 = FFMAX(a->y1, b->y1);
    float inter_x2 = FFMIN(a->x2, b->x2);
    float inter_y2 = FFMIN(a->y2, b->y2);
    float inter_w = FFMAX(0.0f, inter_x2 - inter_x1);
    float inter_h = FFMAX(0.0f, inter_y2 - inter_y1);
    float inter = inter_w * inter_h;
    float area_a = FFMAX(0.0f, a->x2 - a->x1) * FFMAX(0.0f, a->y2 - a->y1);
    float area_b = FFMAX(0.0f, b->x2 - b->x1) * FFMAX(0.0f, b->y2 - b->y1);
    float uni = area_a + area_b - inter;

    return uni > 0.0f ? inter / uni : 0.0f;
}

static int detect_apply_nms(DetectBox *boxes, int count, float iou_threshold)
{
    int i, j, out = 0;

    for (i = 0; i < count; i++) {
        int keep = 1;

        for (j = 0; j < out; j++) {
            if (boxes[j].class_id != boxes[i].class_id)
                continue;
            if (detect_box_iou(&boxes[j], &boxes[i]) > iou_threshold) {
                keep = 0;
                break;
            }
        }

        if (keep)
            boxes[out++] = boxes[i];
    }

    return out;
}

static float detect_sigmoid(float x)
{
    if (x >= 0.f)
        return 1.f / (1.f + expf(-x));
    {
        float e = expf(x);
        return e / (1.f + e);
    }
}

static float detect_dfl_side(const float logits[16])
{
    float maxv = logits[0];
    int i;

    for (i = 1; i < 16; i++)
        maxv = fmaxf(maxv, logits[i]);

    {
        float sum = 0.f, acc = 0.f;
        for (i = 0; i < 16; i++) {
            float e = expf(logits[i] - maxv);
            sum += e;
            acc += e * (float)i;
        }
        return sum > 0.f ? acc / sum : 0.f;
    }
}

static int decode_yolov8_face_head(const float *pred, int channels, int grid,
                                   int stride, float ratio, float pad_x, float pad_y,
                                   int src_w, int src_h, float score_threshold,
                                   DetectBox *boxes, int *count, int max_det,
                                   int output_class_id)
{
    int x, y, plane = grid * grid;

    for (y = 0; y < grid; y++) {
        for (x = 0; x < grid; x++) {
            int i = y * grid + x;
            float grid_x = (float)x + 0.5f;
            float grid_y = (float)y + 0.5f;
            float score = detect_sigmoid(pred[64 * plane + i]);
            float dist[4];
            int side, slot;

            if (score < score_threshold)
                continue;

            for (side = 0; side < 4; side++) {
                float logits[16];
                int k;

                for (k = 0; k < 16; k++)
                    logits[k] = pred[(side * 16 + k) * plane + i];
                dist[side] = detect_dfl_side(logits);
            }

            /* Cap candidates; not a real OOM (old code returned ENOMEM and
             * broke small max_det such as 3). */
            if (*count >= max_det)
                return 0;

            slot = (*count)++;
            boxes[slot].x1 = ((grid_x - dist[0]) * (float)stride - pad_x) / ratio;
            boxes[slot].y1 = ((grid_y - dist[1]) * (float)stride - pad_y) / ratio;
            boxes[slot].x2 = ((grid_x + dist[2]) * (float)stride - pad_x) / ratio;
            boxes[slot].y2 = ((grid_y + dist[3]) * (float)stride - pad_y) / ratio;
            boxes[slot].x1 = av_clipf(boxes[slot].x1, 0.f, (float)(src_w - 1));
            boxes[slot].y1 = av_clipf(boxes[slot].y1, 0.f, (float)(src_h - 1));
            boxes[slot].x2 = av_clipf(boxes[slot].x2, boxes[slot].x1 + 1.f, (float)src_w);
            boxes[slot].y2 = av_clipf(boxes[slot].y2, boxes[slot].y1 + 1.f, (float)src_h);
            boxes[slot].score = score;
            boxes[slot].class_id = output_class_id;
        }
    }

    (void)channels;
    return 0;
}

static int box_cmp_roi_priority(const void *a, const void *b)
{
    const DetectBox *ba = a;
    const DetectBox *bb = b;

    if (ba->class_id != bb->class_id)
        return bb->class_id - ba->class_id;
    return (bb->score > ba->score) - (bb->score < ba->score);
}

static int decode_yolov8_face_host(AVFilterContext *ctx, AVFrame *frame,
                                   const FFDetectTRTModelInfo *info,
                                   float ratio, float pad_x, float pad_y,
                                   int output_class_id)
{
    CUDADetectContext *s = ctx->priv;
    DetectBox *candidates = NULL;
    int ret, h, count = 0;

    candidates = av_malloc_array(s->max_det * 4, sizeof(*candidates));
    if (!candidates)
        return AVERROR(ENOMEM);

    for (h = 0; h < info->num_face_heads; h++) {
        const FFDetectTRTFaceHeadInfo *head = &info->face_heads[h];
        size_t offset = 0;
        int hi;

        for (hi = 0; hi < h; hi++)
            offset += info->face_heads[hi].bytes;

        ret = decode_yolov8_face_head((const float *)((uint8_t *)s->yolov8_face_host + offset),
                                      head->channels, head->grid_size, head->stride,
                                      ratio, pad_x, pad_y, frame->width, frame->height,
                                      s->score, candidates, &count, s->max_det * 4,
                                      output_class_id);
        if (ret < 0)
            goto done;
    }

    count = detect_apply_nms(candidates, count, s->iou);
    /* Keep highest-score boxes when capping to max_det. */
    if (count > 1)
        qsort(candidates, count, sizeof(*candidates), box_cmp_roi_priority);
    if (count > s->max_det)
        count = s->max_det;

    s->last_count = count;
    if (count > 0)
        memcpy(s->host_boxes, candidates, (size_t)count * sizeof(*candidates));
    ret = 0;

done:
    av_free(candidates);
    return ret;
}

static int filter_targets(AVFilterContext *ctx, DetectBox *boxes, int count)
{
    CUDADetectContext *s = ctx->priv;
    int i, out = 0;

    for (i = 0; i < count; i++) {
        DetectBox *b = &boxes[i];
        if (b->class_id == s->person_class_id && s->want_person)
            boxes[out++] = *b;
        else if (b->class_id == s->face_class_id && s->want_face)
            boxes[out++] = *b;
    }

    return out;
}

/* ------------------------------------------------------------------------- */
/* Cold init: hw_frames pool, PTX module, GPU buffer allocation              */
/* ------------------------------------------------------------------------- */

static av_cold int init_hwframe_ctx(CUDADetectContext *s,
                                    AVBufferRef *device_ctx,
                                    enum AVPixelFormat sw_format,
                                    int width, int height)
{
    AVBufferRef *out_ref = NULL;
    AVHWFramesContext *out_ctx;
    int ret;

    out_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!out_ref)
        return AVERROR(ENOMEM);

    out_ctx = (AVHWFramesContext *)out_ref->data;
    out_ctx->format = AV_PIX_FMT_CUDA;
    out_ctx->sw_format = sw_format;
    out_ctx->width = FFALIGN(width, 32);
    out_ctx->height = FFALIGN(height, 32);
    out_ctx->initial_pool_size = s->pool_size;

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0)
        goto fail;

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = out_ref;
    return 0;

fail:
    av_buffer_unref(&out_ref);
    return ret;
}

static av_cold int detect_cuda_load_functions(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_detect_cuda_ptx_data[];
    extern const unsigned int ff_vf_detect_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_detect_cuda_ptx_data,
                              ff_vf_detect_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_preproc, s->cu_module,
                                           "detect_preproc_420"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_yolo_decode, s->cu_module,
                                           "detect_yolov8_decode"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_draw_y, s->cu_module,
                                           "detect_draw_y"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_draw_uv, s->cu_module,
                                           "detect_draw_uv"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_bilateral_y, s->cu_module,
                                           "detect_bilateral_face_y"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_bilateral_uv, s->cu_module,
                                           "detect_bilateral_face_uv"));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold int alloc_gpu_buffers(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;
    size_t input_size = (size_t)3 * s->input_size * s->input_size * sizeof(float);

    s->host_boxes = av_calloc(s->max_det, sizeof(*s->host_boxes));
    if (!s->host_boxes)
        return AVERROR(ENOMEM);

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = CHECK_CU(cu->cuMemAlloc(&s->input_dev, input_size));
    if (ret < 0) goto fail;
    if (s->raw_output_bytes > 0) {
        ret = CHECK_CU(cu->cuMemAlloc(&s->raw_output_dev, s->raw_output_bytes));
        if (ret < 0) goto fail;
    }

    {
        int i;
        size_t host_bytes = 0;

        for (i = 0; i < FF_DETECT_FACE_HEADS; i++) {
            size_t bytes = 0;

            if (s->model_info.model_type == DETECT_MODEL_YOLOV8_FACE)
                bytes = FFMAX(bytes, s->model_info.face_heads[i].bytes);
            if (s->face_model_info.model_type == DETECT_MODEL_YOLOV8_FACE)
                bytes = FFMAX(bytes, s->face_model_info.face_heads[i].bytes);
            if (bytes > 0) {
                ret = CHECK_CU(cu->cuMemAlloc(&s->yolov8_face_dev[i], bytes));
                if (ret < 0) goto fail;
                host_bytes += bytes;
            }
        }

        if (host_bytes > 0) {
            s->yolov8_face_host = av_malloc(host_bytes);
            if (!s->yolov8_face_host) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            s->yolov8_face_host_bytes = host_bytes;
        }
    }

    ret = CHECK_CU(cu->cuMemAlloc(&s->boxes_dev, (size_t)s->max_det * sizeof(DetectBox)));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuMemAlloc(&s->count_dev, sizeof(int)));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuMemAlloc(&s->matrix_dev, 12 * sizeof(float)));
    if (ret < 0) goto fail;

    ret = CHECK_CU(cu->cuMemsetD8Async(s->count_dev, 0, sizeof(int), s->cu_stream));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static void compute_yuv_matrix(const AVFrame *frame, int bit_depth,
                               float coeffs[12])
{
    float kr = 0.299f, kb = 0.114f;
    float kg, cy, y_bias, cuv, c_mid, rv, bu, gu, gv;
    int limited = frame->color_range != AVCOL_RANGE_JPEG;

    if (frame->colorspace == AVCOL_SPC_BT709) {
        kr = 0.2126f;
        kb = 0.0722f;
    } else if (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
               frame->colorspace == AVCOL_SPC_BT2020_CL) {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    kg = 1.0f - kr - kb;

    if (limited) {
        float max_code = (float)((1 << bit_depth) - 1);
        float y_min = (float)(16 << (bit_depth - 8)) / max_code;
        float y_max = (float)(235 << (bit_depth - 8)) / max_code;
        c_mid = (float)(128 << (bit_depth - 8)) / max_code;
        cy = 1.0f / (y_max - y_min);
        y_bias = -y_min * cy;
        cuv = 1.0f / (((float)(240 << (bit_depth - 8)) / max_code) - c_mid);
    } else {
        cy = 1.0f;
        y_bias = 0.0f;
        c_mid = 0.5f;
        cuv = 2.0f;
    }

    rv = 2.0f * (1.0f - kr) * cuv;
    bu = 2.0f * (1.0f - kb) * cuv;
    gu = -2.0f * kb * (1.0f - kb) / kg * cuv;
    gv = -2.0f * kr * (1.0f - kr) / kg * cuv;

    coeffs[0] = cy; coeffs[1] = 0.0f; coeffs[2] = rv; coeffs[9]  = y_bias - rv * c_mid;
    coeffs[3] = cy; coeffs[4] = gu;   coeffs[5] = gv; coeffs[10] = y_bias - (gu + gv) * c_mid;
    coeffs[6] = cy; coeffs[7] = bu;   coeffs[8] = 0.0f; coeffs[11] = y_bias - bu * c_mid;
}

/* ------------------------------------------------------------------------- */
/* GPU frame helpers: copy, letterbox preproc, decode, draw                  */
/* ------------------------------------------------------------------------- */

static int detect_cuda_copy_htod(CudaFunctions *cu, CUdeviceptr dst,
                                 const void *src, size_t size, CUstream stream)
{
    CUDA_MEMCPY2D cpy = { 0 };

    if (size == 0)
        return CUDA_SUCCESS;

    cpy.srcMemoryType = CU_MEMORYTYPE_HOST;
    cpy.srcHost = src;
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = dst;
    cpy.dstPitch = size;
    cpy.WidthInBytes = size;
    cpy.Height = 1;

    return cu->cuMemcpy2DAsync(&cpy, stream);
}

static int detect_cuda_copy_dtoh(CudaFunctions *cu, void *dst,
                                 CUdeviceptr src, size_t size, CUstream stream)
{
    CUDA_MEMCPY2D cpy = { 0 };

    if (size == 0)
        return CUDA_SUCCESS;

    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = src;
    cpy.srcPitch = size;
    cpy.dstMemoryType = CU_MEMORYTYPE_HOST;
    cpy.dstHost = dst;
    cpy.dstPitch = size;
    cpy.WidthInBytes = size;
    cpy.Height = 1;

    return cu->cuMemcpy2DAsync(&cpy, stream);
}

static int copy_cuda_frame(AVFilterContext *ctx, AVFrame *dst, const AVFrame *src)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int planes = av_pix_fmt_count_planes(s->sw_format);
    int p, ret = 0;

    for (p = 0; p < planes; p++) {
        CUDA_MEMCPY2D cpy = { 0 };
        int h = p ? AV_CEIL_RSHIFT(src->height, s->desc->log2_chroma_h) : src->height;
        int w = p ? AV_CEIL_RSHIFT(src->width, s->desc->log2_chroma_w) : src->width;
        int bytes = (s->bit_depth > 8) ? 2 : 1;
        if (s->sw_format == AV_PIX_FMT_NV12 || s->sw_format == AV_PIX_FMT_P010)
            bytes = p ? 2 * bytes : bytes;

        cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.srcDevice = (CUdeviceptr)src->data[p];
        cpy.srcPitch = src->linesize[p];
        cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.dstDevice = (CUdeviceptr)dst->data[p];
        cpy.dstPitch = dst->linesize[p];
        cpy.WidthInBytes = (size_t)w * bytes;
        cpy.Height = h;

        ret = CHECK_CU(cu->cuMemcpy2DAsync(&cpy, s->cu_stream));
        if (ret < 0)
            return ret;
    }
    return ret;
}

static int launch_preproc(AVFilterContext *ctx, AVFrame *frame,
                          float ratio, float pad_x, float pad_y)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    float coeffs[12];
    CUdeviceptr matrix_dev = s->matrix_dev;
    CUdeviceptr bias_dev = s->matrix_dev + 9 * sizeof(float);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr u = (CUdeviceptr)frame->data[1];
    CUdeviceptr v = s->sw_format == AV_PIX_FMT_YUV420P ? (CUdeviceptr)frame->data[2] : 0;
    int pitch_y = frame->linesize[0];
    int pitch_u = frame->linesize[1];
    int pitch_v = s->sw_format == AV_PIX_FMT_YUV420P ? frame->linesize[2] : 0;
    void *args[] = {
        &y, &u, &v, &pitch_y, &pitch_u, &pitch_v, &frame->width, &frame->height,
        &s->detect_format, &s->bit_depth, &s->input_dev, &s->input_size,
        &s->input_size, &ratio, &pad_x, &pad_y, &matrix_dev, &bias_dev,
    };
    int ret;

    compute_yuv_matrix(frame, s->bit_depth, coeffs);
    ret = CHECK_CU(detect_cuda_copy_htod(cu, s->matrix_dev, coeffs,
                                         sizeof(coeffs), s->cu_stream));
    if (ret < 0)
        return ret;

    return CHECK_CU(cu->cuLaunchKernel(s->cu_func_preproc,
                                       DIV_UP(s->input_size, BLOCKX),
                                       DIV_UP(s->input_size, BLOCKY),
                                       1, BLOCKX, BLOCKY, 1, 0,
                                       s->cu_stream, args, NULL));
}

static int sync_preproc(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;

    return CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
}

static int launch_yolo_decode(AVFilterContext *ctx, AVFrame *frame,
                              const FFDetectTRTModelInfo *info,
                              float ratio, float pad_x, float pad_y,
                              int filter_class_id, int output_class_id)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int threads = 256;
    int num_classes = info->num_classes;
    int num_anchors = info->num_anchors;
    int transposed = info->yolo_transposed;
    void *args[] = {
        &s->raw_output_dev, &num_classes, &num_anchors, &transposed,
        &s->boxes_dev, &s->count_dev, &s->max_det, &s->score,
        &filter_class_id, &output_class_id,
        &ratio, &pad_x, &pad_y, &frame->width, &frame->height,
    };

    if (num_anchors <= 0 || num_classes <= 0 || !s->raw_output_dev)
        return AVERROR(EINVAL);

    return CHECK_CU(cu->cuLaunchKernel(s->cu_func_yolo_decode,
                                       DIV_UP(num_anchors, threads), 1, 1,
                                       threads, 1, 1, 0, s->cu_stream, args, NULL));
}

/* Face-ROI bilateral beauty presets: sigmaS/sigmaR in pixel / 0-255 units. */
static const struct {
    float sigmaS;
    float sigmaR;
    int window_size;
} detect_bilateral_presets[4] = {
    { 0.f,  0.f,  0 },
    { 3.f, 12.f,  5 }, /* weak */
    { 6.f, 25.f,  7 }, /* medium */
    { 10.f, 40.f, 9 }, /* strong */
};

static int launch_bilateral_faces(AVFilterContext *ctx, AVFrame *src, AVFrame *dst)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int level = s->bilateral;
    float sigmaS, sigmaR;
    int window_size;
    int uv_w = AV_CEIL_RSHIFT(dst->width, s->desc->log2_chroma_w);
    int uv_h = AV_CEIL_RSHIFT(dst->height, s->desc->log2_chroma_h);
    CUdeviceptr src_y = (CUdeviceptr)src->data[0];
    CUdeviceptr src_u = (CUdeviceptr)src->data[1];
    CUdeviceptr src_v = s->sw_format == AV_PIX_FMT_YUV420P ? (CUdeviceptr)src->data[2] : 0;
    CUdeviceptr dst_y = (CUdeviceptr)dst->data[0];
    CUdeviceptr dst_u = (CUdeviceptr)dst->data[1];
    CUdeviceptr dst_v = s->sw_format == AV_PIX_FMT_YUV420P ? (CUdeviceptr)dst->data[2] : 0;
    int pitch_src_y = src->linesize[0];
    int pitch_src_u = src->linesize[1];
    int pitch_src_v = s->sw_format == AV_PIX_FMT_YUV420P ? src->linesize[2] : 0;
    int pitch_dst_y = dst->linesize[0];
    int pitch_dst_u = dst->linesize[1];
    int pitch_dst_v = s->sw_format == AV_PIX_FMT_YUV420P ? dst->linesize[2] : 0;
    void *args_y[] = {
        &src_y, &dst_y, &pitch_src_y, &pitch_dst_y, &dst->width, &dst->height,
        &s->detect_format, &s->bit_depth, &s->boxes_dev, &s->count_dev,
        &s->max_det, &s->face_class_id, &window_size, &sigmaS, &sigmaR,
    };
    void *args_uv[] = {
        &src_y, &src_u, &src_v, &dst_u, &dst_v,
        &pitch_src_y, &pitch_src_u, &pitch_src_v, &pitch_dst_u, &pitch_dst_v,
        &dst->width, &dst->height, &s->detect_format, &s->bit_depth,
        &s->boxes_dev, &s->count_dev, &s->max_det, &s->face_class_id,
        &window_size, &sigmaS, &sigmaR,
    };
    int ret;

    if (level <= 0 || !s->want_face || s->last_count <= 0)
        return 0;
    if (level > 3)
        level = 3;

    sigmaS = detect_bilateral_presets[level].sigmaS;
    sigmaR = detect_bilateral_presets[level].sigmaR;
    window_size = detect_bilateral_presets[level].window_size | 1;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_bilateral_y,
                                      DIV_UP(dst->width, BLOCKX),
                                      DIV_UP(dst->height, BLOCKY),
                                      1, BLOCKX, BLOCKY, 1, 0,
                                      s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;

    return CHECK_CU(cu->cuLaunchKernel(s->cu_func_bilateral_uv,
                                       DIV_UP(uv_w, BLOCKX),
                                       DIV_UP(uv_h, BLOCKY),
                                       1, BLOCKX, BLOCKY, 1, 0,
                                       s->cu_stream, args_uv, NULL));
}

static int launch_draw(AVFilterContext *ctx, AVFrame *frame)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int uv_w = AV_CEIL_RSHIFT(frame->width, s->desc->log2_chroma_w);
    int uv_h = AV_CEIL_RSHIFT(frame->height, s->desc->log2_chroma_h);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr u = (CUdeviceptr)frame->data[1];
    CUdeviceptr v = s->sw_format == AV_PIX_FMT_YUV420P ? (CUdeviceptr)frame->data[2] : 0;
    int pitch_y = frame->linesize[0];
    int pitch_u = frame->linesize[1];
    int pitch_v = s->sw_format == AV_PIX_FMT_YUV420P ? frame->linesize[2] : 0;
    void *args_y[] = {
        &y, &pitch_y, &frame->width, &frame->height, &s->detect_format,
        &s->bit_depth, &s->boxes_dev, &s->count_dev, &s->max_det, &s->thickness,
        &s->face_class_id,
    };
    void *args_uv[] = {
        &u, &v, &pitch_u, &pitch_v, &frame->width, &frame->height,
        &s->detect_format, &s->bit_depth, &s->boxes_dev, &s->count_dev,
        &s->max_det, &s->thickness, &s->face_class_id,
    };
    int ret;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_draw_y,
                                      DIV_UP(frame->width, BLOCKX),
                                      DIV_UP(frame->height, BLOCKY),
                                      1, BLOCKX, BLOCKY, 1, 0,
                                      s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;

    return CHECK_CU(cu->cuLaunchKernel(s->cu_func_draw_uv,
                                       DIV_UP(uv_w, BLOCKX),
                                       DIV_UP(uv_h, BLOCKY),
                                       1, BLOCKX, BLOCKY, 1, 0,
                                       s->cu_stream, args_uv, NULL));
}

static int sync_boxes_to_gpu(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int ret;

    ret = CHECK_CU(detect_cuda_copy_htod(cu, s->count_dev, &s->last_count,
                                       sizeof(s->last_count), s->cu_stream));
    if (ret < 0)
        return ret;

    if (s->last_count <= 0)
        return 0;

    return CHECK_CU(detect_cuda_copy_htod(cu, s->boxes_dev, s->host_boxes,
                                          (size_t)s->last_count * sizeof(*s->host_boxes),
                                          s->cu_stream));
}

/* Read decoded boxes from GPU, filter by targets, optional host NMS */
static int read_boxes_to_host(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int ret;

    ret = CHECK_CU(detect_cuda_copy_dtoh(cu, &s->last_count, s->count_dev,
                                       sizeof(s->last_count), s->cu_stream));
    if (ret < 0)
        return ret;
    ret = CHECK_CU(detect_cuda_copy_dtoh(cu, s->host_boxes, s->boxes_dev,
                                       (size_t)s->max_det * sizeof(*s->host_boxes),
                                       s->cu_stream));
    if (ret < 0)
        return ret;
    ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
    if (ret < 0)
        return ret;

    s->last_count = av_clip(s->last_count, 0, s->max_det);
    s->last_count = filter_targets(ctx, s->host_boxes, s->last_count);
    qsort(s->host_boxes, s->last_count, sizeof(*s->host_boxes), box_cmp_roi_priority);
    if (s->iou > 0.0f && s->last_count > 1)
        s->last_count = detect_apply_nms(s->host_boxes, s->last_count, s->iou);
    /* Keep GPU count/boxes aligned with host (draw reads count_dev). */
    return sync_boxes_to_gpu(ctx);
}

/* Attach AVRegionOfInterest side data; qoffset drives NVENC roi_qp_map delta */
static int write_roi_side_data(AVFilterContext *ctx, AVFrame *frame)
{
    CUDADetectContext *s = ctx->priv;
    AVFrameSideData *sd;
    AVRegionOfInterest *roi;
    char key[32], value[160];
    const char *label_name;
    int i;

    av_frame_remove_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (s->last_count <= 0)
        return 0;

    sd = av_frame_new_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST,
                                (size_t)s->last_count * sizeof(*roi));
    if (!sd)
        return AVERROR(ENOMEM);

    roi = (AVRegionOfInterest *)sd->data;
    for (i = 0; i < s->last_count; i++) {
        DetectBox *b = &s->host_boxes[i];
        roi[i].self_size = sizeof(*roi);
        roi[i].left   = av_clip((int)(b->x1 + 0.5f), 0, frame->width - 1);
        roi[i].top    = av_clip((int)(b->y1 + 0.5f), 0, frame->height - 1);
        roi[i].right  = av_clip((int)(b->x2 + 0.5f), roi[i].left + 1, frame->width);
        roi[i].bottom = av_clip((int)(b->y2 + 0.5f), roi[i].top + 1, frame->height);
        if (b->class_id == s->face_class_id) {
            roi[i].qoffset = av_make_q(s->face_qoffset, 100);
            label_name = "face";
        } else {
            roi[i].qoffset = av_make_q(s->person_qoffset, 100);
            label_name = "person";
        }

        snprintf(key, sizeof(key), "det_%d", i);
        snprintf(value, sizeof(value),
                 "label=%s,class=%d,score=%.4f,box=%d,%d,%d,%d",
                 label_name, b->class_id, b->score,
                 roi[i].left, roi[i].top, roi[i].right, roi[i].bottom);
        av_dict_set(&sd->metadata, key, value, 0);
    }
    av_dict_set_int(&sd->metadata, "count", s->last_count, 0);
    return 0;
}

static void dump_boxes(CUDADetectContext *s, const AVFrame *frame)
{
    int i;

    if (!s->dump_file)
        return;

    fprintf(s->dump_file, "{\"pts\":%"PRId64",\"frame\":%d,\"dets\":[",
            frame->pts, s->frame_index);
    for (i = 0; i < s->last_count; i++) {
        DetectBox *b = &s->host_boxes[i];
        const char *label = b->class_id == s->face_class_id ? "face" : "person";
        fprintf(s->dump_file,
                "%s{\"label\":\"%s\",\"class\":%d,\"score\":%.4f,\"box\":[%.1f,%.1f,%.1f,%.1f]}",
                i ? "," : "", label, b->class_id, b->score,
                b->x1, b->y1, b->x2, b->y2);
    }
    fprintf(s->dump_file, "]}\n");
    fflush(s->dump_file);
}

/* ------------------------------------------------------------------------- */
/* TensorRT enqueue + model-specific decode dispatch                           */
/* ------------------------------------------------------------------------- */

static int run_trt_engine(AVFilterContext *ctx, AVFrame *frame,
                          FFDetectTRTContext *trt,
                          const FFDetectTRTModelInfo *info,
                          float ratio, float pad_x, float pad_y,
                          int yolo_filter_class, int output_class_id,
                          int reset_count)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    FFDetectTRTExecuteParams params = { 0 };
    char errbuf[DETECT_ERR_SIZE] = { 0 };
    int ret;

    params.input  = s->input_dev;
    params.stream = s->cu_stream;

    if (reset_count) {
        ret = CHECK_CU(cu->cuMemsetD8Async(s->count_dev, 0, sizeof(int), s->cu_stream));
        if (ret < 0)
            return ret;
    }

    switch (info->model_type) {
    case DETECT_MODEL_YOLOV8:
        if (!s->raw_output_dev) {
            av_log(ctx, AV_LOG_ERROR, "raw TRT output buffer is not allocated\n");
            return AVERROR(EINVAL);
        }
        params.raw_output = s->raw_output_dev;
        break;
    case DETECT_MODEL_YOLOV8_FACE: {
        int i;

        if (!s->yolov8_face_host) {
            av_log(ctx, AV_LOG_ERROR, "YOLOv8-Face host buffer is not allocated\n");
            return AVERROR(EINVAL);
        }
        params.num_raw_outputs = info->num_face_heads;
        for (i = 0; i < info->num_face_heads; i++)
            params.raw_outputs[i] = s->yolov8_face_dev[i];
        break;
    }
    default:
        return AVERROR(EINVAL);
    }

    ret = ff_detect_trt_execute(trt, &params, errbuf, sizeof(errbuf));
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "TensorRT execution failed: %s\n", errbuf);
        return AVERROR_EXTERNAL;
    }

    switch (info->model_type) {
    case DETECT_MODEL_YOLOV8:
        return launch_yolo_decode(ctx, frame, info, ratio, pad_x, pad_y,
                                  yolo_filter_class, output_class_id);
    case DETECT_MODEL_YOLOV8_FACE: {
        int i;
        size_t offset = 0;

        ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
        if (ret < 0)
            return ret;

        for (i = 0; i < info->num_face_heads; i++) {
            ret = CHECK_CU(detect_cuda_copy_dtoh(cu,
                                                 (uint8_t *)s->yolov8_face_host + offset,
                                                 s->yolov8_face_dev[i],
                                                 info->face_heads[i].bytes,
                                                 s->cu_stream));
            if (ret < 0)
                return ret;
            offset += info->face_heads[i].bytes;
        }

        ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
        if (ret < 0)
            return ret;

        return decode_yolov8_face_host(ctx, frame, info, ratio, pad_x, pad_y,
                                       output_class_id);
    }
    default:
        return AVERROR(EINVAL);
    }
}

/*
 * Full detection pass for one frame:
 *   letterbox params -> preproc -> TRT (person and/or face) -> merge -> NMS
 *   -> optional face bilateral -> optional draw.
 * Box coordinates are mapped back to frame->width/height.
 */
static int run_detection(AVFilterContext *ctx, AVFrame *dst, AVFrame *src)
{
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    AVFrame *frame = dst;
    /* Letterbox: scale so the longer side fits input_size, center-pad to square */
    float ratio = FFMIN((float)s->input_size / frame->width,
                        (float)s->input_size / frame->height);
    float pad_x = (s->input_size - frame->width * ratio) * 0.5f;
    float pad_y = (s->input_size - frame->height * ratio) * 0.5f;
    DetectBox *merged = NULL;
    int ret, reset = 1;
    int total = 0;
    int gpu_decode = 0;

    ret = launch_preproc(ctx, frame, ratio, pad_x, pad_y);
    if (ret < 0)
        return ret;

    ret = sync_preproc(ctx);
    if (ret < 0)
        return ret;

    merged = av_malloc_array(s->max_det * 4, sizeof(*merged));
    if (!merged)
        return AVERROR(ENOMEM);

    if (s->want_person && s->trt &&
        s->model_info.model_type != DETECT_MODEL_YOLOV8_FACE) {
        ret = run_trt_engine(ctx, frame, s->trt, &s->model_info,
                             ratio, pad_x, pad_y,
                             s->person_filter_class, s->person_class_id, reset);
        if (ret < 0)
            goto fail;
        gpu_decode = 1;
        reset = 0;
    }

    if (s->want_face && s->face_trt &&
        s->face_model_info.model_type != DETECT_MODEL_YOLOV8_FACE) {
        ret = run_trt_engine(ctx, frame, s->face_trt, &s->face_model_info,
                             ratio, pad_x, pad_y, -1, s->face_class_id, reset);
        if (ret < 0)
            goto fail;
        gpu_decode = 1;
        reset = 0;
    }

    if (gpu_decode) {
        ret = read_boxes_to_host(ctx);
        if (ret < 0)
            goto fail;
        total = s->last_count;
        if (total > 0)
            memcpy(merged, s->host_boxes, (size_t)total * sizeof(*merged));
    }

    if (s->want_face && s->face_trt &&
        s->face_model_info.model_type == DETECT_MODEL_YOLOV8_FACE) {
        int face_count;

        ret = run_trt_engine(ctx, frame, s->face_trt, &s->face_model_info,
                             ratio, pad_x, pad_y, -1, s->face_class_id, reset);
        if (ret < 0)
            goto fail;

        face_count = filter_targets(ctx, s->host_boxes, s->last_count);
        if (face_count > 0) {
            if (total + face_count > s->max_det * 4)
                face_count = s->max_det * 4 - total;
            memcpy(merged + total, s->host_boxes, (size_t)face_count * sizeof(*merged));
            total += face_count;
        }
    }

    total = filter_targets(ctx, merged, total);
    if (total > 1)
        qsort(merged, total, sizeof(*merged), box_cmp_roi_priority);
    if (total > 1 && s->iou > 0.0f)
        total = detect_apply_nms(merged, total, s->iou);
    /* Cap AFTER score sort so max_det=1 keeps the best box, not merge order. */
    if (total > s->max_det)
        total = s->max_det;

    s->last_count = total;
    if (s->last_count > 0)
        memcpy(s->host_boxes, merged, (size_t)s->last_count * sizeof(*s->host_boxes));

    /* Always sync finalized boxes; beauty/draw/ROI share this same list. */
    ret = sync_boxes_to_gpu(ctx);
    if (ret < 0)
        goto fail;

    if (s->bilateral > 0) {
        ret = launch_bilateral_faces(ctx, src, dst);
        if (ret < 0)
            goto fail;
    }

    if (s->draw) {
        ret = launch_draw(ctx, frame);
        if (ret < 0)
            goto fail;
    }

    if (s->bilateral > 0 || s->draw) {
        ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
        if (ret < 0)
            goto fail;
    }

    av_free(merged);
    return 0;

fail:
    av_free(merged);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Per-frame filter callback                                                 */
/* ------------------------------------------------------------------------- */

static int detect_cuda_filter_frame(AVFilterLink *link, AVFrame *src)
{
    AVFilterContext *ctx = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *ol = ff_filter_link(outlink);
    CUDADetectContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    AVFrame *dst = NULL;
    int ret, do_infer;

    dst = av_frame_alloc();
    if (!dst) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(ol->hw_frames_ctx, dst, 0);
    if (ret < 0)
        goto fail;

    dst->width = outlink->w;
    dst->height = outlink->h;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    ret = copy_cuda_frame(ctx, dst, src);
    if (ret < 0)
        goto pop_fail;

    /* stride>1: skip TRT and reuse last boxes; force infer when no prior dets */
    do_infer = s->stride <= 1 || (s->frame_index % s->stride) == 0 || s->last_count <= 0;
    if (do_infer) {
        ret = run_detection(ctx, dst, src);
    } else {
        /* Reuse last boxes: optional face beauty then optional draw. */
        if (s->bilateral > 0)
            ret = launch_bilateral_faces(ctx, src, dst);
        else
            ret = 0;
        if (ret >= 0 && s->draw)
            ret = launch_draw(ctx, dst);
        if (ret >= 0)
            ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
    }
    if (ret < 0)
        goto pop_fail;

pop_fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(dst, src);
    if (ret < 0)
        goto fail;

    ret = write_roi_side_data(ctx, dst);
    if (ret < 0)
        goto fail;

    dump_boxes(s, dst);
    s->frame_index++;

    av_frame_free(&src);
    return ff_filter_frame(outlink, dst);

fail:
    av_frame_free(&src);
    av_frame_free(&dst);
    return ret;
}

/* Load or build TRT engine; ONNX caches as
 * {path}.{S}x{S}.{fp16|fp32}.sm{XX}.trt{MMN}.trt */
static av_cold int create_trt_context(AVFilterContext *ctx,
                                      const char *path,
                                      DetectModelType model_type,
                                      FFDetectTRTContext **trt_out,
                                      FFDetectTRTModelInfo *info_out)
{
    CUDADetectContext *s = ctx->priv;
    FFDetectTRTConfig trt_cfg = { 0 };
    char errbuf[DETECT_ERR_SIZE] = { 0 };
    char cache_path[1024];
    int ret, building = 0;
    size_t plen, slen;

    trt_cfg.engine_path     = path;
    trt_cfg.model_type      = model_type;
    trt_cfg.input_w         = s->input_size;
    trt_cfg.input_h         = s->input_size;
    trt_cfg.max_det         = s->max_det;
    trt_cfg.fp16            = s->fp16;
    trt_cfg.score_threshold = s->score;
    trt_cfg.iou_threshold   = s->iou;
    trt_cfg.cuda_device     = s->hwctx->internal->cuda_device;

    plen = strlen(path);
    slen = strlen(".onnx");
    if (plen >= slen && !strcmp(path + plen - slen, ".onnx")) {
        FILE *cache_fp;

        ret = ff_detect_trt_resolve_cache_path(&trt_cfg, cache_path,
                                               sizeof(cache_path),
                                               errbuf, sizeof(errbuf));
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "detect_cuda: resolve TRT cache path failed for %s: %s\n",
                   path, errbuf);
            return AVERROR_EXTERNAL;
        }

        cache_fp = fopen(cache_path, "rb");
        if (!cache_fp) {
            av_log(ctx, AV_LOG_WARNING,
                   "detect_cuda: 首次从 ONNX 构建 TensorRT 引擎，预计耗时 1~3 分钟，请耐心等待...\n");
            av_log(ctx, AV_LOG_WARNING, "detect_cuda: ONNX 路径: %s\n", path);
            av_log(ctx, AV_LOG_WARNING, "detect_cuda: TRT  缓存: %s\n", cache_path);
            building = 1;
        } else {
            fclose(cache_fp);
            av_log(ctx, AV_LOG_INFO,
                   "detect_cuda: 从缓存加载 TensorRT 引擎: %s\n", cache_path);
        }
    } else {
        av_log(ctx, AV_LOG_INFO, "detect_cuda: 加载 TensorRT 引擎: %s\n", path);
    }

    ret = ff_detect_trt_create(trt_out, &trt_cfg, errbuf, sizeof(errbuf));
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "TensorRT init failed for %s: %s\n", path, errbuf);
        return AVERROR_EXTERNAL;
    }

    if (building) {
        av_log(ctx, AV_LOG_INFO,
               "detect_cuda: TensorRT 引擎构建完成，已写入缓存: %s\n", cache_path);
    }

    ret = ff_detect_trt_get_info(*trt_out, info_out);
    if (ret < 0)
        return ret;

    if (info_out->model_type == DETECT_MODEL_YOLOV8_FACE &&
        info_out->num_face_heads == FF_DETECT_FACE_HEADS) {
        av_log(ctx, AV_LOG_INFO,
               "detect_cuda: detected YOLOv8-Face 3-head DFL format "
               "(grids %dx%d / %dx%d / %dx%d, strides %d/%d/%d)\n",
               info_out->face_heads[0].grid_size, info_out->face_heads[0].grid_size,
               info_out->face_heads[1].grid_size, info_out->face_heads[1].grid_size,
               info_out->face_heads[2].grid_size, info_out->face_heads[2].grid_size,
               info_out->face_heads[0].stride, info_out->face_heads[1].stride,
               info_out->face_heads[2].stride);
    }

    if (info_out->raw_output_bytes > s->raw_output_bytes)
        s->raw_output_bytes = info_out->raw_output_bytes;

    if (info_out->model_type == DETECT_MODEL_YOLOV8 &&
        (info_out->num_anchors <= 0 || info_out->num_classes <= 0 ||
         info_out->raw_output_bytes == 0)) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid YOLOv8 model %s: anchors=%d classes=%d bytes=%zu\n",
               path, info_out->num_anchors, info_out->num_classes,
               info_out->raw_output_bytes);
        ff_detect_trt_destroy(trt_out);
        return AVERROR(EINVAL);
    }

    if (info_out->model_type == DETECT_MODEL_YOLOV8_FACE &&
        (info_out->num_face_heads != FF_DETECT_FACE_HEADS ||
         info_out->face_heads_total_bytes == 0)) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid YOLOv8-Face model %s: heads=%d bytes=%zu\n",
               path, info_out->num_face_heads, info_out->face_heads_total_bytes);
        ff_detect_trt_destroy(trt_out);
        return AVERROR(EINVAL);
    }

    return 0;
}

/* Output link setup: validate options, create TRT contexts, alloc GPU buffers */
static av_cold int detect_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    CUDADetectContext *s = ctx->priv;
    AVHWFramesContext *frames_ctx;
    int ret;
    const char *model_names[] = { "auto", "yolov8", "yolov8_face" };

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No CUDA hw_frames_ctx on input\n");
        return AVERROR(EINVAL);
    }

    frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    if (!format_is_supported(frames_ctx->sw_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported CUDA sw_format %s\n",
               av_get_pix_fmt_name(frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    s->hwctx = frames_ctx->device_ctx->hwctx;
    s->cu_stream = s->hwctx->stream;
    s->sw_format = frames_ctx->sw_format;
    s->desc = av_pix_fmt_desc_get(s->sw_format);
    s->detect_format = detect_format_code(s->sw_format);
    s->bit_depth = s->desc->comp[0].depth;
    ret = parse_labels(ctx, s);
    if (ret < 0)
        return ret;
    ret = parse_targets(ctx, s);
    if (ret < 0)
        return ret;

    if (s->want_face && s->want_person && (!s->face_engine || !s->face_engine[0])) {
        av_log(ctx, AV_LOG_WARNING,
               "person+face requested but face_engine not set; "
               "disabling face target (set face_engine=/models/yolov8n-face.onnx)\n");
        s->want_face = 0;
    }

    if (!s->want_person && !s->want_face) {
        av_log(ctx, AV_LOG_ERROR,
               "no detection target enabled; set targets=person, face, or person+face\n");
        return AVERROR(EINVAL);
    }

    if (s->bilateral > 0 && !s->want_face) {
        av_log(ctx, AV_LOG_WARNING,
               "bilateral=%d ignored: face beauty needs targets including face\n",
               s->bilateral);
        s->bilateral = 0;
    } else if (s->bilateral > 0) {
        av_log(ctx, AV_LOG_INFO,
               "face bilateral beauty level=%d (sigmaS=%.1f sigmaR=%.1f window=%d)\n",
               s->bilateral,
               detect_bilateral_presets[s->bilateral].sigmaS,
               detect_bilateral_presets[s->bilateral].sigmaR,
               detect_bilateral_presets[s->bilateral].window_size | 1);
    }

    if (s->want_person && (!s->engine || !s->engine[0])) {
        av_log(ctx, AV_LOG_ERROR, "engine option is required for person detection\n");
        return AVERROR(EINVAL);
    }

    if (s->want_face && !s->want_person &&
        (!s->engine || !s->engine[0]) &&
        (!s->face_engine || !s->face_engine[0])) {
        av_log(ctx, AV_LOG_ERROR,
               "engine or face_engine is required for face detection\n");
        return AVERROR(EINVAL);
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;

        ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (ret < 0)
            return ret;

        if (s->want_person) {
            ret = create_trt_context(ctx, s->engine, s->model_type, &s->trt, &s->model_info);
            if (ret < 0)
                goto trt_fail;
        }

        if (s->want_face) {
            const char *face_path;
            DetectModelType face_type;

            if (s->face_engine && s->face_engine[0])
                face_path = s->face_engine;
            else
                face_path = s->engine;

            face_type = s->face_model_type;
            if (face_type == DETECT_MODEL_AUTO && !s->want_person)
                face_type = s->model_type;

            ret = create_trt_context(ctx, face_path, face_type,
                                     &s->face_trt, &s->face_model_info);
            if (ret < 0)
                goto trt_fail;
        }

        ret = CHECK_CU(cu->cuCtxPopCurrent(&dummy));
        if (ret < 0)
            return ret;
        goto trt_done;

trt_fail:
        ff_detect_trt_destroy(&s->trt);
        ff_detect_trt_destroy(&s->face_trt);
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
        return ret;

trt_done:
        ;
    }

    ret = init_hwframe_ctx(s, frames_ctx->device_ref, s->sw_format,
                           inlink->w, inlink->h);
    if (ret < 0)
        return ret;

    ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ol->hw_frames_ctx)
        return AVERROR(ENOMEM);

    ret = detect_cuda_load_functions(ctx);
    if (ret < 0)
        return ret;

    ret = alloc_gpu_buffers(ctx);
    if (ret < 0)
        return ret;

    if ((s->model_info.model_type == DETECT_MODEL_YOLOV8 ||
         s->face_model_info.model_type == DETECT_MODEL_YOLOV8) &&
        s->raw_output_bytes > 0 && !s->raw_output_dev) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate TRT raw output buffer\n");
        return AVERROR(ENOMEM);
    }

    if ((s->model_info.model_type == DETECT_MODEL_YOLOV8_FACE ||
         s->face_model_info.model_type == DETECT_MODEL_YOLOV8_FACE) &&
        !s->yolov8_face_host) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate YOLOv8-Face head buffers\n");
        return AVERROR(ENOMEM);
    }

    if (s->dump_path && s->dump_path[0]) {
        s->dump_file = fopen(s->dump_path, "wb");
        if (!s->dump_file) {
            av_log(ctx, AV_LOG_ERROR, "Could not open dump file %s\n", s->dump_path);
            return AVERROR(EIO);
        }
    }

    av_log(ctx, AV_LOG_INFO,
           "detect_cuda: %dx%d %s input=%d targets=%s max_det=%d "
           "person_model=%s(%s) face_model=%s(%s) score=%.3f iou=%.3f\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(s->sw_format),
           s->input_size, s->targets ? s->targets : "person+face", s->max_det,
           s->trt ? s->engine : "none",
           s->trt ? model_names[s->model_info.model_type] : "none",
           s->face_trt ? (s->face_engine && s->face_engine[0] ? s->face_engine : s->engine)
                       : "none",
           s->face_trt ? model_names[s->face_model_info.model_type] : "none",
           s->score, s->iou);
    return 0;
}

static av_cold void detect_cuda_uninit(AVFilterContext *ctx)
{
    CUDADetectContext *s = ctx->priv;
    int i;

    if (s->dump_file)
        fclose(s->dump_file);

    ff_detect_trt_destroy(&s->trt);
    ff_detect_trt_destroy(&s->face_trt);

    if (s->hwctx) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (s->cu_module)
            CHECK_CU(cu->cuModuleUnload(s->cu_module));
        if (s->input_dev)
            CHECK_CU(cu->cuMemFree(s->input_dev));
        if (s->raw_output_dev)
            CHECK_CU(cu->cuMemFree(s->raw_output_dev));
        if (s->boxes_dev)
            CHECK_CU(cu->cuMemFree(s->boxes_dev));
        if (s->count_dev)
            CHECK_CU(cu->cuMemFree(s->count_dev));
        if (s->matrix_dev)
            CHECK_CU(cu->cuMemFree(s->matrix_dev));
        for (i = 0; i < FF_DETECT_FACE_HEADS; i++) {
            if (s->yolov8_face_dev[i])
                CHECK_CU(cu->cuMemFree(s->yolov8_face_dev[i]));
        }
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    av_buffer_unref(&s->frames_ctx);
    av_freep(&s->host_boxes);
    av_freep(&s->yolov8_face_host);
}

/* ------------------------------------------------------------------------- */
/* AVOption table, filter registration                                       */
/* ------------------------------------------------------------------------- */

#define OFFSET(x) offsetof(CUDADetectContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption detect_cuda_options[] = {
    { "engine", "TensorRT/ONNX path for person model, or face model when targets=face", OFFSET(engine), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "face_engine", "TensorRT/ONNX path for face model (required for person+face dual mode)", OFFSET(face_engine), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "model_type", "Primary model output format", OFFSET(model_type), AV_OPT_TYPE_INT, { .i64 = DETECT_MODEL_AUTO }, DETECT_MODEL_AUTO, DETECT_MODEL_YOLOV8_FACE, FLAGS, .unit = "model_type" },
    { "auto", "Detect format from engine tensors", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_AUTO }, 0, 0, FLAGS, .unit = "model_type" },
    { "yolov8", "YOLOv8 raw output [1,4+nc,anchors]", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_YOLOV8 }, 0, 0, FLAGS, .unit = "model_type" },
    { "yolov8_face", "YOLOv8-Face 3-head DFL output (yakhyo yolov8n-face.onnx)", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_YOLOV8_FACE }, 0, 0, FLAGS, .unit = "model_type" },
    { "face_model_type", "Face engine output format", OFFSET(face_model_type), AV_OPT_TYPE_INT, { .i64 = DETECT_MODEL_AUTO }, DETECT_MODEL_AUTO, DETECT_MODEL_YOLOV8_FACE, FLAGS, .unit = "face_model_type" },
    { "auto", "Detect format from face engine tensors", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_AUTO }, 0, 0, FLAGS, .unit = "face_model_type" },
    { "yolov8", "Face YOLOv8 engine", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_YOLOV8 }, 0, 0, FLAGS, .unit = "face_model_type" },
    { "yolov8_face", "Face YOLOv8-Face 3-head DFL engine", 0, AV_OPT_TYPE_CONST, { .i64 = DETECT_MODEL_YOLOV8_FACE }, 0, 0, FLAGS, .unit = "face_model_type" },
    { "targets", "Detection targets: person, face, or person+face (+ separated)", OFFSET(targets), AV_OPT_TYPE_STRING, { .str = "person" }, 0, 0, FLAGS },
    { "labels", "Class names for ROI metadata: person, face, or person+face (+ separated)", OFFSET(labels), AV_OPT_TYPE_STRING, { .str = "person+face" }, 0, 0, FLAGS },
    { "score", "Detection score threshold", OFFSET(score), AV_OPT_TYPE_FLOAT, { .dbl = 0.45 }, 0.0, 1.0, FLAGS },
    { "iou", "Host-side NMS IoU threshold(非极大值抑制)", OFFSET(iou), AV_OPT_TYPE_FLOAT, { .dbl = 0.45 }, 0.0, 1.0, FLAGS },
    { "max_det", "Maximum detections per frame", OFFSET(max_det), AV_OPT_TYPE_INT, { .i64 = 64 }, 1, 1024, FLAGS },
    { "input_size", "Square model input size", OFFSET(input_size), AV_OPT_TYPE_INT, { .i64 = 640 }, 32, 4096, FLAGS },
    { "fp16", "Build ONNX engine with FP16 when supported", OFFSET(fp16), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "draw", "Draw detection boxes on output frames", OFFSET(draw), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "bilateral", "Face-region beauty strength: 0=off, 1=weak, 2=medium, 3=strong", OFFSET(bilateral), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 3, FLAGS },
    { "thickness", "Box border thickness in pixels", OFFSET(thickness), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 16, FLAGS },
    { "face_qoffset", "ROI qoffset numerator for face (-1..1 maps via /100)", OFFSET(face_qoffset), AV_OPT_TYPE_INT, { .i64 = -100 }, -100, 100, FLAGS },
    { "person_qoffset", "ROI qoffset numerator for person (-1..1 maps via /100)", OFFSET(person_qoffset), AV_OPT_TYPE_INT, { .i64 = -100 }, -100, 100, FLAGS },
    { "stride", "Run inference every N frames", OFFSET(stride), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 1024, FLAGS },
    { "dump", "Optional JSONL detection dump path", OFFSET(dump_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "pool_size", "CUDA output frame pool size", OFFSET(pool_size), AV_OPT_TYPE_INT, { .i64 = 32 }, 8, 128, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(detect_cuda);

static const AVFilterPad detect_cuda_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = detect_cuda_filter_frame,
    },
};

static const AVFilterPad detect_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = detect_cuda_config_props,
    },
};

const FFFilter ff_vf_detect_cuda = {
    .p.name         = "detect_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("CUDA/TensorRT person and face detection with ROI side data"),
    .p.priv_class   = &detect_cuda_class,
    .priv_size      = sizeof(CUDADetectContext),
    .uninit         = detect_cuda_uninit,
    FILTER_INPUTS(detect_cuda_inputs),
    FILTER_OUTPUTS(detect_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
