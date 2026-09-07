/*
 * Copyright (c) 2013 Clément Bœsch
 * Copyright (c) 2026 FFmpeg contributors
 *
 * CUDA curves filter — host LUT build mirrors vf_curves.c; GPU applies
 * composed R/G/B LUTs after BT.709 YUV↔RGB conversion.
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

#include "libavutil/avassert.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/eval.h"
#include "libavutil/file.h"
#include "libavutil/file_open.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
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

#define R 0
#define G 1
#define B 2
#define NB_COMP 3
#define CURVES_CUDA_LUT_MAX 1024

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
};

struct keypoint {
    double x, y;
    struct keypoint *next;
};

enum preset {
    PRESET_NONE,
    PRESET_COLOR_NEGATIVE,
    PRESET_CROSS_PROCESS,
    PRESET_DARKER,
    PRESET_INCREASE_CONTRAST,
    PRESET_LIGHTER,
    PRESET_LINEAR_CONTRAST,
    PRESET_MEDIUM_CONTRAST,
    PRESET_NEGATIVE,
    PRESET_STRONG_CONTRAST,
    PRESET_VINTAGE,
    NB_PRESETS,
};

enum interp {
    INTERP_NATURAL,
    INTERP_PCHIP,
    NB_INTERPS,
};

typedef struct CurvesCUDAContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;
    enum AVPixelFormat sw_format;
    int depth;
    int lut_size;

    int preset;
    char *comp_points_str[NB_COMP + 1];
    char *comp_points_str_all;
    uint16_t *graph[NB_COMP + 1];
    char *psfile;
    char *plot_filename;
    int saved_plot;
    int parsed_psfile;
    int interp;

    CUdeviceptr lut_dev[NB_COMP];
    size_t lut_bytes;

    CUmodule cu_module;
    CUfunction cu_curves_nv12;
    CUfunction cu_curves_p010;

    AVBufferRef *frames_ctx;
    AVFrame *frame;
    AVFrame *tmp_frame;
} CurvesCUDAContext;

#define OFFSET(x) offsetof(CurvesCUDAContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption curves_cuda_options[] = {
    { "preset", "select a color curves preset", OFFSET(preset), AV_OPT_TYPE_INT, {.i64=PRESET_NONE}, PRESET_NONE, NB_PRESETS-1, FLAGS, .unit = "preset_name" },
        { "none",               NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_NONE},                 0, 0, FLAGS, .unit = "preset_name" },
        { "color_negative",     NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_COLOR_NEGATIVE},       0, 0, FLAGS, .unit = "preset_name" },
        { "cross_process",      NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_CROSS_PROCESS},        0, 0, FLAGS, .unit = "preset_name" },
        { "darker",             NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_DARKER},               0, 0, FLAGS, .unit = "preset_name" },
        { "increase_contrast",  NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_INCREASE_CONTRAST},    0, 0, FLAGS, .unit = "preset_name" },
        { "lighter",            NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_LIGHTER},              0, 0, FLAGS, .unit = "preset_name" },
        { "linear_contrast",    NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_LINEAR_CONTRAST},      0, 0, FLAGS, .unit = "preset_name" },
        { "medium_contrast",    NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_MEDIUM_CONTRAST},      0, 0, FLAGS, .unit = "preset_name" },
        { "negative",           NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_NEGATIVE},             0, 0, FLAGS, .unit = "preset_name" },
        { "strong_contrast",    NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_STRONG_CONTRAST},      0, 0, FLAGS, .unit = "preset_name" },
        { "vintage",            NULL, 0, AV_OPT_TYPE_CONST, {.i64=PRESET_VINTAGE},              0, 0, FLAGS, .unit = "preset_name" },
    { "master","set master points coordinates",OFFSET(comp_points_str[NB_COMP]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "m",     "set master points coordinates",OFFSET(comp_points_str[NB_COMP]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "red",   "set red points coordinates",   OFFSET(comp_points_str[0]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "r",     "set red points coordinates",   OFFSET(comp_points_str[0]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "green", "set green points coordinates", OFFSET(comp_points_str[1]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "g",     "set green points coordinates", OFFSET(comp_points_str[1]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "blue",  "set blue points coordinates",  OFFSET(comp_points_str[2]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "b",     "set blue points coordinates",  OFFSET(comp_points_str[2]), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "all",   "set points coordinates for all components", OFFSET(comp_points_str_all), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "psfile", "set Photoshop curves file name", OFFSET(psfile), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "plot", "save Gnuplot script of the curves in specified file", OFFSET(plot_filename), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "interp", "specify the kind of interpolation", OFFSET(interp), AV_OPT_TYPE_INT, {.i64=INTERP_NATURAL}, INTERP_NATURAL, NB_INTERPS-1, FLAGS, .unit = "interp_name" },
    { "natural", "natural cubic spline", 0, AV_OPT_TYPE_CONST, {.i64=INTERP_NATURAL}, 0, 0, FLAGS, .unit = "interp_name" },
    { "pchip",   "monotonically cubic interpolation", 0, AV_OPT_TYPE_CONST, {.i64=INTERP_PCHIP},   0, 0, FLAGS, .unit = "interp_name" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(curves_cuda);

static const struct {
    const char *r;
    const char *g;
    const char *b;
    const char *master;
} curves_presets[] = {
    [PRESET_COLOR_NEGATIVE] = {
        "0.129/1 0.466/0.498 0.725/0",
        "0.109/1 0.301/0.498 0.517/0",
        "0.098/1 0.235/0.498 0.423/0",
    },
    [PRESET_CROSS_PROCESS] = {
        "0/0 0.25/0.156 0.501/0.501 0.686/0.745 1/1",
        "0/0 0.25/0.188 0.38/0.501 0.745/0.815 1/0.815",
        "0/0 0.231/0.094 0.709/0.874 1/1",
    },
    [PRESET_DARKER]             = { .master = "0/0 0.5/0.4 1/1" },
    [PRESET_INCREASE_CONTRAST]  = { .master = "0/0 0.149/0.066 0.831/0.905 0.905/0.98 1/1" },
    [PRESET_LIGHTER]            = { .master = "0/0 0.4/0.5 1/1" },
    [PRESET_LINEAR_CONTRAST]    = { .master = "0/0 0.305/0.286 0.694/0.713 1/1" },
    [PRESET_MEDIUM_CONTRAST]    = { .master = "0/0 0.286/0.219 0.639/0.643 1/1" },
    [PRESET_NEGATIVE]           = { .master = "0/1 1/0" },
    [PRESET_STRONG_CONTRAST]    = { .master = "0/0 0.301/0.196 0.592/0.6 0.686/0.737 1/1" },
    [PRESET_VINTAGE] = {
        "0/0.11 0.42/0.51 1/0.95",
        "0/0 0.50/0.48 1/1",
        "0/0.22 0.49/0.44 1/0.8",
    }
};

static int format_is_supported(enum AVPixelFormat fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

static struct keypoint *make_point(double x, double y, struct keypoint *next)
{
    struct keypoint *point = av_mallocz(sizeof(*point));

    if (!point)
        return NULL;
    point->x = x;
    point->y = y;
    point->next = next;
    return point;
}

static int parse_points_str(AVFilterContext *ctx, struct keypoint **points, const char *s,
                            int lut_size)
{
    char *p = (char *)s;
    struct keypoint *last = NULL;
    const int scale = lut_size - 1;

    while (p && *p) {
        struct keypoint *point = make_point(0, 0, NULL);
        if (!point)
            return AVERROR(ENOMEM);
        point->x = av_strtod(p, &p); if (p && *p) p++;
        point->y = av_strtod(p, &p); if (p && *p) p++;
        if (point->x < 0 || point->x > 1 || point->y < 0 || point->y > 1) {
            av_log(ctx, AV_LOG_ERROR, "Invalid key point coordinates (%f;%f), "
                   "x and y must be in the [0;1] range.\n", point->x, point->y);
            av_free(point);
            return AVERROR(EINVAL);
        }
        if (last) {
            if ((int)(last->x * scale) >= (int)(point->x * scale)) {
                av_log(ctx, AV_LOG_ERROR, "Key point coordinates (%f;%f) "
                       "and (%f;%f) are too close from each other or not "
                       "strictly increasing on the x-axis\n",
                       last->x, last->y, point->x, point->y);
                av_free(point);
                return AVERROR(EINVAL);
            }
            last->next = point;
        }
        if (!*points)
            *points = point;
        last = point;
    }

    if (*points && !(*points)->next) {
        av_log(ctx, AV_LOG_WARNING, "Only one point (at (%f;%f)) is defined, "
               "this is unlikely to behave as you expect. You probably want"
               "at least 2 points.",
               (*points)->x, (*points)->y);
    }

    return 0;
}

static int get_nb_points(const struct keypoint *d)
{
    int n = 0;
    while (d) {
        n++;
        d = d->next;
    }
    return n;
}

#define CLIP(v) (nbits == 8 ? av_clip_uint8(v) : av_clip_uintp2_c(v, nbits))

static inline int interpolate(void *log_ctx, uint16_t *y,
                              const struct keypoint *points, int nbits)
{
    int i, ret = 0;
    const struct keypoint *point = points;
    double xprev = 0;
    const int lut_size = 1<<nbits;
    const int scale = lut_size - 1;

    double (*matrix)[3];
    double *h, *r;
    const int n = get_nb_points(points);

    if (n == 0) {
        for (i = 0; i < lut_size; i++)
            y[i] = i;
        return 0;
    }

    if (n == 1) {
        for (i = 0; i < lut_size; i++)
            y[i] = CLIP(point->y * scale);
        return 0;
    }

    matrix = av_calloc(n, sizeof(*matrix));
    h = av_malloc((n - 1) * sizeof(*h));
    r = av_calloc(n, sizeof(*r));

    if (!matrix || !h || !r) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    i = -1;
    for (point = points; point; point = point->next) {
        if (i != -1)
            h[i] = point->x - xprev;
        xprev = point->x;
        i++;
    }

    point = points;
    for (i = 1; i < n - 1; i++) {
        const double yp = point->y;
        const double yc = point->next->y;
        const double yn = point->next->next->y;
        r[i] = 6 * ((yn-yc)/h[i] - (yc-yp)/h[i-1]);
        point = point->next;
    }

#define BD 0
#define MD 1
#define AD 2

    matrix[0][MD] = matrix[n - 1][MD] = 1;
    for (i = 1; i < n - 1; i++) {
        matrix[i][BD] = h[i-1];
        matrix[i][MD] = 2 * (h[i-1] + h[i]);
        matrix[i][AD] = h[i];
    }

    for (i = 1; i < n; i++) {
        const double den = matrix[i][MD] - matrix[i][BD] * matrix[i-1][AD];
        const double k = den ? 1./den : 1.;
        matrix[i][AD] *= k;
        r[i] = (r[i] - matrix[i][BD] * r[i - 1]) * k;
    }
    for (i = n - 2; i >= 0; i--)
        r[i] = r[i] - matrix[i][AD] * r[i + 1];

    point = points;

    for (i = 0; i < (int)(point->x * scale); i++)
        y[i] = CLIP(point->y * scale);

    i = 0;
    av_assert0(point->next);
    while (point->next) {
        const double yc = point->y;
        const double yn = point->next->y;

        const double a = yc;
        const double b = (yn-yc)/h[i] - h[i]*r[i]/2. - h[i]*(r[i+1]-r[i])/6.;
        const double c = r[i] / 2.;
        const double d = (r[i+1] - r[i]) / (6.*h[i]);

        int x;
        const int x_start = point->x       * scale;
        const int x_end   = point->next->x * scale;

        av_assert0(x_start >= 0 && x_start < lut_size &&
                   x_end   >= 0 && x_end   < lut_size);

        for (x = x_start; x <= x_end; x++) {
            const double xx = (x - x_start) * 1./scale;
            const double yy = a + b*xx + c*xx*xx + d*xx*xx*xx;
            y[x] = CLIP(yy * scale);
            av_log(log_ctx, AV_LOG_DEBUG, "f(%f)=%f -> y[%d]=%d\n", xx, yy, x, y[x]);
        }

        point = point->next;
        i++;
    }

    for (i = (int)(point->x * scale); i < lut_size; i++)
        y[i] = CLIP(point->y * scale);

end:
    av_free(matrix);
    av_free(h);
    av_free(r);
    return ret;
}

#define SIGN(x) (x > 0.0 ? 1 : x < 0.0 ? -1 : 0)

static double pchip_edge_case(double h0, double h1, double m0, double m1)
{
    int mask, mask2;
    double d;

    d = ((2 * h0 + h1) * m0 - h0 * m1) / (h0 + h1);

    mask = SIGN(d) != SIGN(m0);
    mask2 = (SIGN(m0) != SIGN(m1)) && (fabs(d) > 3. * fabs(m0));

    if (mask) d = 0.0;
    else if (mask2) d = 3.0 * m0;

    return d;
}

static int pchip_find_derivatives(const int n, const double *hk, const double *mk, double *dk)
{
    int ret = 0;
    const int m = n - 1;
    int8_t *smk;

    smk = av_malloc(n);
    if (!smk) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (int i = 0; i < n; i++) smk[i] = SIGN(mk[i]);

    for (int i = 0; i < m; i++) {
        int8_t condition = (smk[i + 1] != smk[i]) || (mk[i + 1] == 0) || (mk[i] == 0);
        if (condition) {
            dk[i + 1] = 0.0;
        } else {
            double w1 = 2 * hk[i + 1] + hk[i];
            double w2 = hk[i + 1] + 2 * hk[i];
            dk[i + 1] = (w1 + w2) / (w1 / mk[i] + w2 / mk[i + 1]);
        }
    }

    dk[0] = pchip_edge_case(hk[0], hk[1], mk[0], mk[1]);
    dk[n] = pchip_edge_case(hk[n - 1], hk[n - 2], mk[n - 1], mk[n - 2]);

end:
    av_free(smk);

    return ret;
}

static inline double interp_cubic_hermite_half(const double x, const double f,
                                               const double d)
{
    double x2 = x * x, x3 = x2 * x;
    return f * (3.0 * x2 - 2.0 * x3) + d * (x3 - x2);
}

static inline int interpolate_pchip(void *log_ctx, uint16_t *y,
                                    const struct keypoint *points, int nbits)
{
    const struct keypoint *point = points;
    const int lut_size = 1<<nbits;
    const int n = get_nb_points(points);
    double *xi, *fi, *di, *hi, *mi;
    const int scale = lut_size - 1;
    uint16_t x;
    int ret = 0;

    if (n == 0) {
        for (int i = 0; i < lut_size; i++) y[i] = i;
        return 0;
    }

    if (n == 1) {
        const uint16_t yval = CLIP(point->y * scale);
        for (int i = 0; i < lut_size; i++) y[i] = yval;
        return 0;
    }

    xi = av_calloc(3*n + 2*(n-1), sizeof(double));
    if (!xi) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    fi = xi + n;
    di = fi + n;
    hi = di + n;
    mi = hi + n - 1;

    for (int i = 0; i < n; i++) {
        xi[i] = point->x * scale;
        fi[i] = point->y * scale;
        point = point->next;
    }

    for (int i = 0; i < n - 1; i++) {
        const double val = (xi[i+1]-xi[i]);
        hi[i] = val;
        mi[i] = (fi[i+1]-fi[i]) / val;
    }

    if (n == 2) {
        const double m = mi[0], b = fi[0] - xi[0]*m;
        for (int i = 0; i < lut_size; i++) y[i] = CLIP(i*m + b);
        goto end;
    }

    ret = pchip_find_derivatives(n-1, hi, mi, di);
    if (ret)
        goto end;

    x = 0;
    if (xi[0] > 0) {
        const double xi0 = xi[0];
        const double yi0 = fi[0];
        const uint16_t yval = CLIP(yi0);
        for (; x < xi0; x++) {
            y[x] = yval;
            av_log(log_ctx, AV_LOG_TRACE, "f(%f)=%f -> y[%d]=%d\n", xi0, yi0, x, y[x]);
        }
        av_log(log_ctx, AV_LOG_DEBUG, "Interval -1: [0, %d] -> %d\n", x - 1, yval);
    }

    for (int i = 0, x0 = x; i < n-1; i++, x0 = x) {
        const double xi0 = xi[i];
        const double xi1 = xi[i + 1];
        const double h = hi[i];
        const double f0 = fi[i];
        const double f1 = fi[i + 1];
        const double d0 = di[i];
        const double d1 = di[i + 1];

        for (; x < xi1; x++) {
            const double xx = (x - xi0) / h;
            const double yy = interp_cubic_hermite_half(1 - xx, f0, -h * d0)
                            + interp_cubic_hermite_half(xx, f1, h * d1);
            y[x] = CLIP(yy);
            av_log(log_ctx, AV_LOG_TRACE, "f(%f)=%f -> y[%d]=%d\n", xx, yy, x, y[x]);
        }

        if (x > x0)
            av_log(log_ctx, AV_LOG_DEBUG, "Interval %d: [%d, %d] -> [%d, %d]\n",
                                                    i, x0, x-1, y[x0], y[x-1]);
        else
            av_log(log_ctx, AV_LOG_DEBUG, "Interval %d: empty\n", i);
    }

    if (x && x < lut_size) {
        const double xi1 = xi[n - 1];
        const double yi1 = fi[n - 1];
        const uint16_t yval = CLIP(yi1);
        av_log(log_ctx, AV_LOG_DEBUG, "Interval %d: [%d, %d] -> %d\n",
                                                n-1, x, lut_size - 1, yval);
        for (; x && x < lut_size; x++) {
            y[x] = yval;
            av_log(log_ctx, AV_LOG_TRACE, "f(%f)=%f -> y[%d]=%d\n", xi1, yi1, x, yval);
        }
    }

end:
    av_free(xi);
    return ret;
}

static int parse_psfile(AVFilterContext *ctx, const char *fname)
{
    CurvesCUDAContext *curves = ctx->priv;
    uint8_t *buf;
    size_t size;
    int i, ret, version av_unused, nb_curves;
    AVBPrint ptstr;
    static const int comp_ids[] = {3, 0, 1, 2};

    av_bprint_init(&ptstr, 0, AV_BPRINT_SIZE_AUTOMATIC);

    ret = av_file_map(fname, &buf, &size, 0, NULL);
    if (ret < 0)
        return ret;

#define READ16(dst) do {                \
    if (size < 2) {                     \
        ret = AVERROR_INVALIDDATA;      \
        goto end;                       \
    }                                   \
    dst = AV_RB16(buf);                 \
    buf  += 2;                          \
    size -= 2;                          \
} while (0)

    READ16(version);
    READ16(nb_curves);
    for (i = 0; i < FFMIN(nb_curves, FF_ARRAY_ELEMS(comp_ids)); i++) {
        int nb_points, n;
        av_bprint_clear(&ptstr);
        READ16(nb_points);
        for (n = 0; n < nb_points; n++) {
            int y, x;
            READ16(y);
            READ16(x);
            av_bprintf(&ptstr, "%f/%f ", x / 255., y / 255.);
        }
        if (*ptstr.str) {
            char **pts = &curves->comp_points_str[comp_ids[i]];
            if (!*pts) {
                *pts = av_strdup(ptstr.str);
                av_log(ctx, AV_LOG_DEBUG, "curves %d (intid=%d) [%d points]: [%s]\n",
                       i, comp_ids[i], nb_points, *pts);
                if (!*pts) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
            }
        }
    }
end:
    av_bprint_finalize(&ptstr, NULL);
    av_file_unmap(buf, size);
    return ret;
}

static int dump_curves(const char *fname, uint16_t *graph[NB_COMP + 1],
                       struct keypoint *comp_points[NB_COMP + 1],
                       int lut_size, void *log_ctx)
{
    int i;
    AVBPrint buf;
    const double scale = 1. / (lut_size - 1);
    static const char * const colors[] = { "red", "green", "blue", "#404040", };
    FILE *f = avpriv_fopen_utf8(fname, "w");

    av_assert0(FF_ARRAY_ELEMS(colors) == NB_COMP + 1);

    if (!f) {
        int ret = AVERROR(errno);
        av_log(log_ctx, AV_LOG_ERROR, "Cannot open file '%s' for writing: %s\n",
               fname, av_err2str(ret));
        return ret;
    }

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(&buf, "set xtics 0.1\n");
    av_bprintf(&buf, "set ytics 0.1\n");
    av_bprintf(&buf, "set size square\n");
    av_bprintf(&buf, "set grid\n");

    for (i = 0; i < FF_ARRAY_ELEMS(colors); i++) {
        av_bprintf(&buf, "%s'-' using 1:2 with lines lc '%s' title ''",
                   i ? ", " : "plot ", colors[i]);
        if (comp_points[i])
            av_bprintf(&buf, ", '-' using 1:2 with points pointtype 3 lc '%s' title ''",
                    colors[i]);
    }
    av_bprintf(&buf, "\n");

    for (i = 0; i < FF_ARRAY_ELEMS(colors); i++) {
        int x;

        for (x = 0; x < lut_size; x++)
            av_bprintf(&buf, "%f %f\n", x * scale, graph[i][x] * scale);
        av_bprintf(&buf, "e\n");

        if (comp_points[i]) {
            const struct keypoint *point = comp_points[i];

            while (point) {
                av_bprintf(&buf, "%f %f\n", point->x, point->y);
                point = point->next;
            }
            av_bprintf(&buf, "e\n");
        }
    }

    fwrite(buf.str, 1, buf.len, f);
    fclose(f);
    av_bprint_finalize(&buf, NULL);
    return 0;
}

static av_cold int curves_cuda_init_opts(AVFilterContext *ctx)
{
    int i, ret;
    CurvesCUDAContext *curves = ctx->priv;
    char **pts = curves->comp_points_str;
    const char *allp = curves->comp_points_str_all;

    if (allp) {
        for (i = 0; i < NB_COMP; i++) {
            if (!pts[i])
                pts[i] = av_strdup(allp);
            if (!pts[i])
                return AVERROR(ENOMEM);
        }
    }

    if (curves->psfile && !curves->parsed_psfile) {
        ret = parse_psfile(ctx, curves->psfile);
        if (ret < 0)
            return ret;
        curves->parsed_psfile = 1;
    }

    if (curves->preset != PRESET_NONE) {
#define SET_COMP_IF_NOT_SET(n, name) do {                           \
    if (!pts[n] && curves_presets[curves->preset].name) {           \
        pts[n] = av_strdup(curves_presets[curves->preset].name);    \
        if (!pts[n])                                                \
            return AVERROR(ENOMEM);                                 \
    }                                                               \
} while (0)
        SET_COMP_IF_NOT_SET(0, r);
        SET_COMP_IF_NOT_SET(1, g);
        SET_COMP_IF_NOT_SET(2, b);
        SET_COMP_IF_NOT_SET(3, master);
        curves->preset = PRESET_NONE;
    }

    return 0;
}

static void free_keypoints(struct keypoint *comp_points[NB_COMP + 1])
{
    for (int i = 0; i < NB_COMP + 1; i++) {
        struct keypoint *point = comp_points[i];
        while (point) {
            struct keypoint *next = point->next;
            av_free(point);
            point = next;
        }
        comp_points[i] = NULL;
    }
}

static int build_graphs(AVFilterContext *ctx)
{
    int i, j, ret;
    CurvesCUDAContext *s = ctx->priv;
    char **pts = s->comp_points_str;
    struct keypoint *comp_points[NB_COMP + 1] = {0};

    for (i = 0; i < NB_COMP + 1; i++) {
        if (!s->graph[i])
            s->graph[i] = av_calloc(CURVES_CUDA_LUT_MAX, sizeof(*s->graph[0]));
        if (!s->graph[i]) {
            free_keypoints(comp_points);
            return AVERROR(ENOMEM);
        }
        ret = parse_points_str(ctx, comp_points + i, s->comp_points_str[i], s->lut_size);
        if (ret < 0) {
            free_keypoints(comp_points);
            return ret;
        }
        if (s->interp == INTERP_PCHIP)
            ret = interpolate_pchip(ctx, s->graph[i], comp_points[i], s->depth);
        else
            ret = interpolate(ctx, s->graph[i], comp_points[i], s->depth);
        if (ret < 0) {
            free_keypoints(comp_points);
            return ret;
        }
    }

    if (pts[NB_COMP]) {
        for (i = 0; i < NB_COMP; i++)
            for (j = 0; j < s->lut_size; j++)
                s->graph[i][j] = s->graph[NB_COMP][s->graph[i][j]];
    }

    if (av_log_get_level() >= AV_LOG_VERBOSE) {
        for (i = 0; i < NB_COMP; i++) {
            const struct keypoint *point = comp_points[i];
            av_log(ctx, AV_LOG_VERBOSE, "#%d points:", i);
            while (point) {
                av_log(ctx, AV_LOG_VERBOSE, " (%f;%f)", point->x, point->y);
                point = point->next;
            }
        }
    }

    if (s->plot_filename && !s->saved_plot) {
        dump_curves(s->plot_filename, s->graph, comp_points, s->lut_size, ctx);
        s->saved_plot = 1;
    }

    free_keypoints(comp_points);
    return 0;
}

static int upload_luts(AVFilterContext *ctx)
{
    CurvesCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    for (int c = 0; c < NB_COMP; c++) {
        if (s->depth == 8) {
            uint8_t tmp[256];
            for (int i = 0; i < 256; i++)
                tmp[i] = (uint8_t)s->graph[c][i];
            ret = CHECK_CU(cu->cuMemcpyHtoD(s->lut_dev[c], tmp, 256));
        } else {
            ret = CHECK_CU(cu->cuMemcpyHtoD(s->lut_dev[c], s->graph[c],
                                             s->lut_size * sizeof(uint16_t)));
        }
        if (ret < 0)
            break;
    }

    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int rebuild_and_upload(AVFilterContext *ctx)
{
    int ret;

    ret = build_graphs(ctx);
    if (ret < 0)
        return ret;
    return upload_luts(ctx);
}

static av_cold int curves_cuda_load(AVFilterContext *ctx)
{
    CurvesCUDAContext *s = ctx->priv;
    CUcontext dummy;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int ret;

    extern const unsigned char ff_vf_curves_cuda_ptx_data[];
    extern const unsigned int ff_vf_curves_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vf_curves_cuda_ptx_data, ff_vf_curves_cuda_ptx_len);
    if (ret < 0)
        goto fail;

    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_curves_nv12, s->cu_module, "curves_nv12"));
    if (ret < 0) goto fail;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_curves_p010, s->cu_module, "curves_p010"));
    if (ret < 0) goto fail;

    s->lut_bytes = (s->depth == 8) ? 256 : s->lut_size * sizeof(uint16_t);
    for (int c = 0; c < NB_COMP; c++) {
        ret = CHECK_CU(cu->cuMemAlloc(&s->lut_dev[c], s->lut_bytes));
        if (ret < 0)
            goto fail;
    }

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static av_cold int curves_cuda_init(AVFilterContext *ctx)
{
    CurvesCUDAContext *s = ctx->priv;

    s->frame = av_frame_alloc();
    s->tmp_frame = av_frame_alloc();
    if (!s->frame || !s->tmp_frame)
        return AVERROR(ENOMEM);

    return curves_cuda_init_opts(ctx);
}

static av_cold void curves_cuda_uninit(AVFilterContext *ctx)
{
    CurvesCUDAContext *s = ctx->priv;
    CUcontext dummy;

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        for (int c = 0; c < NB_COMP; c++) {
            if (s->lut_dev[c]) {
                CHECK_CU(cu->cuMemFree(s->lut_dev[c]));
                s->lut_dev[c] = 0;
            }
        }
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        s->cu_module = NULL;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    for (int i = 0; i < NB_COMP + 1; i++)
        av_freep(&s->graph[i]);

    av_frame_free(&s->frame);
    av_frame_free(&s->tmp_frame);
    av_buffer_unref(&s->frames_ctx);
}

static int curves_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    CurvesCUDAContext *s = ctx->priv;
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

    ret = curves_cuda_load(ctx);
    if (ret < 0)
        return ret;

    ret = rebuild_and_upload(ctx);
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

static int curves_cuda_apply(AVFilterContext *ctx, AVFrame *out, AVFrame *in)
{
    CurvesCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int full = (in->color_range == AVCOL_RANGE_JPEG);
    int w = in->width;
    int h = in->height;
    int y_src_pitch = in->linesize[0] / ((s->depth == 8) ? 1 : 2);
    int y_dst_pitch = out->linesize[0] / ((s->depth == 8) ? 1 : 2);
    int uv_src_pitch = in->linesize[1] / ((s->depth == 8) ? 2 : 4);
    int uv_dst_pitch = out->linesize[1] / ((s->depth == 8) ? 2 : 4);
    CUdeviceptr d_ys = (CUdeviceptr)in->data[0];
    CUdeviceptr d_yd = (CUdeviceptr)out->data[0];
    CUdeviceptr d_us = (CUdeviceptr)in->data[1];
    CUdeviceptr d_ud = (CUdeviceptr)out->data[1];
    CUfunction func = (s->depth == 8) ? s->cu_curves_nv12 : s->cu_curves_p010;
    void *args[] = {
        &d_ys, &d_us, &d_yd, &d_ud,
        &y_src_pitch, &uv_src_pitch, &y_dst_pitch, &uv_dst_pitch,
        &w, &h,
        &s->lut_dev[R], &s->lut_dev[G], &s->lut_dev[B],
        &full
    };

    return CHECK_CU(cu->cuLaunchKernel(func,
                                       DIV_UP(w, BLOCK_X), DIV_UP(h, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream,
                                       args, NULL));
}

static int curves_cuda_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    CurvesCUDAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *outl = ff_filter_link(outlink);
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CUcontext dummy;
    AVFrame *out;
    int ret;

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

    ret = curves_cuda_apply(ctx, out, in);
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
    CurvesCUDAContext *curves = ctx->priv;
    int ret;

    if (!strcmp(cmd, "plot")) {
        curves->saved_plot = 0;
    } else if (!strcmp(cmd, "all") || !strcmp(cmd, "preset") || !strcmp(cmd, "psfile") || !strcmp(cmd, "interp")) {
        if (!strcmp(cmd, "psfile"))
            curves->parsed_psfile = 0;
        av_freep(&curves->comp_points_str_all);
        av_freep(&curves->comp_points_str[0]);
        av_freep(&curves->comp_points_str[1]);
        av_freep(&curves->comp_points_str[2]);
        av_freep(&curves->comp_points_str[NB_COMP]);
    } else if (!strcmp(cmd, "red") || !strcmp(cmd, "r")) {
        av_freep(&curves->comp_points_str[0]);
    } else if (!strcmp(cmd, "green") || !strcmp(cmd, "g")) {
        av_freep(&curves->comp_points_str[1]);
    } else if (!strcmp(cmd, "blue") || !strcmp(cmd, "b")) {
        av_freep(&curves->comp_points_str[2]);
    } else if (!strcmp(cmd, "master") || !strcmp(cmd, "m")) {
        av_freep(&curves->comp_points_str[NB_COMP]);
    }

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    ret = curves_cuda_init_opts(ctx);
    if (ret < 0)
        return ret;

    if (!curves->hwctx)
        return 0;

    return rebuild_and_upload(ctx);
}

static const AVFilterPad curves_cuda_inputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .filter_frame = curves_cuda_filter_frame,
}};

static const AVFilterPad curves_cuda_outputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = curves_cuda_config_props,
}};

const FFFilter ff_vf_curves_cuda = {
    .p.name          = "curves_cuda",
    .p.description   = NULL_IF_CONFIG_SMALL("CUDA adjust components curves."),
    .p.priv_class    = &curves_cuda_class,
    .p.flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size       = sizeof(CurvesCUDAContext),
    .init            = curves_cuda_init,
    .uninit          = curves_cuda_uninit,
    .process_command = process_command,
    FILTER_INPUTS(curves_cuda_inputs),
    FILTER_OUTPUTS(curves_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
