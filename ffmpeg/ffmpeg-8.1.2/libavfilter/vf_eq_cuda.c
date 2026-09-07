/*
 * CUDA video equalizer
 *
 * Based on vf_eq.c (GPL).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <math.h>

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "filters.h"
#include "cuda/load_helper.h"
#include "video.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCK_X 32
#define BLOCK_Y 16
#define EQ_LUT_MAX 1024

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_P010,
};

static const char *const var_names[] = {
    "n", "r", "t", NULL
};

enum var_name {
    VAR_N,
    VAR_R,
    VAR_T,
    VAR_NB
};

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

typedef struct EQCUDAContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;
    enum AVPixelFormat sw_format;
    int depth;
    int lut_size;

    char *contrast_expr;
    AVExpr *contrast_pexpr;
    double contrast;

    char *brightness_expr;
    AVExpr *brightness_pexpr;
    double brightness;

    char *saturation_expr;
    AVExpr *saturation_pexpr;
    double saturation;

    char *gamma_expr;
    AVExpr *gamma_pexpr;
    double gamma;

    char *gamma_weight_expr;
    AVExpr *gamma_weight_pexpr;
    double gamma_weight;

    char *gamma_r_expr;
    AVExpr *gamma_r_pexpr;
    double gamma_r;

    char *gamma_g_expr;
    AVExpr *gamma_g_pexpr;
    double gamma_g;

    char *gamma_b_expr;
    AVExpr *gamma_b_pexpr;
    double gamma_b;

    double var_values[VAR_NB];
    int eval_mode;

    /* per-plane params mirroring CPU eq */
    double p_contrast[3];
    double p_brightness[3];
    double p_gamma[3];
    double p_gamma_weight[3];
    int lut_dirty;

    uint16_t host_lut[3][EQ_LUT_MAX];
    CUdeviceptr lut_dev[3];
    size_t lut_bytes;

    CUmodule cu_module;
    CUfunction cu_eq_uchar;
    CUfunction cu_eq_uchar2;
    CUfunction cu_eq_ushort;
    CUfunction cu_eq_ushort2;

    AVBufferRef *frames_ctx;
} EQCUDAContext;

static int format_is_supported(enum AVPixelFormat fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

static void create_lut_plane(EQCUDAContext *s, int plane)
{
    const int lut_size = s->lut_size;
    const double maxv = lut_size - 1.0;
    const double contrast = s->p_contrast[plane];
    const double brightness = s->p_brightness[plane];
    const double g = 1.0 / s->p_gamma[plane];
    const double lw = 1.0 - s->p_gamma_weight[plane];
    const double gw = s->p_gamma_weight[plane];

    for (int i = 0; i < lut_size; i++) {
        double v = i / maxv;
        v = contrast * (v - 0.5) + 0.5 + brightness;
        if (v <= 0.0) {
            s->host_lut[plane][i] = 0;
        } else {
            v = v * lw + pow(v, g) * gw;
            if (v >= 1.0)
                s->host_lut[plane][i] = (uint16_t)maxv;
            else
                s->host_lut[plane][i] = (uint16_t)(v * lut_size);
        }
    }
}

static int upload_luts(AVFilterContext *ctx)
{
    EQCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    for (int p = 0; p < 3; p++)
        create_lut_plane(s, p);

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    for (int p = 0; p < 3; p++) {
        if (s->depth == 8) {
            uint8_t tmp[256];
            for (int i = 0; i < 256; i++)
                tmp[i] = (uint8_t)s->host_lut[p][i];
            ret = CHECK_CU(cu->cuMemcpyHtoD(s->lut_dev[p], tmp, 256));
        } else {
            ret = CHECK_CU(cu->cuMemcpyHtoD(s->lut_dev[p], s->host_lut[p],
                                             s->lut_size * sizeof(uint16_t)));
        }
        if (ret < 0)
            break;
    }

    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    s->lut_dirty = 0;
    return ret;
}

static void set_contrast(EQCUDAContext *s)
{
    s->contrast = av_clipd(av_expr_eval(s->contrast_pexpr, s->var_values, s), -1000.0, 1000.0);
    s->p_contrast[0] = s->contrast;
    s->lut_dirty = 1;
}

static void set_brightness(EQCUDAContext *s)
{
    s->brightness = av_clipd(av_expr_eval(s->brightness_pexpr, s->var_values, s), -1.0, 1.0);
    s->p_brightness[0] = s->brightness;
    s->lut_dirty = 1;
}

static void set_gamma(EQCUDAContext *s)
{
    s->gamma        = av_clipd(av_expr_eval(s->gamma_pexpr,        s->var_values, s), 0.1, 10.0);
    s->gamma_r      = av_clipd(av_expr_eval(s->gamma_r_pexpr,      s->var_values, s), 0.1, 10.0);
    s->gamma_g      = av_clipd(av_expr_eval(s->gamma_g_pexpr,      s->var_values, s), 0.1, 10.0);
    s->gamma_b      = av_clipd(av_expr_eval(s->gamma_b_pexpr,      s->var_values, s), 0.1, 10.0);
    s->gamma_weight = av_clipd(av_expr_eval(s->gamma_weight_pexpr, s->var_values, s), 0.0, 1.0);

    s->p_gamma[0] = s->gamma * s->gamma_g;
    s->p_gamma[1] = sqrt(s->gamma_b / s->gamma_g);
    s->p_gamma[2] = sqrt(s->gamma_r / s->gamma_g);
    for (int i = 0; i < 3; i++)
        s->p_gamma_weight[i] = s->gamma_weight;
    s->lut_dirty = 1;
}

static void set_saturation(EQCUDAContext *s)
{
    s->saturation = av_clipd(av_expr_eval(s->saturation_pexpr, s->var_values, s), 0.0, 3.0);
    s->p_contrast[1] = s->saturation;
    s->p_contrast[2] = s->saturation;
    s->lut_dirty = 1;
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = *pexpr;

    ret = av_expr_parse(pexpr, expr, var_names, NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when parsing the expression '%s' for %s\n", expr, option);
        *pexpr = old;
        return ret;
    }
    av_expr_free(old);
    return 0;
}

static av_cold int eq_cuda_load(AVFilterContext *ctx)
{
    EQCUDAContext *s = ctx->priv;
    CUcontext dummy;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int ret;

    extern const unsigned char ff_vf_eq_cuda_ptx_data[];
    extern const unsigned int ff_vf_eq_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_eq_cuda_ptx_data, ff_vf_eq_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_eq_uchar,  s->cu_module, "eq_uchar"));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_eq_uchar2, s->cu_module, "eq_uchar2"));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_eq_ushort, s->cu_module, "eq_ushort"));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_eq_ushort2,s->cu_module, "eq_ushort2"));
    if (ret < 0) goto fail;

    s->lut_bytes = (s->depth == 8) ? 256 : s->lut_size * sizeof(uint16_t);
    for (int p = 0; p < 3; p++) {
        ret = CHECK_CU(cu->cuMemAlloc(&s->lut_dev[p], s->lut_bytes));
        if (ret < 0)
            goto fail;
    }

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold int eq_cuda_init(AVFilterContext *ctx)
{
    EQCUDAContext *s = ctx->priv;
    int ret;

    for (int i = 0; i < 3; i++) {
        s->p_contrast[i] = 1.0;
        s->p_brightness[i] = 0.0;
        s->p_gamma[i] = 1.0;
        s->p_gamma_weight[i] = 1.0;
    }

    if ((ret = set_expr(&s->contrast_pexpr,     s->contrast_expr,     "contrast",     ctx)) < 0 ||
        (ret = set_expr(&s->brightness_pexpr,   s->brightness_expr,   "brightness",   ctx)) < 0 ||
        (ret = set_expr(&s->saturation_pexpr,   s->saturation_expr,   "saturation",   ctx)) < 0 ||
        (ret = set_expr(&s->gamma_pexpr,        s->gamma_expr,        "gamma",        ctx)) < 0 ||
        (ret = set_expr(&s->gamma_r_pexpr,      s->gamma_r_expr,      "gamma_r",      ctx)) < 0 ||
        (ret = set_expr(&s->gamma_g_pexpr,      s->gamma_g_expr,      "gamma_g",      ctx)) < 0 ||
        (ret = set_expr(&s->gamma_b_pexpr,      s->gamma_b_expr,      "gamma_b",      ctx)) < 0 ||
        (ret = set_expr(&s->gamma_weight_pexpr, s->gamma_weight_expr, "gamma_weight", ctx)) < 0)
        return ret;

    return 0;
}

static av_cold void eq_cuda_uninit(AVFilterContext *ctx)
{
    EQCUDAContext *s = ctx->priv;
    CUcontext dummy;

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        for (int p = 0; p < 3; p++) {
            if (s->lut_dev[p]) {
                CHECK_CU(cu->cuMemFree(s->lut_dev[p]));
                s->lut_dev[p] = 0;
            }
        }
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        s->cu_module = NULL;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    av_expr_free(s->contrast_pexpr);
    av_expr_free(s->brightness_pexpr);
    av_expr_free(s->saturation_pexpr);
    av_expr_free(s->gamma_pexpr);
    av_expr_free(s->gamma_weight_pexpr);
    av_expr_free(s->gamma_r_pexpr);
    av_expr_free(s->gamma_g_pexpr);
    av_expr_free(s->gamma_b_pexpr);

    av_buffer_unref(&s->frames_ctx);
}

static int eq_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    EQCUDAContext *s = ctx->priv;
    AVHWFramesContext *in_frames_ctx;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->hwctx = in_frames_ctx->device_ctx->hwctx;
    s->sw_format = in_frames_ctx->sw_format;

    if (!format_is_supported(s->sw_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported format: %s\n",
               av_get_pix_fmt_name(s->sw_format));
        return AVERROR(EINVAL);
    }

    s->depth = (s->sw_format == AV_PIX_FMT_P010) ? 10 : 8;
    s->lut_size = 1 << s->depth;

    s->var_values[VAR_N] = 0;
    s->var_values[VAR_R] = inl->frame_rate.num && inl->frame_rate.den ?
                           av_q2d(inl->frame_rate) : NAN;

    if (s->eval_mode == EVAL_MODE_INIT) {
        set_gamma(s);
        set_contrast(s);
        set_brightness(s);
        set_saturation(s);
    }

    ret = eq_cuda_load(ctx);
    if (ret < 0)
        return ret;

    ret = upload_luts(ctx);
    if (ret < 0)
        return ret;

    /* passthrough frames ctx (same size/format) */
    outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->format = AV_PIX_FMT_CUDA;

    return 0;
}

static int eq_cuda_apply(AVFilterContext *ctx, AVFrame *out, AVFrame *in)
{
    EQCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->sw_format);
    int ret;

    if (s->lut_dirty) {
        ret = upload_luts(ctx);
        if (ret < 0)
            return ret;
    }

    if (s->sw_format == AV_PIX_FMT_NV12 || s->sw_format == AV_PIX_FMT_P010) {
        int yw = in->width;
        int yh = in->height;
        int uw = AV_CEIL_RSHIFT(yw, desc->log2_chroma_w);
        int uh = AV_CEIL_RSHIFT(yh, desc->log2_chroma_h);
        int y_src_pitch = in->linesize[0] / ((s->depth == 8) ? 1 : 2);
        int y_dst_pitch = out->linesize[0] / ((s->depth == 8) ? 1 : 2);
        int uv_src_pitch = in->linesize[1] / ((s->depth == 8) ? 2 : 4);
        int uv_dst_pitch = out->linesize[1] / ((s->depth == 8) ? 2 : 4);
        CUdeviceptr d_ys = (CUdeviceptr)in->data[0];
        CUdeviceptr d_yd = (CUdeviceptr)out->data[0];
        CUdeviceptr d_us = (CUdeviceptr)in->data[1];
        CUdeviceptr d_ud = (CUdeviceptr)out->data[1];
        CUfunction fy = (s->depth == 8) ? s->cu_eq_uchar : s->cu_eq_ushort;
        CUfunction fuv = (s->depth == 8) ? s->cu_eq_uchar2 : s->cu_eq_ushort2;
        void *args_y[] = { &d_yd, &y_dst_pitch, &d_ys, &y_src_pitch, &yw, &yh, &s->lut_dev[0] };
        void *args_uv[] = { &d_ud, &uv_dst_pitch, &d_us, &uv_src_pitch, &uw, &uh,
                            &s->lut_dev[1], &s->lut_dev[2] };

        ret = CHECK_CU(cu->cuLaunchKernel(fy, DIV_UP(yw, BLOCK_X), DIV_UP(yh, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args_y, NULL));
        if (ret < 0)
            return ret;
        ret = CHECK_CU(cu->cuLaunchKernel(fuv, DIV_UP(uw, BLOCK_X), DIV_UP(uh, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args_uv, NULL));
        return ret;
    }

    /* planar YUV420P / YUV444P */
    for (int plane = 0; plane < 3; plane++) {
        int w = in->width;
        int h = in->height;
        if (plane > 0) {
            w = AV_CEIL_RSHIFT(w, desc->log2_chroma_w);
            h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
        }
        int src_pitch = in->linesize[plane];
        int dst_pitch = out->linesize[plane];
        CUdeviceptr d_src = (CUdeviceptr)in->data[plane];
        CUdeviceptr d_dst = (CUdeviceptr)out->data[plane];
        void *args[] = { &d_dst, &dst_pitch, &d_src, &src_pitch, &w, &h, &s->lut_dev[plane] };
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_eq_uchar,
                                          DIV_UP(w, BLOCK_X), DIV_UP(h, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args, NULL));
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int eq_cuda_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    EQCUDAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *outl = ff_filter_link(outlink);
    FilterLink *inl = ff_filter_link(inlink);
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CUcontext dummy;
    AVFrame *out;
    int ret;

    s->var_values[VAR_N] = inl->frame_count_out;
    s->var_values[VAR_T] = TS2T(in->pts, inlink->time_base);

    if (s->eval_mode == EVAL_MODE_FRAME) {
        set_gamma(s);
        set_contrast(s);
        set_brightness(s);
        set_saturation(s);
    }

    out = av_frame_alloc();
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ret = av_hwframe_get_buffer(outl->hw_frames_ctx, out, 0);
    if (ret < 0) {
        av_frame_free(&out);
        av_frame_free(&in);
        return ret;
    }

    ret = CHECK_CU(device_hwctx->internal->cuda_dl->cuCtxPushCurrent(device_hwctx->cuda_ctx));
    if (ret < 0) {
        av_frame_free(&out);
        av_frame_free(&in);
        return ret;
    }

    ret = eq_cuda_apply(ctx, out, in);
    CHECK_CU(device_hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));

    if (ret < 0) {
        av_frame_free(&out);
        av_frame_free(&in);
        return ret;
    }

    av_frame_copy_props(out, in);
    out->width = in->width;
    out->height = in->height;
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    EQCUDAContext *s = ctx->priv;
    int ret;

#define SET_PARAM(name, set_fn) do { \
    if (!strcmp(cmd, #name)) { \
        if ((ret = set_expr(&s->name##_pexpr, args, cmd, ctx)) < 0) return ret; \
        if (s->eval_mode == EVAL_MODE_INIT) set_##set_fn(s); \
        return 0; \
    } \
} while (0)

    SET_PARAM(contrast, contrast);
    SET_PARAM(brightness, brightness);
    SET_PARAM(saturation, saturation);
    SET_PARAM(gamma, gamma);
    SET_PARAM(gamma_r, gamma);
    SET_PARAM(gamma_g, gamma);
    SET_PARAM(gamma_b, gamma);
    SET_PARAM(gamma_weight, gamma);
    return AVERROR(ENOSYS);
}

#define OFFSET(x) offsetof(EQCUDAContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define TFLAGS FLAGS|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption eq_cuda_options[] = {
    { "contrast",     "set the contrast adjustment, negative values give a negative image",
        OFFSET(contrast_expr),     AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "brightness",   "set the brightness adjustment",
        OFFSET(brightness_expr),   AV_OPT_TYPE_STRING, {.str = "0.0"}, 0, 0, TFLAGS },
    { "saturation",   "set the saturation adjustment",
        OFFSET(saturation_expr),   AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "gamma",        "set the initial gamma value",
        OFFSET(gamma_expr),        AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "gamma_r",      "gamma value for red",
        OFFSET(gamma_r_expr),      AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "gamma_g",      "gamma value for green",
        OFFSET(gamma_g_expr),      AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "gamma_b",      "gamma value for blue",
        OFFSET(gamma_b_expr),      AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "gamma_weight", "set the gamma weight which reduces the effect of gamma on bright areas",
        OFFSET(gamma_weight_expr), AV_OPT_TYPE_STRING, {.str = "1.0"}, 0, 0, TFLAGS },
    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT,
        {.i64 = EVAL_MODE_INIT}, 0, EVAL_MODE_NB-1, FLAGS, .unit = "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST,
           {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions per-frame",                  0, AV_OPT_TYPE_CONST,
           {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { NULL }
};

static const AVClass eq_cuda_class = {
    .class_name = "eq_cuda",
    .item_name  = av_default_item_name,
    .option     = eq_cuda_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad eq_cuda_inputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .filter_frame = eq_cuda_filter_frame,
}};

static const AVFilterPad eq_cuda_outputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = eq_cuda_config_props,
}};

const FFFilter ff_vf_eq_cuda = {
    .p.name          = "eq_cuda",
    .p.description   = NULL_IF_CONFIG_SMALL("CUDA adjust brightness, contrast, gamma, and saturation."),
    .p.priv_class    = &eq_cuda_class,
    .p.flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size       = sizeof(EQCUDAContext),
    .init            = eq_cuda_init,
    .uninit          = eq_cuda_uninit,
    .process_command = process_command,
    FILTER_INPUTS(eq_cuda_inputs),
    FILTER_OUTPUTS(eq_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
