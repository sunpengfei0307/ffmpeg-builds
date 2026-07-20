/*
 * CUDA physical crop filter
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

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "video.h"

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCK_X 32
#define BLOCK_Y 16

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
};

static const char *const var_names[] = {
    "in_w", "iw",
    "in_h", "ih",
    "out_w", "ow",
    "out_h", "oh",
    "a",
    "sar",
    "dar",
    "hsub",
    "vsub",
    "x",
    "y",
    "n",
    "t",
    NULL
};

enum var_name {
    VAR_IN_W,  VAR_IW,
    VAR_IN_H,  VAR_IH,
    VAR_OUT_W, VAR_OW,
    VAR_OUT_H, VAR_OH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_N,
    VAR_T,
    VAR_VARS_NB
};

typedef struct CUDACropContext {
    const AVClass *class;

    int x, y, w, h;
    AVRational out_sar;
    int keep_aspect;
    int exact;
    int hsub, vsub;

    char *x_expr, *y_expr, *w_expr, *h_expr;
    AVExpr *x_pexpr, *y_pexpr;
    double var_values[VAR_VARS_NB];

    enum AVPixelFormat in_fmt;
    AVBufferRef *frames_ctx;
    AVCUDADeviceContext *hwctx;

    CUmodule cu_module;
    CUfunction cu_func_uchar;
    CUfunction cu_func_uchar2;
    CUfunction cu_func_ushort;
    CUfunction cu_func_ushort2;
} CUDACropContext;

static int format_supported(enum AVPixelFormat fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (fmt == supported_formats[i])
            return 1;
    return 0;
}

static inline int normalize_double(int *n, double d)
{
    if (isnan(d))
        return AVERROR(EINVAL);
    if (d > INT_MAX || d < INT_MIN) {
        *n = d > INT_MAX ? INT_MAX : INT_MIN;
        return AVERROR(EINVAL);
    }
    *n = lrint(d);
    return 0;
}

static int eval_dimensions(AVFilterContext *ctx)
{
    CUDACropContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    const char *expr;
    double res;
    int ret;

    s->var_values[VAR_IN_W] = s->var_values[VAR_IW] = inlink->w;
    s->var_values[VAR_IN_H] = s->var_values[VAR_IH] = inlink->h;
    s->var_values[VAR_A]    = (double)inlink->w / inlink->h;
    s->var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ?
                              av_q2d(inlink->sample_aspect_ratio) : 1;
    s->var_values[VAR_DAR]  = s->var_values[VAR_A] * s->var_values[VAR_SAR];
    s->var_values[VAR_HSUB] = 1 << s->hsub;
    s->var_values[VAR_VSUB] = 1 << s->vsub;
    s->var_values[VAR_X] = s->var_values[VAR_Y] = NAN;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = NAN;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = NAN;
    s->var_values[VAR_N] = 0;
    s->var_values[VAR_T] = NAN;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = res;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;

    if (normalize_double(&s->w, s->var_values[VAR_OUT_W]) < 0 ||
        normalize_double(&s->h, s->var_values[VAR_OUT_H]) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid out_w/out_h expression.\n");
        return AVERROR(EINVAL);
    }

    if (!s->exact) {
        s->w &= ~((1 << s->hsub) - 1);
        s->h &= ~((1 << s->vsub) - 1);
    }

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    s->x_pexpr = s->y_pexpr = NULL;
    if ((ret = av_expr_parse(&s->x_pexpr, s->x_expr, var_names,
                             NULL, NULL, NULL, NULL, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->y_pexpr, s->y_expr, var_names,
                             NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        return AVERROR(EINVAL);

    if (s->keep_aspect) {
        AVRational dar = av_mul_q(inlink->sample_aspect_ratio,
                                  (AVRational){ inlink->w, inlink->h });
        av_reduce(&s->out_sar.num, &s->out_sar.den,
                  (int64_t)dar.num * s->h, (int64_t)dar.den * s->w, INT_MAX);
    } else {
        s->out_sar = inlink->sample_aspect_ratio;
    }

    if (s->w <= 0 || s->h <= 0 || s->w > inlink->w || s->h > inlink->h) {
        av_log(ctx, AV_LOG_ERROR, "Invalid crop size %dx%d for input %dx%d\n",
               s->w, s->h, inlink->w, inlink->h);
        return AVERROR(EINVAL);
    }

    s->x = (inlink->w - s->w) / 2;
    s->y = (inlink->h - s->h) / 2;
    if (!s->exact) {
        s->x &= ~((1 << s->hsub) - 1);
        s->y &= ~((1 << s->vsub) - 1);
    }
    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "Error evaluating expression '%s'\n", expr);
    return ret;
}

static int alloc_out_frames_ctx(AVFilterContext *ctx)
{
    CUDACropContext *s = ctx->priv;
    FilterLink *inl = ff_filter_link(ctx->inputs[0]);
    AVHWFramesContext *in_fc = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    AVBufferRef *out_ref;
    AVHWFramesContext *out_fc;
    int ret;

    out_ref = av_hwframe_ctx_alloc(in_fc->device_ref);
    if (!out_ref)
        return AVERROR(ENOMEM);

    out_fc = (AVHWFramesContext *)out_ref->data;
    out_fc->format    = AV_PIX_FMT_CUDA;
    out_fc->sw_format = s->in_fmt;
    out_fc->width     = FFALIGN(s->w, 32);
    out_fc->height    = FFALIGN(s->h, 32);

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0) {
        av_buffer_unref(&out_ref);
        return ret;
    }

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = out_ref;
    return 0;
}

static av_cold int cuda_crop_load_functions(AVFilterContext *ctx)
{
    CUDACropContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_crop_cuda_ptx_data[];
    extern const unsigned int ff_vf_crop_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_crop_cuda_ptx_data, ff_vf_crop_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uchar, s->cu_module, "crop_uchar"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uchar2, s->cu_module, "crop_uchar2"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_ushort, s->cu_module, "crop_ushort"));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_ushort2, s->cu_module, "crop_ushort2"));

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold void cuda_crop_uninit(AVFilterContext *ctx)
{
    CUDACropContext *s = ctx->priv;

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    s->x_pexpr = s->y_pexpr = NULL;

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

static int cuda_crop_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    CUDACropContext *s = ctx->priv;
    AVHWFramesContext *in_fc;
    const AVPixFmtDescriptor *desc;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    in_fc = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->hwctx = in_fc->device_ctx->hwctx;
    s->in_fmt = in_fc->sw_format;

    if (!format_supported(s->in_fmt)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported sw_format: %s\n",
               av_get_pix_fmt_name(s->in_fmt));
        return AVERROR(ENOTSUP);
    }

    desc = av_pix_fmt_desc_get(s->in_fmt);
    s->hsub = desc->log2_chroma_w;
    s->vsub = desc->log2_chroma_h;

    ret = eval_dimensions(ctx);
    if (ret < 0)
        return ret;

    ret = alloc_out_frames_ctx(ctx);
    if (ret < 0)
        return ret;

    ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ol->hw_frames_ctx)
        return AVERROR(ENOMEM);

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->sample_aspect_ratio = s->out_sar;
    outlink->time_base = inlink->time_base;

    return cuda_crop_load_functions(ctx);
}

static int cuda_crop_launch(AVFilterContext *ctx, AVFrame *out, const AVFrame *in)
{
    CUDACropContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->in_fmt);
    int ret;

    for (int plane = 0; plane < av_pix_fmt_count_planes(s->in_fmt); plane++) {
        const AVComponentDescriptor *comp = &desc->comp[0];
        CUfunction func;
        int hsub = (plane == 1 || plane == 2) ? s->hsub : 0;
        int vsub = (plane == 1 || plane == 2) ? s->vsub : 0;
        int dst_w = AV_CEIL_RSHIFT(s->w, hsub);
        int dst_h = AV_CEIL_RSHIFT(s->h, vsub);
        int src_x = AV_CEIL_RSHIFT(s->x, hsub);
        int src_y = AV_CEIL_RSHIFT(s->y, vsub);
        int dst_pitch, src_pitch;
        CUdeviceptr d_dst = (CUdeviceptr)out->data[plane];
        CUdeviceptr d_src = (CUdeviceptr)in->data[plane];
        void *args[8];

        for (int c = 1; c < desc->nb_components && comp->plane != plane; c++)
            comp = &desc->comp[c];

        if (comp->depth > 8) {
            if (plane == 1) {
                dst_pitch = out->linesize[plane] / 4;
                src_pitch = in->linesize[plane] / 4;
                func = s->cu_func_ushort2;
            } else {
                dst_pitch = out->linesize[plane] / 2;
                src_pitch = in->linesize[plane] / 2;
                func = s->cu_func_ushort;
            }
        } else {
            if (plane == 1) {
                dst_pitch = out->linesize[plane] / 2;
                src_pitch = in->linesize[plane] / 2;
                func = s->cu_func_uchar2;
            } else {
                dst_pitch = out->linesize[plane];
                src_pitch = in->linesize[plane];
                func = s->cu_func_uchar;
            }
        }

        args[0] = &d_dst;
        args[1] = &dst_pitch;
        args[2] = &d_src;
        args[3] = &src_pitch;
        args[4] = &dst_w;
        args[5] = &dst_h;
        args[6] = &src_x;
        args[7] = &src_y;

        ret = CHECK_CU(cu->cuLaunchKernel(func,
                                          DIV_UP(dst_w, BLOCK_X), DIV_UP(dst_h, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1,
                                          0, s->hwctx->stream, args, NULL));
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int cuda_crop_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *l = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    CUDACropContext *s = ctx->priv;
    AVFrame *out = NULL;
    CUcontext dummy;
    int ret;

    s->var_values[VAR_N] = l->frame_count_out;
    s->var_values[VAR_T] = in->pts == AV_NOPTS_VALUE ?
                           NAN : in->pts * av_q2d(inlink->time_base);
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);

    normalize_double(&s->x, s->var_values[VAR_X]);
    normalize_double(&s->y, s->var_values[VAR_Y]);

    s->x = av_clip(s->x, 0, inlink->w - s->w);
    s->y = av_clip(s->y, 0, inlink->h - s->h);
    if (!s->exact) {
        s->x &= ~((1 << s->hsub) - 1);
        s->y &= ~((1 << s->vsub) - 1);
    }

    if (s->x == 0 && s->y == 0 && s->w == in->width && s->h == in->height)
        return ff_filter_frame(outlink, in);

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_hwframe_get_buffer(ol->hw_frames_ctx, out, 0);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    ret = cuda_crop_launch(ctx, out, in);
    CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        goto fail;

    out->width  = s->w;
    out->height = s->h;
    out->sample_aspect_ratio = s->out_sar;
    out->crop_top = out->crop_bottom = out->crop_left = out->crop_right = 0;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int cuda_crop_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                     char *res, int res_len, int flags)
{
    CUDACropContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *ol = ff_filter_link(outlink);
    int old_x = s->x, old_y = s->y, old_w = s->w, old_h = s->h;
    int ret;

    if (strcmp(cmd, "out_w") && strcmp(cmd, "w") &&
        strcmp(cmd, "out_h") && strcmp(cmd, "h") &&
        strcmp(cmd, "x") && strcmp(cmd, "y"))
        return AVERROR(ENOSYS);

    av_opt_set(s, cmd, args, 0);
    ret = eval_dimensions(ctx);
    if (ret < 0) {
        s->x = old_x;
        s->y = old_y;
        s->w = old_w;
        s->h = old_h;
        return ret;
    }

    if (s->w != old_w || s->h != old_h) {
        ret = alloc_out_frames_ctx(ctx);
        if (ret < 0)
            return ret;
        av_buffer_unref(&ol->hw_frames_ctx);
        ol->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
        if (!ol->hw_frames_ctx)
            return AVERROR(ENOMEM);
        outlink->w = s->w;
        outlink->h = s->h;
    }

    outlink->sample_aspect_ratio = s->out_sar;
    return 0;
}

#define OFFSET(x) offsetof(CUDACropContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define TFLAGS (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption crop_cuda_options[] = {
    { "out_w", "set the width crop area expression",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, 0, 0, TFLAGS },
    { "w",     "set the width crop area expression",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, 0, 0, TFLAGS },
    { "out_h", "set the height crop area expression", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, 0, 0, TFLAGS },
    { "h",     "set the height crop area expression", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, 0, 0, TFLAGS },
    { "x",     "set the x crop area expression",      OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "(in_w-out_w)/2"}, 0, 0, TFLAGS },
    { "y",     "set the y crop area expression",      OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "(in_h-out_h)/2"}, 0, 0, TFLAGS },
    { "keep_aspect", "keep aspect ratio", OFFSET(keep_aspect), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { "exact",       "do exact cropping", OFFSET(exact),       AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(crop_cuda);

static const AVFilterPad crop_cuda_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = cuda_crop_filter_frame,
    },
};

static const AVFilterPad crop_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = cuda_crop_config_props,
    },
};

const FFFilter ff_vf_crop_cuda = {
    .p.name          = "crop_cuda",
    .p.description   = NULL_IF_CONFIG_SMALL("CUDA physical crop filter."),
    .p.priv_class    = &crop_cuda_class,
    .priv_size       = sizeof(CUDACropContext),
    .uninit          = cuda_crop_uninit,
    FILTER_INPUTS(crop_cuda_inputs),
    FILTER_OUTPUTS(crop_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .process_command = cuda_crop_process_command,
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
