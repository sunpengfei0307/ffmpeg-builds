/*
 * CUDA bidirectional tonemap filter (libplacebo-inspired parameters).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <float.h>
#include <math.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/internal.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "colorspace.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "vf_tonemap_cuda.h"
#include "video.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCK_X 32
#define BLOCK_Y 16

#define FMT_AUTO (-1)
#define FMT_SAME (-2)

typedef struct CUDATonemapContext {
    const AVClass *class;

    int mode_opt;
    int tonemap;
    double param;
    double desat;
    double peak;             /* HDR-side peak (relative = nits/100), 0=auto */
    double threshold;        /* scene-change threshold for dynamic reset */
    double exposure;
    double knee;
    double reinhard_contrast;
    double spline_contrast;
    double knee_offset;
    double brightness;
    double contrast;
    double saturation;
    double sharpen;          /* luma unsharp amount, 0=off, ~0.35 moderate */
    double hlg_peak;         /* HLG reference peak in nits, default 1000 */
    int gamut_mode;
    int dynamic;             /* 1=measure peak/avg and adapt (opencl-style) */
    double dst_avg;          /* target average for dynamic exposure (rel) */

    /* Output color tags (-1 = mode default). */
    int trc_opt;
    int colorspace_opt;
    int primaries_opt;
    int range_opt;
    int format_opt;

    /* Input color tags (-1 = from frame, else HDR defaults to PQ/bt2020). */
    int src_trc_opt;
    int src_colorspace_opt;
    int src_primaries_opt;

    enum AVPixelFormat in_fmt, out_fmt;
    int resolved_mode;

    enum AVColorTransferCharacteristic trc_in, trc_out;
    enum AVColorSpace colorspace_in, colorspace_out;
    enum AVColorPrimaries primaries_in, primaries_out;
    enum AVColorRange range_in, range_out;

    TonemapCUDAParams gpu_params;

    AVBufferRef *frames_ctx;
    AVCUDADeviceContext *hwctx;
    CUmodule cu_module;
    CUfunction cu_func;
    CUfunction cu_func_sharpen;
    CUfunction cu_func_analyze;
    CUdeviceptr sharpen_buf;
    size_t sharpen_buf_size;
    CUdeviceptr analyze_buf;
    size_t analyze_buf_size;
    TonemapCUDAAnalyzePartial *analyze_host;
    int analyze_npartial;

    /* Temporal dynamic state (relative linear, nits/100). */
    int dynamic_ready;
    double smooth_peak;
    double smooth_avg;
} CUDATonemapContext;

#define TONEMAP_CUDA_DYNAMIC_PEAK_MIN 1.5   /* ~150 nits */
#define TONEMAP_CUDA_DYNAMIC_PEAK_MAX 100.0 /* ~10000 nits */
#define TONEMAP_CUDA_DYNAMIC_EMA      0.18

static int format_supported(enum AVPixelFormat fmt)
{
    return fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010;
}

static int trc_to_cuda(enum AVColorTransferCharacteristic trc)
{
    switch (trc) {
    case AVCOL_TRC_SMPTE2084:    return TONEMAP_CUDA_TRC_PQ;
    case AVCOL_TRC_ARIB_STD_B67: return TONEMAP_CUDA_TRC_HLG;
    case AVCOL_TRC_IEC61966_2_1: return TONEMAP_CUDA_TRC_SRGB;
    case AVCOL_TRC_BT2020_10:    return TONEMAP_CUDA_TRC_BT2020_10;
    case AVCOL_TRC_BT709:
    default:                     return TONEMAP_CUDA_TRC_BT709;
    }
}

static int is_hdr_trc(enum AVColorTransferCharacteristic trc)
{
    return trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67;
}

static int is_sdr_trc(enum AVColorTransferCharacteristic trc)
{
    switch (trc) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
    case AVCOL_TRC_IEC61966_2_1:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT1361_ECG:
    case AVCOL_TRC_IEC61966_2_4:
        return 1;
    default:
        return 0;
    }
}

static const char *tonemap_name(int algo)
{
    switch (algo) {
    case TONEMAP_CUDA_AUTO:     return "auto";
    case TONEMAP_CUDA_CLIP:     return "clip";
    case TONEMAP_CUDA_BT2390:   return "bt.2390";
    case TONEMAP_CUDA_BT2446A:  return "bt.2446a";
    case TONEMAP_CUDA_SPLINE:   return "spline";
    case TONEMAP_CUDA_REINHARD: return "reinhard";
    case TONEMAP_CUDA_MOBIUS:   return "mobius";
    case TONEMAP_CUDA_HABLE:    return "hable";
    case TONEMAP_CUDA_GAMMA:    return "gamma";
    case TONEMAP_CUDA_LINEAR:   return "linear";
    case TONEMAP_CUDA_NONE:     return "none";
    default:                    return "?";
    }
}

static int check_sdr2hdr_algo(AVFilterContext *ctx, int algo)
{
    switch (algo) {
    case TONEMAP_CUDA_AUTO:
    case TONEMAP_CUDA_NONE:
    case TONEMAP_CUDA_LINEAR:
    case TONEMAP_CUDA_HABLE:
    case TONEMAP_CUDA_REINHARD:
    case TONEMAP_CUDA_GAMMA:
    case TONEMAP_CUDA_SPLINE:
    case TONEMAP_CUDA_BT2446A:
        return 0;
    case TONEMAP_CUDA_CLIP:
    case TONEMAP_CUDA_MOBIUS:
    case TONEMAP_CUDA_BT2390:
        av_log(ctx, AV_LOG_ERROR,
               "tonemap=%s does not support sdr2hdr (not invertible)\n",
               tonemap_name(algo));
        return AVERROR(EINVAL);
    default:
        av_log(ctx, AV_LOG_ERROR, "unknown tonemap algorithm %d\n", algo);
        return AVERROR(EINVAL);
    }
}

static int get_rgb2rgb_matrix(enum AVColorPrimaries in, enum AVColorPrimaries out,
                              double rgb2rgb[3][3])
{
    double rgb2xyz[3][3], xyz2rgb[3][3];
    const AVColorPrimariesDesc *in_p  = av_csp_primaries_desc_from_id(in);
    const AVColorPrimariesDesc *out_p = av_csp_primaries_desc_from_id(out);

    if (!in_p || !out_p)
        return AVERROR(EINVAL);

    ff_fill_rgb2xyz_table(&out_p->prim, &out_p->wp, rgb2xyz);
    ff_matrix_invert_3x3(rgb2xyz, xyz2rgb);
    ff_fill_rgb2xyz_table(&in_p->prim, &in_p->wp, rgb2xyz);
    ff_matrix_mul_3x3(rgb2rgb, rgb2xyz, xyz2rgb);
    return 0;
}

static void dmat_to_f9(const double m[3][3], float *out)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 3 + j] = (float)m[i][j];
}

static int prepare_matrices(AVFilterContext *ctx)
{
    CUDATonemapContext *s = ctx->priv;
    const AVLumaCoefficients *luma_src, *luma_dst;
    double rgb2yuv[3][3], yuv2rgb[3][3], rgb2rgb[3][3];
    int ret;

    luma_src = av_csp_luma_coeffs_from_avcsp(s->colorspace_in);
    luma_dst = av_csp_luma_coeffs_from_avcsp(s->colorspace_out);
    if (!luma_src || !luma_dst) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported colorspace in/out\n");
        return AVERROR(EINVAL);
    }

    ff_fill_rgb2yuv_table(luma_src, rgb2yuv);
    ff_matrix_invert_3x3(rgb2yuv, yuv2rgb);
    dmat_to_f9(yuv2rgb, s->gpu_params.yuv2rgb);

    ff_fill_rgb2yuv_table(luma_dst, rgb2yuv);
    dmat_to_f9(rgb2yuv, s->gpu_params.rgb2yuv);

    s->gpu_params.luma_src[0] = av_q2d(luma_src->cr);
    s->gpu_params.luma_src[1] = av_q2d(luma_src->cg);
    s->gpu_params.luma_src[2] = av_q2d(luma_src->cb);
    s->gpu_params.luma_dst[0] = av_q2d(luma_dst->cr);
    s->gpu_params.luma_dst[1] = av_q2d(luma_dst->cg);
    s->gpu_params.luma_dst[2] = av_q2d(luma_dst->cb);

    if (s->primaries_in != s->primaries_out) {
        ret = get_rgb2rgb_matrix(s->primaries_in, s->primaries_out, rgb2rgb);
        if (ret < 0)
            return ret;
        dmat_to_f9(rgb2rgb, s->gpu_params.rgb2rgb);
        s->gpu_params.rgb2rgb_passthrough = 0;
    } else {
        s->gpu_params.rgb2rgb_passthrough = 1;
    }

    return 0;
}

static int resolve_mode_and_defaults(AVFilterContext *ctx, const AVFrame *in)
{
    CUDATonemapContext *s = ctx->priv;
    int mode = s->mode_opt;
    enum AVColorTransferCharacteristic trc_hint;

    /*
     * Input transfer:
     *   1) src_transfer= override
     *   2) else frame auto-detect (color_trc)
     *   3) else assume PQ (smpte2084)
     */
    if (s->src_trc_opt >= 0) {
        s->trc_in = s->src_trc_opt;
    } else if (in->color_trc != AVCOL_TRC_UNSPECIFIED) {
        s->trc_in = in->color_trc;
    } else {
        s->trc_in = AVCOL_TRC_SMPTE2084;
        av_log(ctx, AV_LOG_WARNING,
               "input transfer unspecified; assume PQ (smpte2084). "
               "Use src_transfer= for HLG/SDR.\n");
    }
    trc_hint = s->trc_in;

    if (mode == TONEMAP_CUDA_MODE_AUTO) {
        if (is_hdr_trc(trc_hint))
            mode = TONEMAP_CUDA_MODE_HDR2SDR;
        else if (is_sdr_trc(trc_hint))
            mode = TONEMAP_CUDA_MODE_SDR2HDR;
        else {
            av_log(ctx, AV_LOG_ERROR,
                   "Cannot infer mode from transfer %s; set mode= or src_transfer=\n",
                   av_color_transfer_name(trc_hint));
            return AVERROR(EINVAL);
        }
    }

    if (mode == TONEMAP_CUDA_MODE_SDR2HDR) {
        int ret = check_sdr2hdr_algo(ctx, s->tonemap);
        if (ret < 0)
            return ret;
        if (is_hdr_trc(s->trc_in) && s->src_trc_opt < 0 &&
            in->color_trc == AVCOL_TRC_UNSPECIFIED)
            av_log(ctx, AV_LOG_WARNING,
                   "mode=sdr2hdr but input assumed PQ; set src_transfer=bt709 if source is SDR\n");
    }

    s->resolved_mode = mode;

    /* Input matrix / primaries: override ?frame ?match transfer (HDR?bt2020, else bt709) */
    if (s->src_colorspace_opt >= 0) {
        s->colorspace_in = s->src_colorspace_opt;
    } else if (in->colorspace != AVCOL_SPC_UNSPECIFIED) {
        s->colorspace_in = in->colorspace;
    } else {
        s->colorspace_in = is_hdr_trc(s->trc_in) ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    }

    if (s->src_primaries_opt >= 0) {
        s->primaries_in = s->src_primaries_opt;
    } else if (in->color_primaries != AVCOL_PRI_UNSPECIFIED) {
        s->primaries_in = in->color_primaries;
    } else {
        s->primaries_in = is_hdr_trc(s->trc_in) ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    }

    s->range_in = in->color_range == AVCOL_RANGE_UNSPECIFIED ?
                  AVCOL_RANGE_MPEG : in->color_range;

    /*
     * Output defaults:
     *   hdr2sdr ?transfer=bt709:matrix=bt709:primaries=bt709
     *   sdr2hdr ?transfer=smpte2084:matrix=bt2020:primaries=bt2020  (PQ)
     *             or user sets transfer=hlg / arib-std-b67
     */
    if (mode == TONEMAP_CUDA_MODE_HDR2SDR) {
        s->trc_out        = s->trc_opt >= 0 ? s->trc_opt : AVCOL_TRC_BT709;
        s->colorspace_out = s->colorspace_opt >= 0 ? s->colorspace_opt : AVCOL_SPC_BT709;
        s->primaries_out  = s->primaries_opt >= 0 ? s->primaries_opt : AVCOL_PRI_BT709;
    } else {
        s->trc_out        = s->trc_opt >= 0 ? s->trc_opt : AVCOL_TRC_SMPTE2084;
        s->colorspace_out = s->colorspace_opt >= 0 ? s->colorspace_opt : AVCOL_SPC_BT2020_NCL;
        s->primaries_out  = s->primaries_opt >= 0 ? s->primaries_opt : AVCOL_PRI_BT2020;
    }

    if (s->range_opt >= 0)
        s->range_out = s->range_opt;
    else
        s->range_out = s->range_in;

    if (s->format_opt == FMT_SAME) {
        s->out_fmt = s->in_fmt;
    } else if (s->format_opt == FMT_AUTO || s->format_opt == AV_PIX_FMT_NONE) {
        s->out_fmt = (mode == TONEMAP_CUDA_MODE_HDR2SDR) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010;
    } else {
        s->out_fmt = s->format_opt;
    }

    if (!format_supported(s->out_fmt)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported output format\n");
        return AVERROR(ENOTSUP);
    }

    return 0;
}

static int apply_param_defaults(AVFilterContext *ctx)
{
    CUDATonemapContext *s = ctx->priv;

    switch (s->tonemap) {
    case TONEMAP_CUDA_GAMMA:
        if (isnan(s->param))
            s->param = 1.8;
        break;
    case TONEMAP_CUDA_REINHARD:
        /* Keep user param as contrast-like; kernel uses reinhard_contrast. */
        if (!isnan(s->param) && s->param > 0.0 && s->param < 1.0)
            s->reinhard_contrast = s->param;
        break;
    case TONEMAP_CUDA_MOBIUS:
        if (isnan(s->param))
            s->param = 0.3;
        if (s->knee <= 0.0)
            s->knee = s->param;
        break;
    case TONEMAP_CUDA_SPLINE:
    case TONEMAP_CUDA_AUTO:
        if (s->knee <= 0.0)
            s->knee = 0.3;
        break;
    }
    if (isnan(s->param))
        s->param = 1.0;
    return 0;
}

static int alloc_out_frames_ctx(AVFilterContext *ctx, int w, int h)
{
    CUDATonemapContext *s = ctx->priv;
    FilterLink *inl = ff_filter_link(ctx->inputs[0]);
    AVHWFramesContext *in_fc = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    AVBufferRef *ref;
    AVHWFramesContext *out_fc;
    int ret;

    ref = av_hwframe_ctx_alloc(in_fc->device_ref);
    if (!ref)
        return AVERROR(ENOMEM);

    out_fc = (AVHWFramesContext *)ref->data;
    out_fc->format    = AV_PIX_FMT_CUDA;
    out_fc->sw_format = s->out_fmt;
    out_fc->width     = FFALIGN(w, 32);
    out_fc->height    = FFALIGN(h, 32);

    ret = av_hwframe_ctx_init(ref);
    if (ret < 0) {
        av_buffer_unref(&ref);
        return ret;
    }

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = ref;
    return 0;
}

static av_cold int tonemap_cuda_load(AVFilterContext *ctx)
{
    CUDATonemapContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_tonemap_cuda_ptx_data[];
    extern const unsigned int ff_vf_tonemap_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_tonemap_cuda_ptx_data, ff_vf_tonemap_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func, s->cu_module, "tonemap_cuda"));
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_sharpen, s->cu_module,
                                           "tonemap_cuda_sharpen_y"));
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_analyze, s->cu_module,
                                           "tonemap_cuda_analyze"));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold int tonemap_cuda_init(AVFilterContext *ctx)
{
    return apply_param_defaults(ctx);
}

static av_cold void tonemap_cuda_uninit(AVFilterContext *ctx)
{
    CUDATonemapContext *s = ctx->priv;

    if (s->hwctx && (s->cu_module || s->sharpen_buf || s->analyze_buf)) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (s->sharpen_buf) {
            CHECK_CU(cu->cuMemFree(s->sharpen_buf));
            s->sharpen_buf = 0;
            s->sharpen_buf_size = 0;
        }
        if (s->analyze_buf) {
            CHECK_CU(cu->cuMemFree(s->analyze_buf));
            s->analyze_buf = 0;
            s->analyze_buf_size = 0;
        }
        if (s->cu_module)
            CHECK_CU(cu->cuModuleUnload(s->cu_module));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }
    s->cu_module = NULL;
    av_freep(&s->analyze_host);
    s->analyze_npartial = 0;
    av_buffer_unref(&s->frames_ctx);
}

static int tonemap_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    CUDATonemapContext *s = ctx->priv;
    AVHWFramesContext *in_fc;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    in_fc = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->hwctx = in_fc->device_ctx->hwctx;
    s->in_fmt = in_fc->sw_format;

    if (!format_supported(s->in_fmt)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input sw_format: %s (need nv12/p010)\n",
               av_get_pix_fmt_name(s->in_fmt));
        return AVERROR(ENOTSUP);
    }

    if (s->format_opt == FMT_SAME)
        s->out_fmt = s->in_fmt;
    else if (s->format_opt > 0)
        s->out_fmt = s->format_opt;
    else if (s->mode_opt == TONEMAP_CUDA_MODE_SDR2HDR)
        s->out_fmt = AV_PIX_FMT_P010;
    else
        s->out_fmt = AV_PIX_FMT_NV12;

    ret = alloc_out_frames_ctx(ctx, inlink->w, inlink->h);
    if (ret < 0)
        return ret;

    ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ol->hw_frames_ctx)
        return AVERROR(ENOMEM);

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base = inlink->time_base;

    return tonemap_cuda_load(ctx);
}

static void update_hdr_sidedata(AVFrame *out, int mode, double peak_rel)
{
    if (mode == TONEMAP_CUDA_MODE_HDR2SDR) {
        /* Drop HDR10 side data copied from the source ?leaving BT.2020
         * mastering/CLL on an SDR frame can make players re-tint the image. */
        av_frame_remove_side_data(out, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_remove_side_data(out, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        av_frame_remove_side_data(out, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
        av_frame_remove_side_data(out, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
        return;
    }

    {
        AVMasteringDisplayMetadata *mdm;
        AVContentLightMetadata *cll;
        double nits = peak_rel > 0 ? peak_rel * REFERENCE_WHITE : 1000.0;

        mdm = av_mastering_display_metadata_create_side_data(out);
        if (mdm) {
            mdm->has_luminance = 1;
            mdm->max_luminance = av_d2q(nits, 10000);
            mdm->min_luminance = av_d2q(0.0001, 10000);
            mdm->has_primaries = 1;
            mdm->display_primaries[0][0] = av_d2q(0.708, 100000);
            mdm->display_primaries[0][1] = av_d2q(0.292, 100000);
            mdm->display_primaries[1][0] = av_d2q(0.170, 100000);
            mdm->display_primaries[1][1] = av_d2q(0.797, 100000);
            mdm->display_primaries[2][0] = av_d2q(0.131, 100000);
            mdm->display_primaries[2][1] = av_d2q(0.046, 100000);
            mdm->white_point[0] = av_d2q(0.3127, 100000);
            mdm->white_point[1] = av_d2q(0.3290, 100000);
        }
        cll = av_content_light_metadata_create_side_data(out);
        if (cll) {
            cll->MaxCLL  = (unsigned)FFMIN(nits, 65535);
            cll->MaxFALL = (unsigned)FFMIN(nits * 0.25, 65535);
        }
    }
}

/*
 * Measure frame peak/avg (subsampled Y->linear). Per-block partials on GPU,
 * reduce on host (avoids atomicMax — unavailable in this clang CUDA).
 */
static int run_dynamic_analyze(AVFilterContext *ctx, CUdeviceptr src_y,
                               int src_y_pitch, int width, int height,
                               const TonemapCUDAParams *tp,
                               double *cur_peak, double *cur_avg)
{
    CUDATonemapContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    void *args[11];
    int stride = 4;
    int gw = DIV_UP(DIV_UP(width, stride), BLOCK_X);
    int gh = DIV_UP(DIV_UP(height, stride), BLOCK_Y);
    int npartial = gw * gh;
    size_t bytes;
    int trc = tp->trc_in;
    int bit_depth = tp->src_bit_depth;
    int full_range = tp->full_range_in;
    float hlg_peak = tp->hlg_peak_nits;
    double peak = 0.0, sum = 0.0;
    unsigned int count = 0;
    int i, ret;

    if (npartial <= 0)
        return AVERROR(EINVAL);

    bytes = (size_t)npartial * sizeof(TonemapCUDAAnalyzePartial);
    if (bytes > s->analyze_buf_size) {
        if (s->analyze_buf) {
            ret = CHECK_CU(cu->cuMemFree(s->analyze_buf));
            if (ret < 0)
                return ret;
            s->analyze_buf = 0;
            s->analyze_buf_size = 0;
        }
        ret = CHECK_CU(cu->cuMemAlloc(&s->analyze_buf, bytes));
        if (ret < 0)
            return ret;
        s->analyze_buf_size = bytes;
    }

    if (npartial > s->analyze_npartial) {
        av_freep(&s->analyze_host);
        s->analyze_host = av_malloc_array(npartial, sizeof(*s->analyze_host));
        if (!s->analyze_host) {
            s->analyze_npartial = 0;
            return AVERROR(ENOMEM);
        }
        s->analyze_npartial = npartial;
    }

    args[0] = &src_y;
    args[1] = &src_y_pitch;
    args[2] = &width;
    args[3] = &height;
    args[4] = &bit_depth;
    args[5] = &full_range;
    args[6] = &trc;
    args[7] = &hlg_peak;
    args[8] = &s->analyze_buf;
    args[9] = &npartial;
    args[10] = &gw;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_analyze,
                                      gw, gh, 1,
                                      1, 1, 1,
                                      0, s->hwctx->stream, args, NULL));
    if (ret < 0)
        return ret;

    ret = CHECK_CU(cu->cuMemcpyDtoHAsync(s->analyze_host, s->analyze_buf, bytes,
                                         s->hwctx->stream));
    if (ret < 0)
        return ret;
    ret = CHECK_CU(cu->cuStreamSynchronize(s->hwctx->stream));
    if (ret < 0)
        return ret;

    for (i = 0; i < npartial; i++) {
        peak = FFMAX(peak, (double)s->analyze_host[i].peak);
        sum += (double)s->analyze_host[i].sum;
        count += s->analyze_host[i].count;
    }
    if (count == 0)
        return AVERROR(EINVAL);

    *cur_peak = peak;
    *cur_avg  = sum / (double)count;
    return 0;
}

static void update_dynamic_smooth(CUDATonemapContext *s, double cur_peak,
                                  double cur_avg)
{
    double thr = s->threshold > 0.0 ? s->threshold : 0.2;
    int scene_cut = 0;

    cur_peak = av_clipd(cur_peak, TONEMAP_CUDA_DYNAMIC_PEAK_MIN,
                        TONEMAP_CUDA_DYNAMIC_PEAK_MAX);
    cur_avg  = av_clipd(cur_avg, 1e-4, TONEMAP_CUDA_DYNAMIC_PEAK_MAX);

    if (!s->dynamic_ready) {
        s->smooth_peak = cur_peak;
        s->smooth_avg  = cur_avg;
        s->dynamic_ready = 1;
        return;
    }

    /* Relative average jump ? scene cut (same idea as tonemap_opencl). */
    if (fabs(cur_avg - s->smooth_avg) >
        thr * FFMAX(s->smooth_avg, 0.05))
        scene_cut = 1;

    if (scene_cut) {
        s->smooth_peak = cur_peak;
        s->smooth_avg  = cur_avg;
        return;
    }

    /* Peak rises fast, falls slow  avoid flicker on specular hits. */
    if (cur_peak > s->smooth_peak)
        s->smooth_peak = s->smooth_peak * 0.55 + cur_peak * 0.45;
    else
        s->smooth_peak = s->smooth_peak * (1.0 - TONEMAP_CUDA_DYNAMIC_EMA) +
                         cur_peak * TONEMAP_CUDA_DYNAMIC_EMA;
    s->smooth_avg = s->smooth_avg * (1.0 - TONEMAP_CUDA_DYNAMIC_EMA) +
                    cur_avg * TONEMAP_CUDA_DYNAMIC_EMA;
}

static void fill_gpu_params(CUDATonemapContext *s, TonemapCUDAParams *p,
                            double src_peak, double dst_peak)
{
    *p = s->gpu_params;
    p->src_peak = (float)src_peak;
    p->dst_peak = (float)dst_peak;
    p->param = (float)s->param;
    p->desat = (float)s->desat;
    p->exposure = (float)s->exposure;
    p->knee = (float)s->knee;
    p->reinhard_contrast = (float)s->reinhard_contrast;
    p->spline_contrast = (float)s->spline_contrast;
    p->knee_offset = (float)s->knee_offset;
    p->brightness = (float)s->brightness;
    p->contrast = (float)s->contrast;
    p->saturation = (float)s->saturation;
    p->hlg_peak_nits = (float)s->hlg_peak;
    p->tonemap = s->tonemap;
    p->mode = s->resolved_mode;
    p->gamut_mode = s->gamut_mode;
    p->trc_in  = trc_to_cuda(s->trc_in);
    p->trc_out = trc_to_cuda(s->trc_out);
    p->full_range_in  = s->range_in == AVCOL_RANGE_JPEG;
    p->full_range_out = s->range_out == AVCOL_RANGE_JPEG;
    p->src_bit_depth = (s->in_fmt == AV_PIX_FMT_P010) ? 10 : 8;
    p->dst_bit_depth = (s->out_fmt == AV_PIX_FMT_P010) ? 10 : 8;
}

static int tonemap_cuda_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *ol = ff_filter_link(outlink);
    CUDATonemapContext *s = ctx->priv;
    AVFrame *out = NULL;
    TonemapCUDAParams p;
    CUcontext dummy;
    CUdeviceptr src_y, src_uv, dst_y, dst_uv;
    int src_y_pitch, src_uv_pitch, dst_y_pitch, dst_uv_pitch;
    int width, height;
    double src_peak, dst_peak;
    int ret;
    void *args[11];

    ret = resolve_mode_and_defaults(ctx, in);
    if (ret < 0)
        goto fail;

    if (s->out_fmt != ((AVHWFramesContext *)s->frames_ctx->data)->sw_format) {
        ret = alloc_out_frames_ctx(ctx, inlink->w, inlink->h);
        if (ret < 0)
            goto fail;
        av_buffer_unref(&ol->hw_frames_ctx);
        ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
        if (!ol->hw_frames_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }

    ret = prepare_matrices(ctx);
    if (ret < 0)
        goto fail;

    /* Single option `peak` = HDR-side relative luminance (nits/100). 0 = auto. */
    {
        double hdr_peak = s->peak;
        if (hdr_peak <= 0)
            hdr_peak = ff_determine_signal_peak(in);
        if (hdr_peak <= 0)
            hdr_peak = 10.0; /* ~1000 nits */
        /*
         * Without dynamic: understated MaxCLL/MDL leaves some curves washed 
         * floor auto peak to ~1000 nits. With dynamic, measured peak replaces this.
         */
        if (!s->dynamic && s->peak <= 0 &&
            s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR && hdr_peak < 10.0)
            hdr_peak = 10.0;

        if (s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR) {
            src_peak = hdr_peak;
            dst_peak = 1.0;   /* SDR ~100 nits */
        } else {
            src_peak = 1.0;   /* SDR side */
            dst_peak = hdr_peak;
        }
    }

    fill_gpu_params(s, &p, src_peak, dst_peak);

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = av_hwframe_get_buffer(ol->hw_frames_ctx, out, 0);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->color_trc = s->trc_out;
    out->colorspace = s->colorspace_out;
    out->color_primaries = s->primaries_out;
    out->color_range = s->range_out;
    out->width = in->width;
    out->height = in->height;

    src_y  = (CUdeviceptr)in->data[0];
    src_uv = (CUdeviceptr)in->data[1];
    dst_y  = (CUdeviceptr)out->data[0];
    dst_uv = (CUdeviceptr)out->data[1];

    if (p.src_bit_depth > 8) {
        src_y_pitch  = in->linesize[0] / 2;
        src_uv_pitch = in->linesize[1] / 4;
    } else {
        src_y_pitch  = in->linesize[0];
        src_uv_pitch = in->linesize[1] / 2;
    }
    if (p.dst_bit_depth > 8) {
        dst_y_pitch  = out->linesize[0] / 2;
        dst_uv_pitch = out->linesize[1] / 4;
    } else {
        dst_y_pitch  = out->linesize[0];
        dst_uv_pitch = out->linesize[1] / 2;
    }

    width  = in->width;
    height = in->height;

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    /* Dynamic peak/avg (hdr2sdr): lift dark scenes, track real content peak. */
    if (s->dynamic && s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR &&
        s->cu_func_analyze) {
        double cur_peak = 0, cur_avg = 0;
        ret = run_dynamic_analyze(ctx, src_y, src_y_pitch, width, height, &p,
                                  &cur_peak, &cur_avg);
        if (ret >= 0) {
            double slope, target;
            update_dynamic_smooth(s, cur_peak, cur_avg);
            if (s->peak <= 0)
                src_peak = s->smooth_peak;
            /*
             * opencl: slope = min(1, sdr_avg/average) only darkens bright scenes.
             * Also allow mild lift when average is low (dark HDR stages).
             */
            target = s->dst_avg > 0.0 ? s->dst_avg : 0.25;
            slope = target / FFMAX(s->smooth_avg, 1e-4);
            slope = av_clipd(slope, 0.70, 1.55);
            p.src_peak = (float)src_peak;
            p.exposure = (float)(s->exposure * slope);
            av_log(ctx, AV_LOG_DEBUG,
                   "tonemap_cuda dynamic peak=%.3f avg=%.3f smooth_peak=%.3f "
                   "smooth_avg=%.3f slope=%.3f exposure=%.3f\n",
                   cur_peak, cur_avg, s->smooth_peak, s->smooth_avg,
                   slope, (double)p.exposure);
        } else {
            av_log(ctx, AV_LOG_WARNING,
                   "tonemap_cuda dynamic analyze failed (%d), using static peak\n",
                   ret);
            ret = 0;
        }
    }

    av_log(ctx, AV_LOG_DEBUG,
           "tonemap_cuda mode=%s algo=%s "
           "src[t=%s m=%s p=%s]->dst[t=%s m=%s p=%s] peak=%.3f format=%s->%s\n",
           s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR ? "hdr2sdr" : "sdr2hdr",
           tonemap_name(s->tonemap),
           av_color_transfer_name(s->trc_in),
           av_color_space_name(s->colorspace_in),
           av_color_primaries_name(s->primaries_in),
           av_color_transfer_name(s->trc_out),
           av_color_space_name(s->colorspace_out),
           av_color_primaries_name(s->primaries_out),
           s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR ? src_peak : dst_peak,
           av_get_pix_fmt_name(s->in_fmt), av_get_pix_fmt_name(s->out_fmt));

    args[0] = &src_y;
    args[1] = &src_uv;
    args[2] = &dst_y;
    args[3] = &dst_uv;
    args[4] = &src_y_pitch;
    args[5] = &src_uv_pitch;
    args[6] = &dst_y_pitch;
    args[7] = &dst_uv_pitch;
    args[8] = &width;
    args[9] = &height;
    args[10] = &p;

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuLaunchKernel(
                       s->cu_func,
                       DIV_UP(width, BLOCK_X), DIV_UP(height, BLOCK_Y), 1,
                       BLOCK_X, BLOCK_Y, 1,
                       0, s->hwctx->stream, args, NULL));
    if (ret < 0) {
        CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
        goto fail;
    }

    if (s->sharpen > 0.0) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        float sharpen = (float)s->sharpen;
        int bit_depth = p.dst_bit_depth;
        int full_range = p.full_range_out;
        size_t y_bytes = (size_t)FFABS(out->linesize[0]) * (size_t)height;
        CUdeviceptr sharp_src;
        void *sargs[8];

        if (y_bytes > s->sharpen_buf_size) {
            if (s->sharpen_buf) {
                ret = CHECK_CU(cu->cuMemFree(s->sharpen_buf));
                if (ret < 0) {
                    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
                    goto fail;
                }
                s->sharpen_buf = 0;
                s->sharpen_buf_size = 0;
            }
            ret = CHECK_CU(cu->cuMemAlloc(&s->sharpen_buf, y_bytes));
            if (ret < 0) {
                CHECK_CU(cu->cuCtxPopCurrent(&dummy));
                goto fail;
            }
            s->sharpen_buf_size = y_bytes;
        }

        ret = CHECK_CU(cu->cuMemcpyDtoDAsync(s->sharpen_buf, dst_y, y_bytes,
                                             s->hwctx->stream));
        if (ret < 0) {
            CHECK_CU(cu->cuCtxPopCurrent(&dummy));
            goto fail;
        }

        sharp_src = s->sharpen_buf;
        sargs[0] = &sharp_src;
        sargs[1] = &dst_y;
        sargs[2] = &dst_y_pitch;
        sargs[3] = &width;
        sargs[4] = &height;
        sargs[5] = &bit_depth;
        sargs[6] = &full_range;
        sargs[7] = &sharpen;
        ret = CHECK_CU(cu->cuLaunchKernel(
                           s->cu_func_sharpen,
                           DIV_UP(width, BLOCK_X), DIV_UP(height, BLOCK_Y), 1,
                           BLOCK_X, BLOCK_Y, 1,
                           0, s->hwctx->stream, sargs, NULL));
        if (ret < 0) {
            CHECK_CU(cu->cuCtxPopCurrent(&dummy));
            goto fail;
        }
    }

    CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));

    update_hdr_sidedata(out, s->resolved_mode,
                        s->resolved_mode == TONEMAP_CUDA_MODE_SDR2HDR ? dst_peak : 1.0);
    (void)s->threshold;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

#define OFFSET(x) offsetof(CUDATonemapContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption tonemap_cuda_options[] = {
    { "mode", "tonemap direction", OFFSET(mode_opt), AV_OPT_TYPE_INT,
      {.i64 = TONEMAP_CUDA_MODE_AUTO}, TONEMAP_CUDA_MODE_AUTO, TONEMAP_CUDA_MODE_SDR2HDR, FLAGS, .unit = "mode" },
    { "auto",    "infer from input transfer", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_MODE_AUTO},    0, 0, FLAGS, .unit = "mode" },
    { "hdr2sdr", "HDR to SDR",                0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_MODE_HDR2SDR}, 0, 0, FLAGS, .unit = "mode" },
    { "sdr2hdr", "SDR to HDR",                0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_MODE_SDR2HDR}, 0, 0, FLAGS, .unit = "mode" },

    { "tonemap", "tone-mapping algorithm (default auto=spline, same as VLC/libplacebo)", OFFSET(tonemap), AV_OPT_TYPE_INT,
      {.i64 = TONEMAP_CUDA_AUTO}, 0, TONEMAP_CUDA_COUNT - 1, FLAGS, .unit = "tonemap" },
    { "auto",     "automatic (=spline, VLC/libplacebo default)", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_AUTO},     0, 0, FLAGS, .unit = "tonemap" },
    { "clip",     "PQ-domain hard clip (libplacebo-style)", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_CLIP}, 0, 0, FLAGS, .unit = "tonemap" },
    { "bt.2390",  "ITU-R BT.2390 EETF",           0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_BT2390},   0, 0, FLAGS, .unit = "tonemap" },
    { "bt2390",   "ITU-R BT.2390 EETF",           0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_BT2390},   0, 0, FLAGS, .unit = "tonemap" },
    { "bt.2446a", "ITU-R BT.2446 Method A",       0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_BT2446A},  0, 0, FLAGS, .unit = "tonemap" },
    { "bt2446a",  "ITU-R BT.2446 Method A",       0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_BT2446A},  0, 0, FLAGS, .unit = "tonemap" },
    { "spline",   "single-pivot spline (VLC auto)", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_SPLINE},   0, 0, FLAGS, .unit = "tonemap" },
    { "reinhard", "Reinhard",                     0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_REINHARD}, 0, 0, FLAGS, .unit = "tonemap" },
    { "mobius",   "Möbius",                       0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_MOBIUS},   0, 0, FLAGS, .unit = "tonemap" },
    { "hable",    "filmic Hable (Uncharted2 / vf_tonemap / opencl)", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_HABLE}, 0, 0, FLAGS, .unit = "tonemap" },
    { "gamma",    "gamma knee",                   0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_GAMMA},    0, 0, FLAGS, .unit = "tonemap" },
    { "linear",   "linear stretch",               0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_LINEAR},   0, 0, FLAGS, .unit = "tonemap" },
    { "none",     "scale peaks only",             0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_NONE},     0, 0, FLAGS, .unit = "tonemap" },

    /* Output (defaults: hdr2sdr?bt709; sdr2hdr?PQ/bt2020) */
    { "transfer", "output transfer (hdr2sdr:bt709; sdr2hdr:smpte2084/pq or hlg)", OFFSET(trc_opt),
      AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "transfer" },
    { "bt709",        0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT709},        0, 0, FLAGS, .unit = "transfer" },
    { "bt2020",       0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT2020_10},    0, 0, FLAGS, .unit = "transfer" },
    { "smpte2084",    0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_SMPTE2084},    0, 0, FLAGS, .unit = "transfer" },
    { "pq",           "alias of smpte2084", 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_SMPTE2084}, 0, 0, FLAGS, .unit = "transfer" },
    { "arib-std-b67", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_ARIB_STD_B67}, 0, 0, FLAGS, .unit = "transfer" },
    { "hlg",          "alias of arib-std-b67", 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_ARIB_STD_B67}, 0, 0, FLAGS, .unit = "transfer" },

    { "matrix", "output matrix (hdr2sdr:bt709; sdr2hdr:bt2020)", OFFSET(colorspace_opt),
      AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "matrix" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT709},      0, 0, FLAGS, .unit = "matrix" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT2020_NCL}, 0, 0, FLAGS, .unit = "matrix" },

    { "primaries", "output primaries (hdr2sdr:bt709; sdr2hdr:bt2020)", OFFSET(primaries_opt),
      AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "primaries" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT709},  0, 0, FLAGS, .unit = "primaries" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT2020}, 0, 0, FLAGS, .unit = "primaries" },

    /* Input overrides (HDR unset ?PQ/bt2020; needed for HLG?SDR without correct tags) */
    { "src_transfer", "input transfer override (default: auto-detect from frame; if unknown assume PQ)",
      OFFSET(src_trc_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "src_transfer" },
    { "bt709",        0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT709},        0, 0, FLAGS, .unit = "src_transfer" },
    { "bt2020",       0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT2020_10},    0, 0, FLAGS, .unit = "src_transfer" },
    { "smpte2084",    0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_SMPTE2084},    0, 0, FLAGS, .unit = "src_transfer" },
    { "pq",           0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_SMPTE2084},    0, 0, FLAGS, .unit = "src_transfer" },
    { "arib-std-b67", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_ARIB_STD_B67}, 0, 0, FLAGS, .unit = "src_transfer" },
    { "hlg",          0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_ARIB_STD_B67}, 0, 0, FLAGS, .unit = "src_transfer" },

    { "src_matrix", "input matrix override (default: auto-detect; else follow src transfer)",
      OFFSET(src_colorspace_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "src_matrix" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT709},      0, 0, FLAGS, .unit = "src_matrix" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT2020_NCL}, 0, 0, FLAGS, .unit = "src_matrix" },

    { "src_primaries", "input primaries override (default: auto-detect; else follow src transfer)",
      OFFSET(src_primaries_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "src_primaries" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT709},  0, 0, FLAGS, .unit = "src_primaries" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT2020}, 0, 0, FLAGS, .unit = "src_primaries" },

    { "range", "output color range", OFFSET(range_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "range" },
    { "tv",      0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, .unit = "range" },
    { "pc",      0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, .unit = "range" },
    { "limited", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, .unit = "range" },
    { "full",    0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, .unit = "range" },

    { "format", "output CUDA sw_format", OFFSET(format_opt), AV_OPT_TYPE_INT, {.i64 = FMT_AUTO}, FMT_SAME, INT_MAX, FLAGS, .unit = "fmt" },
    { "auto", 0, 0, AV_OPT_TYPE_CONST, {.i64 = FMT_AUTO}, 0, 0, FLAGS, .unit = "fmt" },
    { "same", 0, 0, AV_OPT_TYPE_CONST, {.i64 = FMT_SAME}, 0, 0, FLAGS, .unit = "fmt" },
    { "nv12", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, 0, FLAGS, .unit = "fmt" },
    { "p010", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010}, 0, 0, FLAGS, .unit = "fmt" },

    { "peak", "HDR peak (relative = nits/100; 0=from metadata, else default 10?000nits)",
      OFFSET(peak), AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, DBL_MAX, FLAGS },
    { "param",     "algorithm parameter", OFFSET(param), AV_OPT_TYPE_DOUBLE, {.dbl = NAN}, DBL_MIN, DBL_MAX, FLAGS },
    { "desat", "perceptual hybrid strength (0?VLC 0.75; >0 overrides). Only for gamut_mode=desaturate as extra desat",
      OFFSET(desat), AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, DBL_MAX, FLAGS },
    { "threshold", "dynamic scene-cut threshold (relative avg jump)", OFFSET(threshold), AV_OPT_TYPE_DOUBLE, {.dbl = 0.2}, 0, DBL_MAX, FLAGS },
    { "dynamic", "measure frame peak/avg and adapt tonemap (1=on, fixes dark clip)", OFFSET(dynamic),
      AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS },
    { "dst_avg", "dynamic target average (relative linear; opencl uses 0.25)", OFFSET(dst_avg),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.25}, 0.05, 1.0, FLAGS },
    { "exposure",  "pre-curve linear gain (hable/spline/...); >1 brighter, <1 darker. Does not change hue", OFFSET(exposure), AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0, 10.0, FLAGS },
    { "knee",      "tone-curve knee/pivot for mobius/spline (unused by hable). 0=algo default", OFFSET(knee), AV_OPT_TYPE_DOUBLE, {.dbl = 0.0}, 0, 1.0, FLAGS },
    { "reinhard_contrast", "Reinhard local contrast", OFFSET(reinhard_contrast), AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0.01, 1.0, FLAGS },
    { "spline_contrast",   "spline contrast", OFFSET(spline_contrast), AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0, 1.5, FLAGS },
    { "knee_offset",       "BT.2390 knee offset", OFFSET(knee_offset), AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0.5, 2.0, FLAGS },
    { "brightness", "linear brightness offset", OFFSET(brightness), AV_OPT_TYPE_DOUBLE, {.dbl = 0.0}, -1.0, 1.0, FLAGS },
    { "contrast",   "linear contrast", OFFSET(contrast), AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0.0, 2.0, FLAGS },
    { "saturation", "saturation (default 1.0 softens BT.2020->709 red flush)", OFFSET(saturation), AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0.0, 2.0, FLAGS },
    { "sharpen",    "luma unsharp after tonemap (0=off, 0.25=moderate default)", OFFSET(sharpen),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0.0, 2.0, FLAGS },
    { "hlg_peak",   "HLG OOTF reference peak in nits", OFFSET(hlg_peak), AV_OPT_TYPE_DOUBLE, {.dbl = 1000.0}, 100.0, 10000.0, FLAGS },

    { "gamut_mode", "gamut mapping (default: perceptual)", OFFSET(gamut_mode), AV_OPT_TYPE_INT,
      {.i64 = TONEMAP_CUDA_GAMUT_PERCEPTUAL}, 0, TONEMAP_CUDA_GAMUT_PERCEPTUAL, FLAGS, .unit = "gamut" },
    { "clip",       "max-RGB tonemap + hue-preserve gamut fit", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_GAMUT_CLIP},       0, 0, FLAGS, .unit = "gamut" },
    { "desaturate", "luma tonemap + desaturate OOG toward luma", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_GAMUT_DESATURATE}, 0, 0, FLAGS, .unit = "gamut" },
    { "perceptual", "luma tonemap + hue-preserve gamut fit (default)", 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_GAMUT_PERCEPTUAL}, 0, 0, FLAGS, .unit = "gamut" },

    { NULL }
};

AVFILTER_DEFINE_CLASS(tonemap_cuda);

static const AVFilterPad tonemap_cuda_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = tonemap_cuda_filter_frame,
    },
};

static const AVFilterPad tonemap_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = tonemap_cuda_config_props,
    },
};

const FFFilter ff_vf_tonemap_cuda = {
    .p.name         = "tonemap_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("CUDA bidirectional HDR/SDR tonemap (libplacebo-inspired)."),
    .p.priv_class   = &tonemap_cuda_class,
    .priv_size      = sizeof(CUDATonemapContext),
    .init           = tonemap_cuda_init,
    .uninit         = tonemap_cuda_uninit,
    FILTER_INPUTS(tonemap_cuda_inputs),
    FILTER_OUTPUTS(tonemap_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
