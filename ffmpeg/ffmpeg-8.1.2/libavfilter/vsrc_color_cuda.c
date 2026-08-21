/*
 * CUDA constant color video source (color_cuda).
 * Parameters aligned with color_vulkan; output AV_PIX_FMT_CUDA (nv12/p010).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "colorspace.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "video.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCKX 32
#define BLOCKY 16

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
};

static int format_is_supported(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

typedef struct ColorCudaContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;
    AVBufferRef *own_device;   /* created when lavfi/-i has no hw_device_ctx */
    AVBufferRef *device_ref;   /* alias: ctx->hw_device_ctx or own_device */
    AVBufferRef *frames_ctx;

    CUmodule cu_module;
    CUfunction cu_func_y_u8;
    CUfunction cu_func_uv_nv12;
    CUfunction cu_func_y_u16;
    CUfunction cu_func_uv_p010;
    CUstream cu_stream;

    int device_idx;
    uint8_t color_rgba[4];
    char *out_format_string;
    int out_range;
    int colorspace; /* user AVColorSpace; UNSPECIFIED = auto by format */
    int color_trc;  /* optional override; UNSPECIFIED = derive from colorspace */

    int w, h;
    AVRational frame_rate;
    AVRational time_base;
    AVRational sar;
    int64_t duration;
    int64_t pts;
    unsigned int nb_frame;

    enum AVPixelFormat sw_format;
    enum AVColorSpace   eff_csp;
    enum AVColorPrimaries eff_pri;
    enum AVColorTransferCharacteristic eff_trc;
    enum AVColorRange   eff_range;
    uint16_t y_val;
    uint16_t u_val;
    uint16_t v_val;

    int draw_once_reset;
    AVFrame *picref;
} ColorCudaContext;

/*
 * Defaults unless overridden:
 *   nv12  -> SDR: bt709 + bt709 + tv(limited)
 *   p010  -> HDR PQ: bt2020nc + smpte2084(PQ) + tv(limited)
 */
static int resolve_color_meta(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;

    if (s->colorspace == AVCOL_SPC_UNSPECIFIED) {
        s->eff_csp = (s->sw_format == AV_PIX_FMT_P010)
                     ? AVCOL_SPC_BT2020_NCL
                     : AVCOL_SPC_BT709;
    } else {
        s->eff_csp = s->colorspace;
    }

    switch (s->eff_csp) {
    case AVCOL_SPC_BT709:
        s->eff_pri = AVCOL_PRI_BT709;
        s->eff_trc = AVCOL_TRC_BT709;
        break;
    case AVCOL_SPC_BT470BG:
        s->eff_pri = AVCOL_PRI_BT470BG;
        s->eff_trc = AVCOL_TRC_SMPTE170M;
        break;
    case AVCOL_SPC_SMPTE170M:
        s->eff_pri = AVCOL_PRI_SMPTE170M;
        s->eff_trc = AVCOL_TRC_SMPTE170M;
        break;
    case AVCOL_SPC_SMPTE240M:
        s->eff_pri = AVCOL_PRI_SMPTE240M;
        s->eff_trc = AVCOL_TRC_SMPTE240M;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        s->eff_pri = AVCOL_PRI_BT2020;
        /* P010 defaults to HDR PQ; NV12 stays SDR-like BT.709 TRC */
        s->eff_trc = (s->sw_format == AV_PIX_FMT_P010)
                     ? AVCOL_TRC_SMPTE2084
                     : AVCOL_TRC_BT709;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported colorspace %d (%s)\n",
               s->eff_csp, av_color_space_name(s->eff_csp));
        return AVERROR(EINVAL);
    }

    if (s->color_trc != AVCOL_TRC_UNSPECIFIED)
        s->eff_trc = s->color_trc;

    /* Video pipelines commonly use limited/TV; PC/full only when asked. */
    s->eff_range = (s->out_range == AVCOL_RANGE_UNSPECIFIED)
                   ? AVCOL_RANGE_MPEG
                   : s->out_range;

    av_log(ctx, AV_LOG_VERBOSE,
           "color meta: space=%s pri=%s trc=%s range=%s format=%s\n",
           av_color_space_name(s->eff_csp),
           av_color_primaries_name(s->eff_pri),
           av_color_transfer_name(s->eff_trc),
           av_color_range_name(s->eff_range),
           av_get_pix_fmt_name(s->sw_format));
    return 0;
}

static int compute_yuv_vals(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;
    double rgb2yuv[3][3];
    double rgbad[4], yuvad[4];
    const AVLumaCoefficients *luma;
    int i;

    luma = av_csp_luma_coeffs_from_avcsp(s->eff_csp);
    if (!luma) {
        av_log(ctx, AV_LOG_ERROR, "No luma coefficients for colorspace %s\n",
               av_color_space_name(s->eff_csp));
        return AVERROR(EINVAL);
    }

    ff_fill_rgb2yuv_table(luma, rgb2yuv);

    for (i = 0; i < 4; i++)
        rgbad[i] = s->color_rgba[i] / 255.0;

    ff_matrix_mul_3x3_vec(yuvad, rgbad, rgb2yuv);
    yuvad[3] = rgbad[3];

    for (i = 0; i < 3; i++) {
        int chroma = i > 0;
        if (s->eff_range == AVCOL_RANGE_MPEG) {
            yuvad[i] *= (chroma ? 224.0 : 219.0) / 255.0;
            yuvad[i] += (chroma ? 128.0 :  16.0) / 255.0;
        } else if (chroma) {
            yuvad[i] += 0.5;
        }
    }

    if (s->sw_format == AV_PIX_FMT_NV12) {
        s->y_val = av_clip_uint8(llrint(yuvad[0] * 255.0));
        s->u_val = av_clip_uint8(llrint(yuvad[1] * 255.0));
        s->v_val = av_clip_uint8(llrint(yuvad[2] * 255.0));
    } else {
        /* P010: 10-bit in high bits of little-endian uint16 */
        s->y_val = av_clip_uintp2(llrint(yuvad[0] * 1023.0), 10) << 6;
        s->u_val = av_clip_uintp2(llrint(yuvad[1] * 1023.0), 10) << 6;
        s->v_val = av_clip_uintp2(llrint(yuvad[2] * 1023.0), 10) << 6;
    }

    return 0;
}

static av_cold int color_cuda_load_functions(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vsrc_color_cuda_ptx_data[];
    extern const unsigned int ff_vsrc_color_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vsrc_color_cuda_ptx_data,
                              ff_vsrc_color_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_y_u8, s->cu_module,
                                           "color_fill_y_u8"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uv_nv12, s->cu_module,
                                           "color_fill_uv_nv12"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_y_u16, s->cu_module,
                                           "color_fill_y_u16"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uv_p010, s->cu_module,
                                           "color_fill_uv_p010"));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int color_cuda_fill_frame(AVFilterContext *ctx, AVFrame *frame)
{
    ColorCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int uv_w = AV_CEIL_RSHIFT(frame->width, 1);
    int uv_h = AV_CEIL_RSHIFT(frame->height, 1);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr uv = (CUdeviceptr)frame->data[1];
    int pitch_y = frame->linesize[0];
    int pitch_uv = frame->linesize[1];
    unsigned char y8, u8, v8;
    unsigned short y16, u16, v16;
    int ret;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    if (s->sw_format == AV_PIX_FMT_NV12) {
        y8 = (unsigned char)s->y_val;
        u8 = (unsigned char)s->u_val;
        v8 = (unsigned char)s->v_val;
        {
            void *args_y[] = {
                &y, &pitch_y, &frame->width, &frame->height, &y8,
            };
            void *args_uv[] = {
                &uv, &pitch_uv, &uv_w, &uv_h, &u8, &v8,
            };

            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_y_u8,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                goto fail;
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_uv_nv12,
                                              DIV_UP(uv_w, BLOCKX),
                                              DIV_UP(uv_h, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_uv, NULL));
        }
    } else {
        y16 = s->y_val;
        u16 = s->u_val;
        v16 = s->v_val;
        {
            void *args_y[] = {
                &y, &pitch_y, &frame->width, &frame->height, &y16,
            };
            void *args_uv[] = {
                &uv, &pitch_uv, &uv_w, &uv_h, &u16, &v16,
            };

            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_y_u16,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                goto fail;
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_uv_p010,
                                              DIV_UP(uv_w, BLOCKX),
                                              DIV_UP(uv_h, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_uv, NULL));
        }
    }
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold int color_cuda_init(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;

    s->time_base = av_inv_q(s->frame_rate);
    s->nb_frame = 0;
    s->pts = 0;
    s->draw_once_reset = 0;

    av_log(ctx, AV_LOG_VERBOSE, "size:%dx%d rate:%d/%d duration:%f sar:%d/%d\n",
           s->w, s->h, s->frame_rate.num, s->frame_rate.den,
           s->duration < 0 ? -1 : (double)s->duration / 1000000,
           s->sar.num, s->sar.den);
    return 0;
}

static av_cold void color_cuda_uninit(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;
    CUcontext dummy;

    av_frame_free(&s->picref);
    av_buffer_unref(&s->frames_ctx);

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }
    s->cu_module = NULL;
    s->hwctx = NULL;
    s->device_ref = NULL;
    av_buffer_unref(&s->own_device);
}

static int color_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ColorCudaContext *s = ctx->priv;
    FilterLink *l = ff_filter_link(outlink);
    AVHWFramesContext *frames_ctx;
    AVHWDeviceContext *device_ctx;
    int ret;

    /* Prefer graph-attached device (-init_hw_device / -filter_hw_device).
     * lavfi -i path has none: create a private CUDA device. */
    if (ctx->hw_device_ctx) {
        s->device_ref = ctx->hw_device_ctx;
    } else {
        char buf[64];

        snprintf(buf, sizeof(buf), "%d", s->device_idx);
        ret = av_hwdevice_ctx_create(&s->own_device, AV_HWDEVICE_TYPE_CUDA,
                                     buf, NULL, 0);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create CUDA device %d\n",
                   s->device_idx);
            return ret;
        }
        s->device_ref = s->own_device;
        av_log(ctx, AV_LOG_VERBOSE, "Created private CUDA device %d\n",
               s->device_idx);
    }

    device_ctx = (AVHWDeviceContext *)s->device_ref->data;
    if (device_ctx->type != AV_HWDEVICE_TYPE_CUDA) {
        av_log(ctx, AV_LOG_ERROR, "Hardware device is not CUDA\n");
        return AVERROR(EINVAL);
    }
    s->hwctx = device_ctx->hwctx;
    s->cu_stream = s->hwctx->stream;

    if (!s->out_format_string)
        s->sw_format = AV_PIX_FMT_NV12;
    else {
        s->sw_format = av_get_pix_fmt(s->out_format_string);
        if (!format_is_supported(s->sw_format)) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unsupported format '%s' (supported: nv12, p010)\n",
                   s->out_format_string);
            return AVERROR(EINVAL);
        }
    }

    if (av_image_check_size(s->w, s->h, 0, ctx) < 0)
        return AVERROR(EINVAL);

    ret = resolve_color_meta(ctx);
    if (ret < 0)
        return ret;

    ret = compute_yuv_vals(ctx);
    if (ret < 0)
        return ret;

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = av_hwframe_ctx_alloc(s->device_ref);
    if (!s->frames_ctx)
        return AVERROR(ENOMEM);

    frames_ctx = (AVHWFramesContext *)s->frames_ctx->data;
    frames_ctx->format    = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = s->sw_format;
    frames_ctx->width     = FFALIGN(s->w, 32);
    frames_ctx->height    = FFALIGN(s->h, 32);
    frames_ctx->initial_pool_size = 4;

    ret = av_hwframe_ctx_init(s->frames_ctx);
    if (ret < 0) {
        av_buffer_unref(&s->frames_ctx);
        return ret;
    }

    ret = color_cuda_load_functions(ctx);
    if (ret < 0)
        return ret;

    s->time_base = av_inv_q(s->frame_rate);
    s->nb_frame = 0;
    s->pts = 0;

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->sample_aspect_ratio = s->sar;
    outlink->time_base = s->time_base;
    l->frame_rate = s->frame_rate;

    av_buffer_unref(&l->hw_frames_ctx);
    l->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!l->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int color_cuda_activate(AVFilterContext *ctx)
{
    ColorCudaContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *frame;
    int ret;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;

    if (s->duration >= 0 &&
        av_rescale_q(s->pts, s->time_base, AV_TIME_BASE_Q) >= s->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (s->draw_once_reset) {
        av_frame_free(&s->picref);
        s->draw_once_reset = 0;
    }

    if (!s->picref) {
        s->picref = ff_get_video_buffer(outlink, s->w, s->h);
        if (!s->picref)
            return AVERROR(ENOMEM);

        s->picref->width  = s->w;
        s->picref->height = s->h;

        ret = color_cuda_fill_frame(ctx, s->picref);
        if (ret < 0) {
            av_frame_free(&s->picref);
            return ret;
        }
    }

    frame = av_frame_clone(s->picref);
    if (!frame)
        return AVERROR(ENOMEM);

    frame->pts                 = s->pts;
    frame->duration            = 1;
    frame->flags              |= AV_FRAME_FLAG_KEY;
    frame->flags              &= ~AV_FRAME_FLAG_INTERLACED;
    frame->pict_type           = AV_PICTURE_TYPE_I;
    frame->sample_aspect_ratio = s->sar;
    frame->color_range         = s->eff_range;
    frame->colorspace          = s->eff_csp;
    frame->color_primaries     = s->eff_pri;
    frame->color_trc           = s->eff_trc;

    s->pts++;
    s->nb_frame++;

    return ff_filter_frame(outlink, frame);
}

static int color_cuda_process_command(AVFilterContext *ctx, const char *cmd,
                                      const char *args, char *res, int res_len,
                                      int flags)
{
    ColorCudaContext *s = ctx->priv;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    ret = resolve_color_meta(ctx);
    if (ret < 0)
        return ret;

    ret = compute_yuv_vals(ctx);
    if (ret < 0)
        return ret;

    s->draw_once_reset = 1;
    return 0;
}

#define OFFSET(x) offsetof(ColorCudaContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define FLAGSR (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption color_cuda_options[] = {
    { "color", "set color", OFFSET(color_rgba), AV_OPT_TYPE_COLOR, {.str = "black"}, 0, 0, FLAGSR },
    { "c",     "set color", OFFSET(color_rgba), AV_OPT_TYPE_COLOR, {.str = "black"}, 0, 0, FLAGSR },

    { "size", "set video size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, { .str = "1920x1080" }, 0, 0, FLAGS },
    { "s",    "set video size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, { .str = "1920x1080" }, 0, 0, FLAGS },

    { "rate", "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "60" }, 0, INT_MAX, FLAGS },
    { "r",    "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "60" }, 0, INT_MAX, FLAGS },

    { "duration", "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, { .i64 = -1 }, -1, INT64_MAX, FLAGS },
    { "d",        "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, { .i64 = -1 }, -1, INT64_MAX, FLAGS },

    { "sar", "set video sample aspect ratio", OFFSET(sar), AV_OPT_TYPE_RATIONAL, { .dbl = 1 },  0, INT_MAX, FLAGS },

    { "format", "CUDA sw_format (nv12 or p010)",
      OFFSET(out_format_string), AV_OPT_TYPE_STRING, { .str = "nv12" }, 0, 0, FLAGS },

    { "device", "CUDA device index when no -init_hw_device is provided",
      OFFSET(device_idx), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },

    { "colorspace", "YUV matrix / color space (auto: nv12=bt709 SDR, p010=bt2020nc HDR PQ)",
      OFFSET(colorspace), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED },
      AVCOL_SPC_RGB, AVCOL_SPC_NB - 1, FLAGSR, .unit = "csp" },
    { "space", "YUV matrix / color space (alias of colorspace)",
      OFFSET(colorspace), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED },
      AVCOL_SPC_RGB, AVCOL_SPC_NB - 1, FLAGSR, .unit = "csp" },
        { "bt709",     "BT.709",            0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT709 },      0, 0, FLAGSR, .unit = "csp" },
        { "bt601",     "BT.601 (SMPTE 170M / 525)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_SMPTE170M }, 0, 0, FLAGSR, .unit = "csp" },
        { "smpte170m", "SMPTE 170M / BT.601 525", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_SMPTE170M }, 0, 0, FLAGSR, .unit = "csp" },
        { "bt470bg",   "BT.470BG / BT.601 625", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT470BG },  0, 0, FLAGSR, .unit = "csp" },
        { "smpte240m", "SMPTE 240M",        0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_SMPTE240M }, 0, 0, FLAGSR, .unit = "csp" },
        { "bt2020",    "BT.2020 NCL",       0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT2020_NCL }, 0, 0, FLAGSR, .unit = "csp" },
        { "bt2020nc",  "BT.2020 NCL",       0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT2020_NCL }, 0, 0, FLAGSR, .unit = "csp" },
        { "bt2020ncl", "BT.2020 NCL",       0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_SPC_BT2020_NCL }, 0, 0, FLAGSR, .unit = "csp" },

    { "color_trc", "Color transfer characteristic (default derived from colorspace/format)",
      OFFSET(color_trc), AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_UNSPECIFIED },
      AVCOL_TRC_RESERVED0, AVCOL_TRC_NB - 1, FLAGSR, .unit = "trc" },
        { "bt709",      "BT.709",           0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_BT709 },     0, 0, FLAGSR, .unit = "trc" },
        { "smpte170m",  "SMPTE 170M",       0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_SMPTE170M }, 0, 0, FLAGSR, .unit = "trc" },
        { "bt2020_10",  "BT.2020 10-bit",   0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_BT2020_10 }, 0, 0, FLAGSR, .unit = "trc" },
        { "bt2020-10",  "BT.2020 10-bit",   0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_BT2020_10 }, 0, 0, FLAGSR, .unit = "trc" },
        { "smpte2084",  "SMPTE ST 2084 (PQ)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_SMPTE2084 }, 0, 0, FLAGSR, .unit = "trc" },
        { "pq",         "SMPTE ST 2084 (PQ)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_SMPTE2084 }, 0, 0, FLAGSR, .unit = "trc" },
        { "arib-std-b67","ARIB STD-B67 (HLG)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_ARIB_STD_B67 }, 0, 0, FLAGSR, .unit = "trc" },
        { "hlg",        "ARIB STD-B67 (HLG)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_TRC_ARIB_STD_B67 }, 0, 0, FLAGSR, .unit = "trc" },

    { "out_range", "Output colour range (default tv/limited)", OFFSET(out_range),
      AV_OPT_TYPE_INT, {.i64 = AVCOL_RANGE_UNSPECIFIED}, AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_JPEG,
      .flags = FLAGS, .unit = "range" },
        { "full", "Full range (pc)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, .unit = "range" },
        { "limited", "Limited range (tv)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, .unit = "range" },
        { "jpeg", "Full range (pc)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, .unit = "range" },
        { "mpeg", "Limited range (tv)", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, .unit = "range" },
        { "tv", "Limited range", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_MPEG }, 0, 0, FLAGS, .unit = "range" },
        { "pc", "Full range", 0, AV_OPT_TYPE_CONST, { .i64 = AVCOL_RANGE_JPEG }, 0, 0, FLAGS, .unit = "range" },
    { NULL },
};

AVFILTER_DEFINE_CLASS(color_cuda);

static const AVFilterPad color_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = color_cuda_config_props,
    },
};

const FFFilter ff_vsrc_color_cuda = {
    .p.name         = "color_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("Generate a constant color (CUDA)"),
    .p.inputs       = NULL,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .p.priv_class   = &color_cuda_class,
    .priv_size      = sizeof(ColorCudaContext),
    .init           = color_cuda_init,
    .uninit         = color_cuda_uninit,
    .activate       = color_cuda_activate,
    FILTER_OUTPUTS(color_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .process_command = color_cuda_process_command,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
