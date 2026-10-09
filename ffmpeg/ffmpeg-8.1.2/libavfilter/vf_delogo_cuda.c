/*
 * CUDA logo remover. Algorithm matches vf_delogo; blur softens the fill.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "video.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, device_hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCK_X 32
#define BLOCK_Y 16

enum {
    PACK_NONE = 0,
    PACK_RGBA,
    PACK_BGRA,
    PACK_RGB0,
    PACK_BGR0,
};

typedef struct FormatInfo {
    enum AVPixelFormat fmt;
    int depth;
    int semi;
    int packed;
} FormatInfo;

/* Same sw_format set as vf_scale_cuda. */
static const FormatInfo supported_formats[] = {
    { AV_PIX_FMT_YUV420P,   8,  0, PACK_NONE },
    { AV_PIX_FMT_YUV422P,   8,  0, PACK_NONE },
    { AV_PIX_FMT_YUV444P,   8,  0, PACK_NONE },
    { AV_PIX_FMT_YUV420P10, 10, 0, PACK_NONE },
    { AV_PIX_FMT_YUV422P10, 10, 0, PACK_NONE },
    { AV_PIX_FMT_YUV444P10, 10, 0, PACK_NONE },
    { AV_PIX_FMT_YUV444P16, 16, 0, PACK_NONE },
    { AV_PIX_FMT_NV12,      8,  1, PACK_NONE },
    { AV_PIX_FMT_NV16,      8,  1, PACK_NONE },
    { AV_PIX_FMT_P010,      10, 1, PACK_NONE },
    { AV_PIX_FMT_P210,      10, 1, PACK_NONE },
    { AV_PIX_FMT_P016,      16, 1, PACK_NONE },
    { AV_PIX_FMT_P216,      16, 1, PACK_NONE },
    { AV_PIX_FMT_0RGB32,    8,  0, PACK_BGR0 },
    { AV_PIX_FMT_0BGR32,    8,  0, PACK_RGB0 },
    { AV_PIX_FMT_RGB32,     8,  0, PACK_BGRA },
    { AV_PIX_FMT_BGR32,     8,  0, PACK_RGBA },
};

typedef struct DelogoCUDAContext {
    const AVClass *class;
    int x, y, w, h;
    int show;
    int blur;

    AVCUDADeviceContext *hwctx;
    enum AVPixelFormat sw_format;
    const FormatInfo *fmt;
    CUmodule cu_module;
    CUfunction cu_copy;
    CUfunction cu_u8, cu_u16;
    CUfunction cu_blur_u8, cu_blur_u16;
    CUfunction cu_scatter_u8, cu_scatter_u16;
    CUdeviceptr scratch;
    int scratch_bytes;
} DelogoCUDAContext;

static const FormatInfo *find_format(enum AVPixelFormat fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i].fmt == fmt)
            return &supported_formats[i];
    return NULL;
}

static int plane_row_bytes(const FormatInfo *fmt, int plane, int width, int hsub)
{
    if (fmt->packed)
        return width * 4;
    const int cw = plane ? AV_CEIL_RSHIFT(width, hsub) : width;
    const int bpp = fmt->depth == 8 ? 1 : 2;
    if (fmt->semi && plane)
        return cw * 2 * bpp;
    return cw * bpp;
}

static av_cold void delogo_cuda_uninit(AVFilterContext *ctx)
{
    DelogoCUDAContext *s = ctx->priv;
    if (!s->hwctx)
        return;

    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    if (cu->cuCtxPushCurrent(s->hwctx->cuda_ctx) >= 0) {
        if (s->scratch)
            cu->cuMemFree(s->scratch);
        if (s->cu_module)
            cu->cuModuleUnload(s->cu_module);
        cu->cuCtxPopCurrent(&dummy);
    }
    s->scratch = 0;
    s->cu_module = NULL;
    s->hwctx = NULL;
}

static int delogo_cuda_load(AVFilterContext *ctx)
{
    DelogoCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_delogo_cuda_ptx_data[];
    extern const unsigned int ff_vf_delogo_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(device_hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, device_hwctx, &s->cu_module,
                              ff_vf_delogo_cuda_ptx_data, ff_vf_delogo_cuda_ptx_len);
    if (ret < 0)
        goto end;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_copy, s->cu_module, "delogo_copy_bytes"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_u8, s->cu_module, "delogo_u8"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_u16, s->cu_module, "delogo_u16"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blur_u8, s->cu_module, "delogo_blur_u8"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blur_u16, s->cu_module, "delogo_blur_u16"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_scatter_u8, s->cu_module, "delogo_scatter_u8"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_scatter_u16, s->cu_module, "delogo_scatter_u16"));

end:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int delogo_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DelogoCUDAContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *frames;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "delogo_cuda 需要 CUDA 硬件帧输入\n");
        return AVERROR(EINVAL);
    }

    frames = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->hwctx = frames->device_ctx->hwctx;
    s->sw_format = frames->sw_format;
    s->fmt = find_format(s->sw_format);
    if (!s->fmt) {
        av_log(ctx, AV_LOG_ERROR, "delogo_cuda 不支持像素格式 %s\n",
               av_get_pix_fmt_name(s->sw_format));
        return AVERROR(EINVAL);
    }

    ret = delogo_cuda_load(ctx);
    if (ret < 0)
        return ret;

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

static int ensure_scratch(AVFilterContext *ctx, int bytes)
{
    DelogoCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    int ret;

    if (bytes <= s->scratch_bytes && s->scratch)
        return 0;
    if (s->scratch) {
        cu->cuMemFree(s->scratch);
        s->scratch = 0;
        s->scratch_bytes = 0;
    }
    ret = CHECK_CU(cu->cuMemAlloc(&s->scratch, bytes));
    if (ret < 0)
        return ret;
    s->scratch_bytes = bytes;
    return 0;
}

static int launch_copy(AVFilterContext *ctx, AVFrame *out, const AVFrame *in)
{
    DelogoCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->sw_format);
    const int nb = s->fmt->packed ? 1 : (s->fmt->semi ? 2 : 3);
    int ret = 0;

    for (int plane = 0; plane < nb; plane++) {
        const int hsub = plane ? desc->log2_chroma_w : 0;
        const int vsub = plane ? desc->log2_chroma_h : 0;
        int row = plane_row_bytes(s->fmt, plane, in->width, hsub);
        int rows = plane ? AV_CEIL_RSHIFT(in->height, vsub) : in->height;
        int src_pitch = in->linesize[plane];
        int dst_pitch = out->linesize[plane];
        CUdeviceptr d_src = (CUdeviceptr)in->data[plane];
        CUdeviceptr d_dst = (CUdeviceptr)out->data[plane];
        void *args[] = { &d_dst, &dst_pitch, &d_src, &src_pitch, &row, &rows };

        if (row <= 0 || rows <= 0 || src_pitch <= 0 || dst_pitch <= 0)
            continue;
        row = FFMIN(row, FFMIN(src_pitch, dst_pitch));
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_copy,
                                          DIV_UP(row, BLOCK_X), DIV_UP(rows, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args, NULL));
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int apply_plane(AVFilterContext *ctx, AVFrame *frame,
                       int plane, int step, int ch,
                       int logo_x, int logo_y, int logo_w, int logo_h,
                       int plane_w, int plane_h, int band, int blur, int sar_num, int sar_den)
{
    DelogoCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    int xclipl, xclipr, yclipt, yclipb;
    int x1, y1, x2, y2;
    int x0, y0, rw, rh;
    int show = s->show;
    int maxv = s->fmt->depth == 8 ? 255 : 65535;
    int pitch = frame->linesize[plane];
    CUdeviceptr d_dst = (CUdeviceptr)frame->data[plane];
    CUfunction fn = s->fmt->depth == 8 ? s->cu_u8 : s->cu_u16;
    int ret;

    if (plane_w < 3 || plane_h < 3 || logo_w < 3 || logo_h < 3)
        return 0;

    xclipl = FFMAX(-logo_x, 0);
    xclipr = FFMAX(logo_x + logo_w - plane_w, 0);
    yclipt = FFMAX(-logo_y, 0);
    yclipb = FFMAX(logo_y + logo_h - plane_h, 0);
    x1 = logo_x + xclipl;
    x2 = logo_x + logo_w - xclipr - 1;
    y1 = logo_y + yclipt;
    y2 = logo_y + logo_h - yclipb - 1;
    if (x2 <= x1 + 1 || y2 <= y1 + 1)
        return 0;

    x0 = x1 + 1;
    y0 = y1 + 1;
    rw = x2 - x0;
    rh = y2 - y0;
    if (rw <= 0 || rh <= 0)
        return 0;

    {
        void *full[] = {
            &d_dst, &pitch, &plane_w, &plane_h, &step, &ch,
            &x0, &y0, &rw, &rh, &x1, &y1, &x2, &y2,
            &logo_x, &logo_y, &logo_w, &logo_h,
            &band, &show, &sar_num, &sar_den, &maxv
        };
        ret = CHECK_CU(cu->cuLaunchKernel(fn,
                                          DIV_UP(rw, BLOCK_X), DIV_UP(rh, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, full, NULL));
        if (ret < 0)
            return ret;
    }

    if (blur > 0) {
        const int inset = show ? 1 : 0;
        int bx0 = x0 + inset;
        int by0 = y0 + inset;
        int bx1 = x2 - inset;
        int by1 = y2 - inset;
        int bw = bx1 - bx0;
        int bh = by1 - by0;
        int spitch = bw;
        int bytes = bw * bh * (s->fmt->depth == 8 ? 1 : 2);
        CUdeviceptr scratch;
        CUfunction bfn = s->fmt->depth == 8 ? s->cu_blur_u8 : s->cu_blur_u16;
        CUfunction sfn = s->fmt->depth == 8 ? s->cu_scatter_u8 : s->cu_scatter_u16;

        if (bw <= 0 || bh <= 0)
            return 0;
        ret = ensure_scratch(ctx, bytes);
        if (ret < 0)
            return ret;
        scratch = s->scratch;
        {
            void *bargs[] = {
                &scratch, &spitch, &d_dst, &pitch, &step, &ch,
                &bx0, &by0, &bw, &bh, &blur
            };
            ret = CHECK_CU(cu->cuLaunchKernel(bfn,
                                              DIV_UP(bw, BLOCK_X), DIV_UP(bh, BLOCK_Y), 1,
                                              BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, bargs, NULL));
            if (ret < 0)
                return ret;
        }
        {
            void *sargs[] = {
                &d_dst, &pitch, &step, &ch, &scratch, &spitch,
                &bx0, &by0, &bw, &bh
            };
            ret = CHECK_CU(cu->cuLaunchKernel(sfn,
                                              DIV_UP(bw, BLOCK_X), DIV_UP(bh, BLOCK_Y), 1,
                                              BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, sargs, NULL));
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static int delogo_cuda_apply(AVFilterContext *ctx, AVFrame *frame)
{
    DelogoCUDAContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->sw_format);
    const int hsub0 = desc->log2_chroma_w;
    const int vsub0 = desc->log2_chroma_h;
    const int band = 1;
    int x = s->x, y = s->y, w = s->w, h = s->h;
    int sar_num, sar_den;
    int ret;

    if (x + (band - 1) <= 0)
        x = 1 + band;
    if (y + (band - 1) <= 0)
        y = 1 + band;
    if (x + w - (band * 2 - 2) > frame->width)
        w = frame->width - x - (band * 2 - 2);
    if (y + h - (band * 2 - 2) > frame->height)
        h = frame->height - y - (band * 2 - 2);
    if (w < 3 || h < 3)
        return 0;

    x -= band;
    y -= band;
    w += band * 2;
    h += band * 2;

    sar_num = frame->sample_aspect_ratio.num;
    sar_den = frame->sample_aspect_ratio.den;
    if (sar_num <= 0 || sar_den <= 0)
        sar_num = sar_den = 1;

    if (s->fmt->packed) {
        const int nch = (s->fmt->packed == PACK_RGB0 || s->fmt->packed == PACK_BGR0) ? 3 : 4;
        for (int ch = 0; ch < nch; ch++) {
            ret = apply_plane(ctx, frame, 0, 4, ch, x, y, w, h,
                              frame->width, frame->height, band, s->blur, sar_num, sar_den);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    {
        const int nb = s->fmt->semi ? 2 : 3;
        for (int plane = 0; plane < nb; plane++) {
            const int hsub = plane ? hsub0 : 0;
            const int vsub = plane ? vsub0 : 0;
            const int pw = AV_CEIL_RSHIFT(frame->width, hsub);
            const int ph = AV_CEIL_RSHIFT(frame->height, vsub);
            const int px = x >> hsub;
            const int py = y >> vsub;
            const int pwlogo = AV_CEIL_RSHIFT(w + (x & ((1 << hsub) - 1)), hsub);
            const int phlogo = AV_CEIL_RSHIFT(h + (y & ((1 << vsub) - 1)), vsub);
            const int band_p = band >> FFMIN(hsub, vsub);
            int blur_p = s->blur >> FFMIN(hsub, vsub);
            const int step = (s->fmt->semi && plane) ? 2 : 1;
            const int nch = (s->fmt->semi && plane) ? 2 : 1;

            if (s->blur > 0 && blur_p < 1)
                blur_p = 1;
            for (int ch = 0; ch < nch; ch++) {
                ret = apply_plane(ctx, frame, plane, step, ch,
                                  px, py, pwlogo, phlogo, pw, ph,
                                  band_p > 0 ? band_p : (plane ? 0 : band),
                                  blur_p, sar_num, sar_den);
                if (ret < 0)
                    return ret;
            }
        }
    }
    return 0;
}

static int delogo_cuda_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DelogoCUDAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *outl = ff_filter_link(outlink);
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    AVFrame *out = NULL;
    CUcontext dummy;
    int ret, direct;

    if (s->w <= 0 || s->h <= 0)
        return ff_filter_frame(outlink, in);

    direct = av_frame_is_writable(in);
    if (direct) {
        out = in;
    } else {
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
        av_frame_copy_props(out, in);
    }

    ret = CHECK_CU(device_hwctx->internal->cuda_dl->cuCtxPushCurrent(device_hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    if (!direct) {
        ret = launch_copy(ctx, out, in);
        if (ret < 0)
            goto pop;
    }

    ret = delogo_cuda_apply(ctx, out);

pop:
    CHECK_CU(device_hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;
    if (!direct)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    if (out != in)
        av_frame_free(&out);
    av_frame_free(&in);
    return ret < 0 ? ret : AVERROR_EXTERNAL;
}

#define OFFSET(x) offsetof(DelogoCUDAContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define FLAGSR (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption delogo_cuda_options[] = {
    { "x",    "logo left",   OFFSET(x),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "y",    "logo top",    OFFSET(y),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "w",    "logo width",  OFFSET(w),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "h",    "logo height", OFFSET(h),    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "show", "draw the logo rectangle", OFFSET(show), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGSR },
    { "blur", "box-blur radius of the repaired area, 0 = classic interpolation", OFFSET(blur), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 32, FLAGSR },
    { NULL }
};

AVFILTER_DEFINE_CLASS(delogo_cuda);

static const AVFilterPad inputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .filter_frame = delogo_cuda_filter_frame,
}};

static const AVFilterPad outputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = delogo_cuda_config_props,
}};

const FFFilter ff_vf_delogo_cuda = {
    .p.name        = "delogo_cuda",
    .p.description = NULL_IF_CONFIG_SMALL("Remove a logo on CUDA frames."),
    .p.priv_class  = &delogo_cuda_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size     = sizeof(DelogoCUDAContext),
    .uninit        = delogo_cuda_uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .process_command = ff_filter_process_command,
};
