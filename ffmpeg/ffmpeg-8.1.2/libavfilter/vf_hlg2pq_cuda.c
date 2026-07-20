/*
 * CUDA accelerated HLG to PQ HDR conversion filter (hlg2pq_cuda).
 *
 * Pipeline (per frame):
 *   1. Validate input tags: BT.2020 NCL + HLG (ARIB STD-B67).
 *   2. Run CUDA kernels on P010 planes (Y then interleaved UV).
 *   3. Copy timing/SAR/etc. from source, then overwrite color tags and
 *      attach PQ mastering / content-light side data.
 *
 * Hardware path: AV_PIX_FMT_CUDA frames with sw_format P010 (10-bit YUV
 * in 16-bit containers). Kernels are compiled to vf_hlg2pq_cuda.ptx and
 * loaded at config_props time.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <float.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "libavutil/internal.h"

#include "avfilter.h"
#include "filters.h"
#include "pq_metadata.h"
#include "video.h"

#include "cuda/load_helper.h"

/* Host-visible sw formats accepted inside CUDA hw frames. */
static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_P010,
};

#define DIV_UP(a, b) ( ((a) + (b) - 1) / (b) )
#define BLOCKX 32  /* Must match blockDim in vf_hlg2pq_cuda.cu */
#define BLOCKY 16

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)

typedef struct CUDAHlg2pqContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;
    CUcontext cu_ctx;       /* Cached from hwctx, used for Push/PopCurrent */
    CUmodule cu_module;
    CUfunction cu_func_y;   /* Kernel hlg2pq_y  — luma plane */
    CUfunction cu_func_uv;  /* Kernel hlg2pq_uv — chroma plane (P010 plane 1) */
    CUstream cu_stream;

    /* Derived from input hw_frames sw_format (e.g. P010). */
    enum AVPixelFormat in_fmt;
    const AVPixFmtDescriptor *in_desc;
    int in_planes;
    int in_plane_depths[4];   /* Effective bit depth per plane */
    int in_plane_channels[4]; /* Texel channel count (1 for Y, 2 for UV) */

    AVBufferRef *frames_ctx;  /* Output CUDA hw_frames pool */
    float peak_luminance;     /* PQ mastering peak (nits), passed to kernels */
    int pool_size;
    int repair_metadata;      /* Default: repair PQ MDM/CLL; pass through PQ */
    int metadata_logged;      /* Log HDR side data once */
} CUDAHlg2pqContext;

static int format_is_supported(enum AVPixelFormat fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

/* Fill plane depth/channel tables used when creating CUDA texture objects. */
static av_cold void set_format_info(CUDAHlg2pqContext *s,
                                    enum AVPixelFormat format)
{
    int i, p, d;

    memset(s->in_plane_depths, 0, sizeof(s->in_plane_depths));
    memset(s->in_plane_channels, 0, sizeof(s->in_plane_channels));

    s->in_fmt = format;
    s->in_desc = av_pix_fmt_desc_get(format);
    s->in_planes = av_pix_fmt_count_planes(format);

    for (i = 0; i < s->in_desc->nb_components; i++) {
        d = (s->in_desc->comp[i].depth + 7) / 8;
        p = s->in_desc->comp[i].plane;
        s->in_plane_channels[p] =
            FFMAX(s->in_plane_channels[p], s->in_desc->comp[i].step / d);
        s->in_plane_depths[p] = s->in_desc->comp[i].depth;
    }
}

/* Allocate output CUDA frames context (same device, P010 sw_format). */
static av_cold int init_hwframe_ctx(CUDAHlg2pqContext *s,
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
    /* 32-pixel alignment matches other CUDA filters and kernel tiling. */
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

/* Load PTX module and resolve hlg2pq_y / hlg2pq_uv entry points. */
static av_cold int hlg2pq_cuda_load_functions(AVFilterContext *ctx)
{
    CUDAHlg2pqContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_hlg2pq_cuda_ptx_data[];
    extern const unsigned int ff_vf_hlg2pq_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->cu_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_hlg2pq_cuda_ptx_data,
                              ff_vf_hlg2pq_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_y, s->cu_module,
                                           "hlg2pq_y"));
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uv, s->cu_module,
                                           "hlg2pq_uv"));
    if (ret < 0)
        goto fail;

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

/* Bind CUDA device, create output hw_frames, load kernels. */
static av_cold int hlg2pq_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    CUDAHlg2pqContext *s = ctx->priv;
    AVHWFramesContext *frames_ctx;
    enum AVPixelFormat sw_format;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No CUDA hw_frames_ctx on input\n");
        return AVERROR(EINVAL);
    }

    frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    sw_format = frames_ctx->sw_format;

    if (!format_is_supported(sw_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported CUDA sw_format %s; P010 is required\n",
               av_get_pix_fmt_name(sw_format));
        return AVERROR(ENOSYS);
    }

    s->hwctx    = frames_ctx->device_ctx->hwctx;
    s->cu_ctx   = s->hwctx->cuda_ctx;
    s->cu_stream = s->hwctx->stream;
    set_format_info(s, sw_format);

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    ret = init_hwframe_ctx(s, frames_ctx->device_ref, sw_format,
                           inlink->w, inlink->h);
    if (ret < 0)
        return ret;

    ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ol->hw_frames_ctx)
        return AVERROR(ENOMEM);

    ret = hlg2pq_cuda_load_functions(ctx);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_INFO,
           "HLG to PQ CUDA conversion configured: %dx%d %s peak %.1f nits\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(sw_format),
           s->peak_luminance);

    return 0;
}

/*
 * Wrap a device plane as a 2D integer texture (point sampling).
 * plane 0 = full-resolution Y; plane 1 = subsampled interleaved UV.
 */
static int create_plane_texture(AVFilterContext *ctx, AVFrame *src, int plane,
                                CUtexObject *tex)
{
    CUDAHlg2pqContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUDA_TEXTURE_DESC tex_desc = { 0 };
    CUDA_RESOURCE_DESC res_desc = { 0 };
    int width = src->width;
    int height = src->height;

    if (plane) {
        width = AV_CEIL_RSHIFT(src->width, s->in_desc->log2_chroma_w);
        height = AV_CEIL_RSHIFT(src->height, s->in_desc->log2_chroma_h);
    }

    tex_desc.filterMode = CU_TR_FILTER_MODE_POINT;
    tex_desc.flags = CU_TRSF_READ_AS_INTEGER;

    res_desc.resType = CU_RESOURCE_TYPE_PITCH2D;
    res_desc.res.pitch2D.format = CU_AD_FORMAT_UNSIGNED_INT16;
    res_desc.res.pitch2D.numChannels = s->in_plane_channels[plane];
    res_desc.res.pitch2D.width = width;
    res_desc.res.pitch2D.height = height;
    res_desc.res.pitch2D.pitchInBytes = src->linesize[plane];
    res_desc.res.pitch2D.devPtr = (CUdeviceptr)src->data[plane];

    return CHECK_CU(cu->cuTexObjectCreate(tex, &res_desc, &tex_desc, NULL));
}

/* Launch hlg2pq_y or hlg2pq_uv; plane selects dst plane index. */
static int call_hlg2pq_kernel(AVFilterContext *ctx, CUfunction func,
                              CUtexObject src_y_tex, CUtexObject src_uv_tex,
                              AVFrame *dst, int plane,
                              int src_width, int src_height,
                              int width, int height)
{
    CUDAHlg2pqContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUdeviceptr dst_devptr = (CUdeviceptr)dst->data[plane];
    int dst_pitch = dst->linesize[plane];
    int bit_depth = s->in_plane_depths[0];
    int limited_range = 1; /* BT.2020 10-bit limited range in/out */
    void *args[] = {
        &src_y_tex, &src_uv_tex, &dst_devptr, &src_width, &src_height,
        &width, &height, &dst_pitch, &s->peak_luminance, &bit_depth,
        &limited_range,
    };

    return CHECK_CU(cu->cuLaunchKernel(func,
                                       DIV_UP(width, BLOCKX),
                                       DIV_UP(height, BLOCKY),
                                       1, BLOCKX, BLOCKY, 1, 0,
                                       s->cu_stream, args, NULL));
}

/* Bind src textures, launch Y/UV kernels, synchronize the device stream. */
static int hlg2pq_cuda_process(AVFilterContext *ctx, AVFrame *dst,
                               AVFrame *src)
{
    CUDAHlg2pqContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    CUtexObject tex_y = 0, tex_uv = 0;
    int uv_width = AV_CEIL_RSHIFT(src->width, s->in_desc->log2_chroma_w);
    int uv_height = AV_CEIL_RSHIFT(src->height, s->in_desc->log2_chroma_h);
    int ret;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->cu_ctx));
    if (ret < 0)
        return ret;

    ret = create_plane_texture(ctx, src, 0, &tex_y);
    if (ret < 0)
        goto exit;

    ret = create_plane_texture(ctx, src, 1, &tex_uv);
    if (ret < 0)
        goto exit;

    ret = call_hlg2pq_kernel(ctx, s->cu_func_y, tex_y, tex_uv, dst, 0,
                             src->width, src->height,
                             dst->width, dst->height);
    if (ret < 0)
        goto exit;

    ret = call_hlg2pq_kernel(ctx, s->cu_func_uv, tex_y, tex_uv, dst, 1,
                             src->width, src->height,
                             uv_width, uv_height);
    if (ret < 0)
        goto exit;

    ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));

exit:
    if (tex_uv)
        CHECK_CU(cu->cuTexObjectDestroy(tex_uv));
    if (tex_y)
        CHECK_CU(cu->cuTexObjectDestroy(tex_y));
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int hlg2pq_cuda_filter_frame(AVFilterLink *link, AVFrame *src)
{
    /* Pixel processing first; color tags updated only after copy_props. */
    AVFilterContext *ctx = link->dst;
    CUDAHlg2pqContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *ol = ff_filter_link(outlink);
    AVFrame *dst = NULL;
    int ret;

    /*
     * Default: if input is already standard PQ, skip conversion and run the
     * same repair path as vf_repair_pq_metadata (fill missing MDM/CLL).
     */
    if (s->repair_metadata && ff_frame_is_standard_pq(src)) {
        ret = ff_frame_apply_pq_hdr_metadata(ctx, src, s->peak_luminance,
                                             FF_PQ_METADATA_REPAIR_MISSING,
                                             &s->metadata_logged);
        if (ret < 0)
            goto fail;
        return ff_filter_frame(outlink, src);
    }

    if (src->color_trc != AVCOL_TRC_ARIB_STD_B67) {
        av_log(ctx, AV_LOG_ERROR,
               "Input transfer must be HLG/arib-std-b67, got %s\n",
               av_color_transfer_name(src->color_trc));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (src->color_primaries != AVCOL_PRI_BT2020 &&
        !(s->repair_metadata && src->color_primaries == AVCOL_PRI_UNSPECIFIED)) {
        av_log(ctx, AV_LOG_ERROR,
               "Input must be BT.2020 primaries (or unspecified with repair_metadata)\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (src->colorspace != AVCOL_SPC_BT2020_NCL &&
        !(s->repair_metadata && src->colorspace == AVCOL_SPC_UNSPECIFIED)) {
        av_log(ctx, AV_LOG_ERROR,
               "Input must be BT.2020 NCL matrix (or unspecified with repair_metadata)\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

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

    ret = hlg2pq_cuda_process(ctx, dst, src);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(dst, src);
    if (ret < 0)
        goto fail;

    /* HLG→PQ: force PQ tags + MDM/CLL (same helpers as repair_pq_metadata). */
    ret = ff_frame_apply_pq_hdr_metadata(ctx, dst, s->peak_luminance,
                                         FF_PQ_METADATA_FORCE_ALL,
                                         &s->metadata_logged);
    if (ret < 0)
        goto fail;

    av_frame_free(&src);
    return ff_filter_frame(outlink, dst);

fail:
    av_frame_free(&src);
    av_frame_free(&dst);
    return ret;
}

static av_cold void hlg2pq_cuda_uninit(AVFilterContext *ctx)
{
    CUDAHlg2pqContext *s = ctx->priv;

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;

        CHECK_CU(cu->cuCtxPushCurrent(s->cu_ctx));
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        s->cu_module = NULL;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    av_buffer_unref(&s->frames_ctx);
}

#define OFFSET(x) offsetof(CUDAHlg2pqContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption hlg2pq_cuda_options[] = {
    { "peak_luminance", "Target PQ mastering peak luminance in nits",
      OFFSET(peak_luminance), AV_OPT_TYPE_FLOAT, { .dbl = 1000.0 },
      100.0, 10000.0, FLAGS },
    { "pool_size", "CUDA output frame pool size",
      OFFSET(pool_size), AV_OPT_TYPE_INT, { .i64 = 32 },
      8, 128, FLAGS },
    { "repair_metadata", "Repair/fill PQ HDR side data; pass through already-PQ frames",
      OFFSET(repair_metadata), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(hlg2pq_cuda);

static const AVFilterPad hlg2pq_cuda_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = hlg2pq_cuda_filter_frame,
    },
};

static const AVFilterPad hlg2pq_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = hlg2pq_cuda_config_props,
    },
};

const FFFilter ff_vf_hlg2pq_cuda = {
    .p.name         = "hlg2pq_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("CUDA accelerated HLG to PQ HDR conversion"),
    .p.priv_class   = &hlg2pq_cuda_class,
    .priv_size      = sizeof(CUDAHlg2pqContext),
    .uninit         = hlg2pq_cuda_uninit,
    FILTER_INPUTS(hlg2pq_cuda_inputs),
    FILTER_OUTPUTS(hlg2pq_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
