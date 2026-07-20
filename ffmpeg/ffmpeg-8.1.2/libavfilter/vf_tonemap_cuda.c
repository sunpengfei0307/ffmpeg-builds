/*
 * CUDA bidirectional tonemap filter
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

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/internal.h"
#include "libavutil/mastering_display_metadata.h"
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
    double peak;
    double threshold; /* reserved; peak currently from side-data / override */

    int trc_opt;
    int colorspace_opt;
    int primaries_opt;
    int range_opt;
    int format_opt;

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
} CUDATonemapContext;

static int format_supported(enum AVPixelFormat fmt)
{
    return fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010;
}

static int trc_to_cuda(enum AVColorTransferCharacteristic trc)
{
    switch (trc) {
    case AVCOL_TRC_SMPTE2084:   return TONEMAP_CUDA_TRC_PQ;
    case AVCOL_TRC_ARIB_STD_B67: return TONEMAP_CUDA_TRC_HLG;
    case AVCOL_TRC_IEC61966_2_1: return TONEMAP_CUDA_TRC_SRGB;
    case AVCOL_TRC_BT2020_10:   return TONEMAP_CUDA_TRC_BT2020_10;
    case AVCOL_TRC_BT709:
    default:                    return TONEMAP_CUDA_TRC_BT709;
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

static int check_sdr2hdr_algo(AVFilterContext *ctx, int algo)
{
    switch (algo) {
    case TONEMAP_CUDA_NONE:
    case TONEMAP_CUDA_LINEAR:
    case TONEMAP_CUDA_HABLE:
    case TONEMAP_CUDA_REINHARD:
    case TONEMAP_CUDA_GAMMA:
        return 0;
    case TONEMAP_CUDA_CLIP:
    case TONEMAP_CUDA_MOBIUS:
        av_log(ctx, AV_LOG_ERROR,
               "tonemap algorithm is not invertible for sdr2hdr\n");
        return AVERROR(EINVAL);
    default:
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

    if (mode == TONEMAP_CUDA_MODE_AUTO) {
        if (is_hdr_trc(in->color_trc))
            mode = TONEMAP_CUDA_MODE_HDR2SDR;
        else if (is_sdr_trc(in->color_trc) || in->color_trc == AVCOL_TRC_UNSPECIFIED)
            mode = TONEMAP_CUDA_MODE_SDR2HDR;
        else {
            av_log(ctx, AV_LOG_ERROR,
                   "Cannot infer mode from transfer %s; set mode= explicitly\n",
                   av_color_transfer_name(in->color_trc));
            return AVERROR(EINVAL);
        }
        if (in->color_trc == AVCOL_TRC_UNSPECIFIED)
            av_log(ctx, AV_LOG_WARNING,
                   "Input transfer unspecified; assuming sdr2hdr\n");
    }

    if (mode == TONEMAP_CUDA_MODE_HDR2SDR && !is_hdr_trc(in->color_trc) &&
        in->color_trc != AVCOL_TRC_UNSPECIFIED)
        av_log(ctx, AV_LOG_WARNING,
               "hdr2sdr requested but input transfer is %s\n",
               av_color_transfer_name(in->color_trc));

    if (mode == TONEMAP_CUDA_MODE_SDR2HDR) {
        int ret = check_sdr2hdr_algo(ctx, s->tonemap);
        if (ret < 0)
            return ret;
    }

    s->resolved_mode = mode;
    s->trc_in = in->color_trc;
    s->colorspace_in = in->colorspace == AVCOL_SPC_UNSPECIFIED ?
                       (is_hdr_trc(in->color_trc) ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709) :
                       in->colorspace;
    s->primaries_in = in->color_primaries == AVCOL_PRI_UNSPECIFIED ?
                      (is_hdr_trc(in->color_trc) ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709) :
                      in->color_primaries;
    s->range_in = in->color_range == AVCOL_RANGE_UNSPECIFIED ?
                  AVCOL_RANGE_MPEG : in->color_range;

    if (mode == TONEMAP_CUDA_MODE_HDR2SDR) {
        s->trc_out = s->trc_opt >= 0 ? s->trc_opt : AVCOL_TRC_BT709;
        s->colorspace_out = s->colorspace_opt >= 0 ? s->colorspace_opt : AVCOL_SPC_BT709;
        s->primaries_out = s->primaries_opt >= 0 ? s->primaries_opt : AVCOL_PRI_BT709;
    } else {
        s->trc_out = s->trc_opt >= 0 ? s->trc_opt : AVCOL_TRC_SMPTE2084;
        s->colorspace_out = s->colorspace_opt >= 0 ? s->colorspace_opt : AVCOL_SPC_BT2020_NCL;
        s->primaries_out = s->primaries_opt >= 0 ? s->primaries_opt : AVCOL_PRI_BT2020;
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
        if (!isnan(s->param))
            s->param = (1.0 - s->param) / s->param;
        break;
    case TONEMAP_CUDA_MOBIUS:
        if (isnan(s->param))
            s->param = 0.3;
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

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }
    s->cu_module = NULL;
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
        av_log(ctx, AV_LOG_ERROR, "Unsupported input sw_format: %s\n",
               av_get_pix_fmt_name(s->in_fmt));
        return AVERROR(ENOTSUP);
    }

    /* Tentative output format until first frame resolves mode=auto */
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

static void update_hdr_sidedata(AVFrame *out, int mode, double peak)
{
    if (mode == TONEMAP_CUDA_MODE_HDR2SDR) {
        ff_update_hdr_metadata(out, 1.0);
        return;
    }

    /* sdr2hdr: write Mastering / CLL based on peak (nits) */
    {
        AVMasteringDisplayMetadata *mdm;
        AVContentLightMetadata *cll;
        double nits = peak > 0 ? peak * REFERENCE_WHITE : 1000.0;

        mdm = av_mastering_display_metadata_create_side_data(out);
        if (mdm) {
            mdm->has_luminance = 1;
            mdm->max_luminance = av_d2q(nits, 10000);
            mdm->min_luminance = av_d2q(0.0001, 10000);
            mdm->has_primaries = 1;
            /* BT.2020 primaries approx */
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
    double peak;
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

    peak = s->peak;
    if (peak <= 0)
        peak = ff_determine_signal_peak(in);
    if (peak <= 0)
        peak = (s->resolved_mode == TONEMAP_CUDA_MODE_SDR2HDR) ? 10.0 : 100.0 / REFERENCE_WHITE;

    p = s->gpu_params;
    p.peak = (float)peak;
    p.param = (float)s->param;
    p.desat = (float)s->desat;
    p.target_peak = (s->resolved_mode == TONEMAP_CUDA_MODE_SDR2HDR) ?
                    (float)(peak > 0 ? peak : 10.0f) : 1.0f;
    p.tonemap = s->tonemap;
    p.mode = s->resolved_mode;
    p.trc_in = trc_to_cuda(s->trc_in == AVCOL_TRC_UNSPECIFIED ?
                           (s->resolved_mode == TONEMAP_CUDA_MODE_HDR2SDR ?
                            AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709) :
                           s->trc_in);
    p.trc_out = trc_to_cuda(s->trc_out);
    p.full_range_in  = s->range_in == AVCOL_RANGE_JPEG;
    p.full_range_out = s->range_out == AVCOL_RANGE_JPEG;
    p.src_bit_depth = (s->in_fmt == AV_PIX_FMT_P010) ? 10 : 8;
    p.dst_bit_depth = (s->out_fmt == AV_PIX_FMT_P010) ? 10 : 8;

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

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuLaunchKernel(
                       s->cu_func,
                       DIV_UP(width, BLOCK_X), DIV_UP(height, BLOCK_Y), 1,
                       BLOCK_X, BLOCK_Y, 1,
                       0, s->hwctx->stream, args, NULL));
    CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;

    update_hdr_sidedata(out, s->resolved_mode, peak);
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

    { "tonemap", "tonemap algorithm", OFFSET(tonemap), AV_OPT_TYPE_INT,
      {.i64 = TONEMAP_CUDA_HABLE}, TONEMAP_CUDA_NONE, TONEMAP_CUDA_MOBIUS, FLAGS, .unit = "tonemap" },
    { "none",     0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_NONE},     0, 0, FLAGS, .unit = "tonemap" },
    { "linear",   0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_LINEAR},   0, 0, FLAGS, .unit = "tonemap" },
    { "gamma",    0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_GAMMA},    0, 0, FLAGS, .unit = "tonemap" },
    { "clip",     0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_CLIP},     0, 0, FLAGS, .unit = "tonemap" },
    { "reinhard", 0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_REINHARD}, 0, 0, FLAGS, .unit = "tonemap" },
    { "hable",    0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_HABLE},    0, 0, FLAGS, .unit = "tonemap" },
    { "mobius",   0, 0, AV_OPT_TYPE_CONST, {.i64 = TONEMAP_CUDA_MOBIUS},   0, 0, FLAGS, .unit = "tonemap" },

    { "transfer", "set transfer characteristic", OFFSET(trc_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "transfer" },
    { "t",        "set transfer characteristic", OFFSET(trc_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "transfer" },
    { "bt709",       0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT709},       0, 0, FLAGS, .unit = "transfer" },
    { "bt2020",      0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_BT2020_10},   0, 0, FLAGS, .unit = "transfer" },
    { "smpte2084",   0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_SMPTE2084},   0, 0, FLAGS, .unit = "transfer" },
    { "arib-std-b67",0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_TRC_ARIB_STD_B67},0, 0, FLAGS, .unit = "transfer" },

    { "matrix", "set colorspace matrix", OFFSET(colorspace_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "matrix" },
    { "m",      "set colorspace matrix", OFFSET(colorspace_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "matrix" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT709},      0, 0, FLAGS, .unit = "matrix" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_SPC_BT2020_NCL}, 0, 0, FLAGS, .unit = "matrix" },

    { "primaries", "set color primaries", OFFSET(primaries_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "primaries" },
    { "p",         "set color primaries", OFFSET(primaries_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "primaries" },
    { "bt709",  0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT709},  0, 0, FLAGS, .unit = "primaries" },
    { "bt2020", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_PRI_BT2020}, 0, 0, FLAGS, .unit = "primaries" },

    { "range", "set color range", OFFSET(range_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "range" },
    { "r",     "set color range", OFFSET(range_opt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, .unit = "range" },
    { "tv",      0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, .unit = "range" },
    { "pc",      0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, .unit = "range" },
    { "limited", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, .unit = "range" },
    { "full",    0, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, .unit = "range" },

    { "format", "output sw_format", OFFSET(format_opt), AV_OPT_TYPE_INT, {.i64 = FMT_AUTO}, FMT_SAME, INT_MAX, FLAGS, .unit = "fmt" },
    { "auto", 0, 0, AV_OPT_TYPE_CONST, {.i64 = FMT_AUTO}, 0, 0, FLAGS, .unit = "fmt" },
    { "same", 0, 0, AV_OPT_TYPE_CONST, {.i64 = FMT_SAME}, 0, 0, FLAGS, .unit = "fmt" },
    { "nv12", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, 0, FLAGS, .unit = "fmt" },
    { "p010", 0, 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010}, 0, 0, FLAGS, .unit = "fmt" },

    { "peak",      "signal peak override",      OFFSET(peak),      AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, DBL_MAX, FLAGS },
    { "param",     "tonemap parameter",         OFFSET(param),     AV_OPT_TYPE_DOUBLE, {.dbl = NAN}, DBL_MIN, DBL_MAX, FLAGS },
    { "desat",     "desaturation parameter",    OFFSET(desat),     AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0, DBL_MAX, FLAGS },
    { "threshold", "scene detection threshold", OFFSET(threshold), AV_OPT_TYPE_DOUBLE, {.dbl = 0.2}, 0, DBL_MAX, FLAGS },
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
    .p.description  = NULL_IF_CONFIG_SMALL("CUDA bidirectional HDR/SDR tonemap filter."),
    .p.priv_class   = &tonemap_cuda_class,
    .priv_size      = sizeof(CUDATonemapContext),
    .init           = tonemap_cuda_init,
    .uninit         = tonemap_cuda_uninit,
    FILTER_INPUTS(tonemap_cuda_inputs),
    FILTER_OUTPUTS(tonemap_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
