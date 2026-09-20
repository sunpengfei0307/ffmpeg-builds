/*
 * CUDA page-clock video source (clock_cuda).
 * Parameters aligned with color_cuda; output AV_PIX_FMT_CUDA (nv12/p010).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "avfilter.h"
#include "colorspace.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "video.h"

#include "vsrc_clock_cuda.h"
#include "vsrc_clock_cuda_glyphs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCKX 32
#define BLOCKY 16

enum ClockLayout {
    CLOCK_LAYOUT_DIGITAL = 0,
    CLOCK_LAYOUT_TIME    = 1,
    CLOCK_LAYOUT_ANALOG  = 2,
};

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

typedef struct ClockCudaContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;
    AVBufferRef *own_device;
    AVBufferRef *device_ref;
    AVBufferRef *frames_ctx;

    CUmodule cu_module;
    CUfunction cu_func_fill_y_u8;
    CUfunction cu_func_fill_uv_nv12;
    CUfunction cu_func_fill_y_u16;
    CUfunction cu_func_fill_uv_p010;
    CUfunction cu_func_blit_y_u8;
    CUfunction cu_func_blit_uv_nv12;
    CUfunction cu_func_blit_y_u16;
    CUfunction cu_func_blit_uv_p010;
    CUfunction cu_func_analog_y_u8;
    CUfunction cu_func_analog_uv_nv12;
    CUfunction cu_func_analog_y_u16;
    CUfunction cu_func_analog_uv_p010;
    CUfunction cu_func_card_y_u8;
    CUfunction cu_func_card_uv_nv12;
    CUfunction cu_func_card_y_u16;
    CUfunction cu_func_card_uv_p010;
    CUstream cu_stream;
    CUdeviceptr atlas_dev;
    CUdeviceptr blits_dev;
    CUdeviceptr cards_dev;

    int device_idx;
    uint8_t color_rgba[4];
    uint8_t bg_rgba[4];
    char *out_format_string;
    int out_range;
    int colorspace;
    int color_trc;
    int layout;
    int show_date;
    int show_week;
    int show_seconds;
    int font_style;
    int font_weight;
    int font_size;
    int date_size;

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
    uint16_t y_fg, u_fg, v_fg;
    uint16_t y_bg, u_bg, v_bg;

    int draw_once_reset;
    int last_drawn_key;
    int analog_base_key;
    int re;
    int64_t pace_start_us;
    int64_t origin_us;
    AVFrame *picref;
    AVFrame *analog_base;
} ClockCudaContext;

static int resolve_color_meta(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;

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

    s->eff_range = (s->out_range == AVCOL_RANGE_UNSPECIFIED)
                   ? AVCOL_RANGE_MPEG
                   : s->out_range;
    return 0;
}

static int rgb_to_yuv_vals(AVFilterContext *ctx, const uint8_t rgba[4],
                           uint16_t *y, uint16_t *u, uint16_t *v)
{
    ClockCudaContext *s = ctx->priv;
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
        rgbad[i] = rgba[i] / 255.0;
    ff_matrix_mul_3x3_vec(yuvad, rgbad, rgb2yuv);

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
        *y = av_clip_uint8(llrint(yuvad[0] * 255.0));
        *u = av_clip_uint8(llrint(yuvad[1] * 255.0));
        *v = av_clip_uint8(llrint(yuvad[2] * 255.0));
    } else {
        *y = av_clip_uintp2(llrint(yuvad[0] * 1023.0), 10) << 6;
        *u = av_clip_uintp2(llrint(yuvad[1] * 1023.0), 10) << 6;
        *v = av_clip_uintp2(llrint(yuvad[2] * 1023.0), 10) << 6;
    }
    return 0;
}

static int compute_yuv_vals(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;
    int ret;

    ret = rgb_to_yuv_vals(ctx, s->color_rgba, &s->y_fg, &s->u_fg, &s->v_fg);
    if (ret < 0)
        return ret;
    return rgb_to_yuv_vals(ctx, s->bg_rgba, &s->y_bg, &s->u_bg, &s->v_bg);
}

static int clock_atlas_decode(uint8_t *dst, int dst_len)
{
    const unsigned char *p = clock_atlas_rle;
    const unsigned char *end = p + CLOCK_ATLAS_RLE_LEN;
    int o = 0;

    while (p < end && o < dst_len) {
        unsigned char tag, n;
        if (end - p < 2)
            return AVERROR_INVALIDDATA;
        tag = *p++;
        n = *p++;
        if (tag == 0x00) {
            unsigned char v;
            if (p >= end || o + n > dst_len)
                return AVERROR_INVALIDDATA;
            v = *p++;
            memset(dst + o, v, n);
            o += n;
        } else if (tag == 0x01) {
            if (p + n > end || o + n > dst_len)
                return AVERROR_INVALIDDATA;
            memcpy(dst + o, p, n);
            p += n;
            o += n;
        } else {
            return AVERROR_INVALIDDATA;
        }
    }
    return o == dst_len ? 0 : AVERROR_INVALIDDATA;
}

static const ClockGlyph *find_glyph(unsigned cp, int style, int weight)
{
    const ClockGlyph *any = NULL;
    int i;

    for (i = 0; i < CLOCK_GLYPH_NB; i++) {
        if (clock_glyphs[i].cp != cp)
            continue;
        if (clock_glyphs[i].style == CLOCK_GLYPH_ANY &&
            clock_glyphs[i].weight == CLOCK_GLYPH_ANY) {
            any = &clock_glyphs[i];
            continue;
        }
        if (clock_glyphs[i].style == (unsigned char)style &&
            clock_glyphs[i].weight == (unsigned char)weight)
            return &clock_glyphs[i];
    }
    return any;
}

static unsigned next_utf8(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp;

    if (!*p)
        return 0;
    if (p[0] < 0x80) {
        cp = p[0];
        p += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    } else {
        cp = p[0];
        p += 1;
    }
    *s = (const char *)p;
    return cp;
}

/* ISO-8601 week (Monday start; week 1 contains Jan 4). */
static int iso_week_number(const struct tm *tm)
{
    int iso_wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    int thu = tm->tm_yday - iso_wday + 4;
    int y = tm->tm_year + 1900;

    if (thu < 0) {
        int py = y - 1;
        int leap = ((py % 4 == 0) && (py % 100 != 0)) || (py % 400 == 0);
        thu += leap ? 366 : 365;
    } else {
        int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        int ydays = leap ? 366 : 365;
        if (thu >= ydays)
            return 1;
    }
    return thu / 7 + 1;
}

static const char *cn_weekday(int tm_wday)
{
    /* UTF-8: weekday names Sun..Sat */
    static const char *const names[] = {
        "\xE6\x97\xA5",
        "\xE4\xB8\x80",
        "\xE4\xBA\x8C",
        "\xE4\xB8\x89",
        "\xE5\x9B\x9B",
        "\xE4\xBA\x94",
        "\xE5\x85\xAD",
    };
    if (tm_wday < 0 || tm_wday > 6)
        tm_wday = 0;
    return names[tm_wday];
}

static float clock_snap_px(float v)
{
    return (float)(int)(v + (v >= 0.f ? 0.5f : -0.5f));
}

/* Auto size prefers integer magnification; explicit fontsize keeps exact px. */
static float clock_glyph_scale(float dest_h, int src_h, int exact)
{
    float raw;

    if (src_h < 1)
        return 1.f;
    raw = dest_h / (float)src_h;
    if (exact || raw < 2.f)
        return raw;
    return (float)FFMAX(2, (int)raw);
}

static int append_blits_utf8(ClockBlit *blits, int *n, const char *text,
                             float x, float y, float scale, float max_w,
                             float gap_ratio, int style, int weight)
{
    const char *p;
    unsigned cp;
    float w = 0.f, line_h = 0.f, gap, cur_x;
    int count = 0;
    const ClockGlyph *gs[CLOCK_MAX_BLITS];
    int ng = 0;

    p = text;
    while (*p && ng < CLOCK_MAX_BLITS) {
        cp = next_utf8(&p);
        if (cp == 0x20) {
            gs[ng++] = NULL;
            continue;
        }
        gs[ng] = find_glyph(cp, style, weight);
        if (gs[ng])
            line_h = FFMAX(line_h, (float)gs[ng]->h);
        ng++;
    }
    if (line_h < 1.f)
        return 0;
    gap = gap_ratio * line_h * scale;
    for (count = 0; count < ng; count++) {
        if (!gs[count])
            w += line_h * scale * 0.35f;
        else
            w += (float)gs[count]->w * scale + (count + 1 < ng ? gap : 0);
    }
    if (max_w > 0.f && w > max_w && w > 1.f) {
        float k = max_w / w;
        scale *= k;
        gap *= k;
        w *= k;
    }
    cur_x = clock_snap_px(x - w * 0.5f);
    y = clock_snap_px(y);
    for (count = 0; count < ng && *n < CLOCK_MAX_BLITS; count++) {
        const ClockGlyph *g = gs[count];
        float dw, dh;
        ClockBlit *b;
        if (!g) {
            cur_x += clock_snap_px(line_h * scale * 0.35f);
            continue;
        }
        dw = clock_snap_px((float)g->w * scale);
        dh = clock_snap_px((float)g->h * scale);
        b = &blits[(*n)++];
        b->ax = g->x;
        b->ay = g->y;
        b->aw = g->w;
        b->ah = g->h;
        b->dx = cur_x;
        b->dy = y + clock_snap_px((line_h * scale - dh) * 0.5f);
        b->dw = FFMAX(dw, 1.f);
        b->dh = FFMAX(dh, 1.f);
        cur_x += dw + clock_snap_px(gap);
    }
    return 0;
}

static int build_clock_blits(ClockCudaContext *s, ClockBlit *blits, int *nblits,
                             const struct tm *tm, int analog)
{
    char date[96], timestr[16];
    int show_date = s->show_date && s->layout != CLOCK_LAYOUT_TIME;
    int show_week = s->show_week && s->layout != CLOCK_LAYOUT_TIME;
    float time_h, date_h, time_y, date_y;

    *nblits = 0;

    if (s->show_seconds)
        snprintf(timestr, sizeof(timestr), "%02d:%02d:%02d",
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        snprintf(timestr, sizeof(timestr), "%02d:%02d",
                 tm->tm_hour, tm->tm_min);

    date[0] = 0;
    if (show_date) {
        if (show_week)
            snprintf(date, sizeof(date),
                     "%d\xE5\xB9\xB4%d\xE6\x9C\x88%d\xE6\x97\xA5 "
                     "\xE6\x98\x9F\xE6\x9C\x9F%s \xE7\xAC\xAC%d\xE5\x91\xA8",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     cn_weekday(tm->tm_wday), iso_week_number(tm));
        else
            snprintf(date, sizeof(date),
                     "%d\xE5\xB9\xB4%d\xE6\x9C\x88%d\xE6\x97\xA5 "
                     "\xE6\x98\x9F\xE6\x9C\x9F%s",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     cn_weekday(tm->tm_wday));
    } else if (show_week) {
        snprintf(date, sizeof(date),
                 "\xE6\x98\x9F\xE6\x9C\x9F%s \xE7\xAC\xAC%d\xE5\x91\xA8",
                 cn_weekday(tm->tm_wday), iso_week_number(tm));
    }

    if (s->font_size > 0)
        time_h = (float)s->font_size;
    else if (s->layout == CLOCK_LAYOUT_TIME)
        time_h = s->h * 0.34f;
    else
        time_h = s->h * 0.30f;
    if (s->date_size > 0)
        date_h = (float)s->date_size;
    else if (analog)
        date_h = s->h * 0.045f;
    else
        date_h = s->h * 0.042f;

    if (analog) {
        if (date[0]) {
            date_y = s->h * 0.86f;
            append_blits_utf8(blits, nblits, date,
                              s->w * 0.5f, date_y,
                              clock_glyph_scale(date_h, CLOCK_CJK_H, s->date_size > 0),
                              s->w * 0.86f, 0.06f, s->font_style, s->font_weight);
        }
        return 0;
    }

    if (s->layout == CLOCK_LAYOUT_TIME) {
        time_y = (s->h - time_h) * 0.50f;
        append_blits_utf8(blits, nblits, timestr,
                          s->w * 0.5f, time_y,
                          clock_glyph_scale(time_h, CLOCK_DIGIT_H, s->font_size > 0),
                          s->w * 0.90f, 0.f, s->font_style, s->font_weight);
        return 0;
    }

    date_y = s->h * 0.145f;
    time_y = s->h * 0.38f;
    if (date[0])
        append_blits_utf8(blits, nblits, date,
                          s->w * 0.5f, date_y,
                          clock_glyph_scale(date_h, CLOCK_CJK_H, s->date_size > 0),
                          s->w * 0.86f, 0.06f, s->font_style, s->font_weight);
    append_blits_utf8(blits, nblits, timestr,
                      s->w * 0.5f, time_y,
                      clock_glyph_scale(time_h, CLOCK_DIGIT_H, s->font_size > 0),
                      s->w * 0.92f, 0.f, s->font_style, s->font_weight);
    return 0;
}

static void clock_add_card(ClockCard *cards, int *n,
                           float x, float y, float w, float h, float r,
                           float y_top, float y_bot)
{
    ClockCard *c;
    if (*n >= CLOCK_MAX_CARDS)
        return;
    c = &cards[(*n)++];
    c->dx = clock_snap_px(x);
    c->dy = clock_snap_px(y);
    c->dw = clock_snap_px(w);
    c->dh = clock_snap_px(h);
    c->radius = r;
    c->y_top = y_top;
    c->y_bot = y_bot;
}

static void clock_add_glyph(ClockBlit *blits, int *n, unsigned cp,
                            float x, float y, float w, float h,
                            int style, int weight)
{
    const ClockGlyph *g;
    ClockBlit *b;
    if (*n >= CLOCK_MAX_BLITS)
        return;
    g = find_glyph(cp, style, weight);
    if (!g)
        return;
    b = &blits[(*n)++];
    b->ax = g->x;
    b->ay = g->y;
    b->aw = g->w;
    b->ah = g->h;
    b->dx = clock_snap_px(x);
    b->dy = clock_snap_px(y);
    b->dw = clock_snap_px(w);
    b->dh = clock_snap_px(h);
}

static int build_flip_clock(ClockCudaContext *s, ClockBlit *time_blits, int *n_time,
                            ClockBlit *date_blits, int *n_date,
                            ClockCard *cards, int *ncards,
                            const struct tm *tm, float y_top, float y_bot)
{
    char date[96], timestr[16];
    int show_date = s->show_date && s->layout != CLOCK_LAYOUT_TIME;
    int show_week = s->show_week && s->layout != CLOCK_LAYOUT_TIME;
    float card_h, card_w, gap, colon_gap, cluster_w, x0, y0, date_h;
    float digit_h, digit_w, rad, dot, gx, gy;
    int i, gi;

    *n_time = 0;
    *n_date = 0;
    *ncards = 0;

    if (s->show_seconds)
        snprintf(timestr, sizeof(timestr), "%02d%02d%02d",
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        snprintf(timestr, sizeof(timestr), "%02d%02d",
                 tm->tm_hour, tm->tm_min);

    date[0] = 0;
    if (show_date) {
        if (show_week)
            snprintf(date, sizeof(date),
                     "%d\xE5\xB9\xB4%d\xE6\x9C\x88%d\xE6\x97\xA5 "
                     "\xE6\x98\x9F\xE6\x9C\x9F%s \xE7\xAC\xAC%d\xE5\x91\xA8",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     cn_weekday(tm->tm_wday), iso_week_number(tm));
        else
            snprintf(date, sizeof(date),
                     "%d\xE5\xB9\xB4%d\xE6\x9C\x88%d\xE6\x97\xA5 "
                     "\xE6\x98\x9F\xE6\x9C\x9F%s",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     cn_weekday(tm->tm_wday));
    }

    if (s->font_size > 0)
        card_h = (float)s->font_size * 1.18f;
    else
        card_h = s->h * 0.26f;
    card_w = card_h * 0.82f;
    gap = card_w * 0.085f;
    colon_gap = card_w * 0.42f;
    gi = s->show_seconds ? 6 : 4;
    cluster_w = gi * card_w + (gi - 1) * gap + (s->show_seconds ? 2 : 1) * (colon_gap - gap);
    if (cluster_w > s->w * 0.94f) {
        float k = (s->w * 0.94f) / cluster_w;
        card_h *= k;
        card_w *= k;
        gap *= k;
        colon_gap *= k;
        cluster_w *= k;
    }
    x0 = (s->w - cluster_w) * 0.5f;
    y0 = show_date ? s->h * 0.28f : (s->h - card_h) * 0.48f;
    rad = card_h * 0.16f;
    digit_h = card_h * 0.72f;
    digit_w = card_w * 0.62f;
    dot = FFMAX(card_h * 0.055f, 6.f);

    for (i = 0; i < gi; i++) {
        float x = x0 + i * (card_w + gap);
        if (i >= 2)
            x += colon_gap - gap;
        if (s->show_seconds && i >= 4)
            x += colon_gap - gap;
        clock_add_card(cards, ncards, x, y0, card_w, card_h, rad, y_top, y_bot);
        gx = x + (card_w - digit_w) * 0.5f;
        gy = y0 + (card_h - digit_h) * 0.5f;
        clock_add_glyph(time_blits, n_time, (unsigned)timestr[i],
                        gx, gy, digit_w, digit_h, s->font_style, s->font_weight);
        if (i == 1 || (s->show_seconds && i == 3)) {
            float cx = x + card_w + colon_gap * 0.5f - dot * 0.5f;
            clock_add_card(cards, ncards, cx, y0 + card_h * 0.32f - dot * 0.5f,
                           dot, dot, dot * 0.5f, y_top, y_bot);
            clock_add_card(cards, ncards, cx, y0 + card_h * 0.68f - dot * 0.5f,
                           dot, dot, dot * 0.5f, y_top, y_bot);
        }
    }

    if (date[0]) {
        date_h = s->date_size > 0 ? (float)s->date_size : s->h * 0.055f;
        append_blits_utf8(date_blits, n_date, date,
                          s->w * 0.5f, y0 + card_h + s->h * 0.08f,
                          clock_glyph_scale(date_h, CLOCK_CJK_H, s->date_size > 0),
                          s->w * 0.90f, 0.04f, s->font_style, s->font_weight);
    }
    return 0;
}

static av_cold int clock_cuda_load_functions(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    uint8_t *atlas = NULL;
    int ret;

    extern const unsigned char ff_vsrc_clock_cuda_ptx_data[];
    extern const unsigned int ff_vsrc_clock_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                              ff_vsrc_clock_cuda_ptx_data,
                              ff_vsrc_clock_cuda_ptx_len);
    if (ret < 0)
        goto fail;

#define GET_FN(field, name) do { \
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->field, s->cu_module, name)); \
    if (ret < 0) goto fail; \
} while (0)

    GET_FN(cu_func_fill_y_u8,       "clock_fill_y_u8");
    GET_FN(cu_func_fill_uv_nv12,    "clock_fill_uv_nv12");
    GET_FN(cu_func_fill_y_u16,      "clock_fill_y_u16");
    GET_FN(cu_func_fill_uv_p010,    "clock_fill_uv_p010");
    GET_FN(cu_func_blit_y_u8,       "clock_blit_y_u8");
    GET_FN(cu_func_blit_uv_nv12,    "clock_blit_uv_nv12");
    GET_FN(cu_func_blit_y_u16,      "clock_blit_y_u16");
    GET_FN(cu_func_blit_uv_p010,    "clock_blit_uv_p010");
    GET_FN(cu_func_analog_y_u8,     "clock_analog_y_u8");
    GET_FN(cu_func_analog_uv_nv12,  "clock_analog_uv_nv12");
    GET_FN(cu_func_analog_y_u16,    "clock_analog_y_u16");
    GET_FN(cu_func_analog_uv_p010,  "clock_analog_uv_p010");
    GET_FN(cu_func_card_y_u8,       "clock_card_y_u8");
    GET_FN(cu_func_card_uv_nv12,    "clock_card_uv_nv12");
    GET_FN(cu_func_card_y_u16,      "clock_card_y_u16");
    GET_FN(cu_func_card_uv_p010,    "clock_card_uv_p010");
#undef GET_FN

    atlas = av_malloc(CLOCK_ATLAS_W * CLOCK_ATLAS_H);
    if (!atlas) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = clock_atlas_decode(atlas, CLOCK_ATLAS_W * CLOCK_ATLAS_H);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "clock_cuda: glyph atlas decode failed\n");
        goto fail;
    }
    ret = CHECK_CU(cu->cuMemAlloc(&s->atlas_dev, CLOCK_ATLAS_W * CLOCK_ATLAS_H));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuMemcpyHtoD(s->atlas_dev, atlas,
                                    CLOCK_ATLAS_W * CLOCK_ATLAS_H));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuMemAlloc(&s->blits_dev,
                                  sizeof(ClockBlit) * CLOCK_MAX_BLITS));
    if (ret < 0)
        goto fail;
    ret = CHECK_CU(cu->cuMemAlloc(&s->cards_dev,
                                  sizeof(ClockCard) * CLOCK_MAX_CARDS));

fail:
    av_free(atlas);
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int clock_cuda_fill_bg(AVFilterContext *ctx, AVFrame *frame)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int uv_w = AV_CEIL_RSHIFT(frame->width, 1);
    int uv_h = AV_CEIL_RSHIFT(frame->height, 1);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr uv = (CUdeviceptr)frame->data[1];
    int pitch_y = frame->linesize[0];
    int pitch_uv = frame->linesize[1];
    unsigned char y8, u8, v8;
    unsigned short y16, u16, v16;
    int ret = 0;

    if (s->sw_format == AV_PIX_FMT_NV12) {
        y8 = (unsigned char)s->y_bg;
        u8 = (unsigned char)s->u_bg;
        v8 = (unsigned char)s->v_bg;
        {
            void *args_y[] = { &y, &pitch_y, &frame->width, &frame->height, &y8 };
            void *args_uv[] = { &uv, &pitch_uv, &uv_w, &uv_h, &u8, &v8 };
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_fill_y_u8,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                return ret;
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_fill_uv_nv12,
                                              DIV_UP(uv_w, BLOCKX),
                                              DIV_UP(uv_h, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_uv, NULL));
        }
    } else {
        y16 = s->y_bg;
        u16 = s->u_bg;
        v16 = s->v_bg;
        {
            void *args_y[] = { &y, &pitch_y, &frame->width, &frame->height, &y16 };
            void *args_uv[] = { &uv, &pitch_uv, &uv_w, &uv_h, &u16, &v16 };
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_fill_y_u16,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                return ret;
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_fill_uv_p010,
                                              DIV_UP(uv_w, BLOCKX),
                                              DIV_UP(uv_h, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_uv, NULL));
        }
    }
    return ret;
}

static int clock_cuda_copy_plane(AVFilterContext *ctx, AVFrame *dst, AVFrame *src)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int i, ret;

    for (i = 0; i < 2; i++) {
        CUDA_MEMCPY2D cpy = { 0 };
        if (!src->data[i] || !dst->data[i])
            continue;
        cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.srcDevice     = (CUdeviceptr)src->data[i];
        cpy.dstDevice     = (CUdeviceptr)dst->data[i];
        cpy.srcPitch      = src->linesize[i];
        cpy.dstPitch      = dst->linesize[i];
        cpy.WidthInBytes  = FFMIN(src->linesize[i], dst->linesize[i]);
        cpy.Height        = src->height >> (i ? 1 : 0);
        ret = CHECK_CU(cu->cuMemcpy2DAsync(&cpy, s->cu_stream));
        if (ret < 0)
            return ret;
    }
    return CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
}

static AVFrame *clock_cuda_alloc_frame(AVFilterLink *outlink, int w, int h)
{
    AVFrame *f = ff_get_video_buffer(outlink, w, h);
    if (f) {
        f->width  = w;
        f->height = h;
    }
    return f;
}

static int clock_cuda_launch_analog(AVFilterContext *ctx, AVFrame *frame,
                                    const struct tm *tm, double sec_frac, int mode)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int uv_w = AV_CEIL_RSHIFT(frame->width, 1);
    int uv_h = AV_CEIL_RSHIFT(frame->height, 1);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr uv = (CUdeviceptr)frame->data[1];
    int pitch_y = frame->linesize[0];
    int pitch_uv = frame->linesize[1];
    unsigned char y8, u8, v8;
    unsigned short y16, u16, v16;
    int show_seconds = s->show_seconds;
    float cx, cy, radius, hourf, minf, secf, h_ang, m_ang, s_ang;
    int ret;

    cx = s->w * 0.5f;
    cy = (s->show_date && s->layout != CLOCK_LAYOUT_TIME) ? s->h * 0.46f : s->h * 0.50f;
    radius = FFMIN(s->w, s->h) * 0.32f;
    hourf = tm->tm_hour % 12 + tm->tm_min / 60.0 + (tm->tm_sec + sec_frac) / 3600.0;
    minf  = tm->tm_min + (tm->tm_sec + sec_frac) / 60.0;
    secf  = tm->tm_sec + sec_frac;
    h_ang = hourf * (float)(M_PI / 6.0);
    m_ang = minf  * (float)(M_PI / 30.0);
    s_ang = secf  * (float)(M_PI / 30.0);

    if (s->sw_format == AV_PIX_FMT_NV12) {
        y8 = (unsigned char)s->y_fg;
        u8 = (unsigned char)s->u_fg;
        v8 = (unsigned char)s->v_fg;
        {
            void *args_y[] = {
                &y, &pitch_y, &frame->width, &frame->height,
                &cx, &cy, &radius, &h_ang, &m_ang, &s_ang, &y8, &show_seconds, &mode
            };
            void *args_uv[] = {
                &uv, &pitch_uv, &uv_w, &uv_h,
                &cx, &cy, &radius, &h_ang, &m_ang, &s_ang, &u8, &v8, &show_seconds, &mode
            };
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_analog_y_u8,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                return ret;
            return CHECK_CU(cu->cuLaunchKernel(s->cu_func_analog_uv_nv12,
                                               DIV_UP(uv_w, BLOCKX),
                                               DIV_UP(uv_h, BLOCKY),
                                               1, BLOCKX, BLOCKY, 1, 0,
                                               s->cu_stream, args_uv, NULL));
        }
    }
    y16 = s->y_fg;
    u16 = s->u_fg;
    v16 = s->v_fg;
    {
        void *args_y[] = {
            &y, &pitch_y, &frame->width, &frame->height,
            &cx, &cy, &radius, &h_ang, &m_ang, &s_ang, &y16, &show_seconds, &mode
        };
        void *args_uv[] = {
            &uv, &pitch_uv, &uv_w, &uv_h,
            &cx, &cy, &radius, &h_ang, &m_ang, &s_ang, &u16, &v16, &show_seconds, &mode
        };
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_analog_y_u16,
                                          DIV_UP(frame->width, BLOCKX),
                                          DIV_UP(frame->height, BLOCKY),
                                          1, BLOCKX, BLOCKY, 1, 0,
                                          s->cu_stream, args_y, NULL));
        if (ret < 0)
            return ret;
        return CHECK_CU(cu->cuLaunchKernel(s->cu_func_analog_uv_p010,
                                           DIV_UP(uv_w, BLOCKX),
                                           DIV_UP(uv_h, BLOCKY),
                                           1, BLOCKX, BLOCKY, 1, 0,
                                           s->cu_stream, args_uv, NULL));
    }
}

static int clock_cuda_launch_blit(AVFilterContext *ctx, AVFrame *frame,
                                  ClockBlit *blits, int nblits,
                                  uint16_t yv, uint16_t uv_u, uint16_t uv_v)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int uv_w = AV_CEIL_RSHIFT(frame->width, 1);
    int uv_h = AV_CEIL_RSHIFT(frame->height, 1);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr uv = (CUdeviceptr)frame->data[1];
    int pitch_y = frame->linesize[0];
    int pitch_uv = frame->linesize[1];
    int atlas_w = CLOCK_ATLAS_W, atlas_h = CLOCK_ATLAS_H;
    unsigned char y8, u8, v8;
    unsigned short y16, u16, v16;
    int ret;

    if (nblits <= 0)
        return 0;
    ret = CHECK_CU(cu->cuMemcpyHtoD(s->blits_dev, blits, sizeof(ClockBlit) * nblits));
    if (ret < 0)
        return ret;
    if (s->sw_format == AV_PIX_FMT_NV12) {
        y8 = (unsigned char)yv;
        u8 = (unsigned char)uv_u;
        v8 = (unsigned char)uv_v;
        {
            void *args_y[] = {
                &y, &pitch_y, &frame->width, &frame->height,
                &s->atlas_dev, &atlas_w, &atlas_h,
                &s->blits_dev, &nblits, &y8
            };
            void *args_uv[] = {
                &uv, &pitch_uv, &uv_w, &uv_h,
                &s->atlas_dev, &atlas_w, &atlas_h,
                &s->blits_dev, &nblits, &u8, &v8
            };
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_blit_y_u8,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                return ret;
            return CHECK_CU(cu->cuLaunchKernel(s->cu_func_blit_uv_nv12,
                                               DIV_UP(uv_w, BLOCKX),
                                               DIV_UP(uv_h, BLOCKY),
                                               1, BLOCKX, BLOCKY, 1, 0,
                                               s->cu_stream, args_uv, NULL));
        }
    }
    y16 = yv;
    u16 = uv_u;
    v16 = uv_v;
    {
        void *args_y[] = {
            &y, &pitch_y, &frame->width, &frame->height,
            &s->atlas_dev, &atlas_w, &atlas_h,
            &s->blits_dev, &nblits, &y16
        };
        void *args_uv[] = {
            &uv, &pitch_uv, &uv_w, &uv_h,
            &s->atlas_dev, &atlas_w, &atlas_h,
            &s->blits_dev, &nblits, &u16, &v16
        };
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_blit_y_u16,
                                          DIV_UP(frame->width, BLOCKX),
                                          DIV_UP(frame->height, BLOCKY),
                                          1, BLOCKX, BLOCKY, 1, 0,
                                          s->cu_stream, args_y, NULL));
        if (ret < 0)
            return ret;
        return CHECK_CU(cu->cuLaunchKernel(s->cu_func_blit_uv_p010,
                                           DIV_UP(uv_w, BLOCKX),
                                           DIV_UP(uv_h, BLOCKY),
                                           1, BLOCKX, BLOCKY, 1, 0,
                                           s->cu_stream, args_uv, NULL));
    }
}

static int clock_cuda_launch_cards(AVFilterContext *ctx, AVFrame *frame,
                                   ClockCard *cards, int ncards,
                                   uint16_t u_card, uint16_t v_card)
{
    ClockCudaContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int uv_w = AV_CEIL_RSHIFT(frame->width, 1);
    int uv_h = AV_CEIL_RSHIFT(frame->height, 1);
    CUdeviceptr y = (CUdeviceptr)frame->data[0];
    CUdeviceptr uv = (CUdeviceptr)frame->data[1];
    int pitch_y = frame->linesize[0];
    int pitch_uv = frame->linesize[1];
    unsigned char u8, v8;
    unsigned short u16, v16;
    int ret;

    if (ncards <= 0)
        return 0;
    ret = CHECK_CU(cu->cuMemcpyHtoD(s->cards_dev, cards, sizeof(ClockCard) * ncards));
    if (ret < 0)
        return ret;
    if (s->sw_format == AV_PIX_FMT_NV12) {
        u8 = (unsigned char)u_card;
        v8 = (unsigned char)v_card;
        {
            void *args_y[] = {
                &y, &pitch_y, &frame->width, &frame->height, &s->cards_dev, &ncards
            };
            void *args_uv[] = {
                &uv, &pitch_uv, &uv_w, &uv_h, &s->cards_dev, &ncards, &u8, &v8
            };
            ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_card_y_u8,
                                              DIV_UP(frame->width, BLOCKX),
                                              DIV_UP(frame->height, BLOCKY),
                                              1, BLOCKX, BLOCKY, 1, 0,
                                              s->cu_stream, args_y, NULL));
            if (ret < 0)
                return ret;
            return CHECK_CU(cu->cuLaunchKernel(s->cu_func_card_uv_nv12,
                                               DIV_UP(uv_w, BLOCKX),
                                               DIV_UP(uv_h, BLOCKY),
                                               1, BLOCKX, BLOCKY, 1, 0,
                                               s->cu_stream, args_uv, NULL));
        }
    }
    u16 = u_card;
    v16 = v_card;
    {
        void *args_y[] = {
            &y, &pitch_y, &frame->width, &frame->height, &s->cards_dev, &ncards
        };
        void *args_uv[] = {
            &uv, &pitch_uv, &uv_w, &uv_h, &s->cards_dev, &ncards, &u16, &v16
        };
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_func_card_y_u16,
                                          DIV_UP(frame->width, BLOCKX),
                                          DIV_UP(frame->height, BLOCKY),
                                          1, BLOCKX, BLOCKY, 1, 0,
                                          s->cu_stream, args_y, NULL));
        if (ret < 0)
            return ret;
        return CHECK_CU(cu->cuLaunchKernel(s->cu_func_card_uv_p010,
                                           DIV_UP(uv_w, BLOCKX),
                                           DIV_UP(uv_h, BLOCKY),
                                           1, BLOCKX, BLOCKY, 1, 0,
                                           s->cu_stream, args_uv, NULL));
    }
}

static int clock_cuda_draw(AVFilterContext *ctx, AVFrame *frame,
                           const struct tm *tm, double sec_frac, int analog_mode)
{
    ClockCudaContext *s = ctx->priv;
    ClockBlit time_blits[CLOCK_MAX_BLITS], date_blits[CLOCK_MAX_BLITS];
    ClockCard cards[CLOCK_MAX_CARDS];
    int n_time = 0, n_date = 0, ncards = 0, analog, ret;
    uint8_t rgba[4];
    uint16_t y_top, y_bot, y_dig, u_card, v_card, u_dig, v_dig;

    analog = s->layout == CLOCK_LAYOUT_ANALOG;
    if (analog_mode != 2) {
        ret = clock_cuda_fill_bg(ctx, frame);
        if (ret < 0)
            return ret;
    }

    if (analog) {
        if (analog_mode & 1) {
            build_clock_blits(s, date_blits, &n_date, tm, 1);
            ret = clock_cuda_launch_analog(ctx, frame, tm, sec_frac, 1);
            if (ret < 0)
                return ret;
            ret = clock_cuda_launch_blit(ctx, frame, date_blits, n_date,
                                         s->y_fg, s->u_fg, s->v_fg);
            if (ret < 0)
                return ret;
        }
        if (analog_mode & 2) {
            ret = clock_cuda_launch_analog(ctx, frame, tm, sec_frac, 2);
            if (ret < 0)
                return ret;
        }
        return CHECK_CU(s->hwctx->internal->cuda_dl->cuStreamSynchronize(s->cu_stream));
    }

    rgba[0] = 0xD6; rgba[1] = 0xD6; rgba[2] = 0xD6; rgba[3] = 255;
    ret = rgb_to_yuv_vals(ctx, rgba, &y_top, &u_card, &v_card);
    if (ret < 0)
        return ret;
    rgba[0] = 0xBE; rgba[1] = 0xBE; rgba[2] = 0xBE;
    ret = rgb_to_yuv_vals(ctx, rgba, &y_bot, &u_card, &v_card);
    if (ret < 0)
        return ret;
    rgba[0] = 0x2C; rgba[1] = 0x2C; rgba[2] = 0x2C;
    ret = rgb_to_yuv_vals(ctx, rgba, &y_dig, &u_dig, &v_dig);
    if (ret < 0)
        return ret;

    build_flip_clock(s, time_blits, &n_time, date_blits, &n_date,
                     cards, &ncards, tm, (float)y_top, (float)y_bot);
    ret = clock_cuda_launch_cards(ctx, frame, cards, ncards, u_card, v_card);
    if (ret < 0)
        return ret;
    ret = clock_cuda_launch_blit(ctx, frame, time_blits, n_time, y_dig, u_dig, v_dig);
    if (ret < 0)
        return ret;
    ret = clock_cuda_launch_blit(ctx, frame, date_blits, n_date,
                                 s->y_fg, s->u_fg, s->v_fg);
    if (ret < 0)
        return ret;
    return CHECK_CU(s->hwctx->internal->cuda_dl->cuStreamSynchronize(s->cu_stream));
}

static av_cold int clock_cuda_init(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;

    s->time_base = av_inv_q(s->frame_rate);
    s->nb_frame = 0;
    s->pts = 0;
    s->draw_once_reset = 0;
    s->last_drawn_key = -1;
    s->analog_base_key = -1;
    s->origin_us = 0;
    s->pace_start_us = 0;
    return 0;
}

static av_cold void clock_cuda_uninit(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;
    CUcontext dummy;

    av_frame_free(&s->picref);
    av_frame_free(&s->analog_base);
    av_buffer_unref(&s->frames_ctx);

    if (s->hwctx) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (s->atlas_dev)
            CHECK_CU(cu->cuMemFree(s->atlas_dev));
        if (s->blits_dev)
            CHECK_CU(cu->cuMemFree(s->blits_dev));
        if (s->cards_dev)
            CHECK_CU(cu->cuMemFree(s->cards_dev));
        if (s->cu_module)
            CHECK_CU(cu->cuModuleUnload(s->cu_module));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }
    s->atlas_dev = 0;
    s->blits_dev = 0;
    s->cards_dev = 0;
    s->cu_module = NULL;
    s->hwctx = NULL;
    s->device_ref = NULL;
    av_buffer_unref(&s->own_device);
}

static int clock_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ClockCudaContext *s = ctx->priv;
    FilterLink *l = ff_filter_link(outlink);
    AVHWFramesContext *frames_ctx;
    AVHWDeviceContext *device_ctx;
    int ret;

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
    frames_ctx->initial_pool_size = 16;

    ret = av_hwframe_ctx_init(s->frames_ctx);
    if (ret < 0) {
        av_buffer_unref(&s->frames_ctx);
        return ret;
    }

    ret = clock_cuda_load_functions(ctx);
    if (ret < 0)
        return ret;

    s->time_base = av_inv_q(s->frame_rate);
    s->nb_frame = 0;
    s->pts = 0;
    s->last_drawn_key = -1;
    s->analog_base_key = -1;
    s->origin_us = 0;
    s->pace_start_us = 0;

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->sample_aspect_ratio = s->sar;
    outlink->time_base = s->time_base;
    l->frame_rate = s->frame_rate;

    av_buffer_unref(&l->hw_frames_ctx);
    l->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!l->hw_frames_ctx)
        return AVERROR(ENOMEM);

    av_log(ctx, AV_LOG_VERBOSE,
           "clock_cuda size:%dx%d rate:%d/%d layout:%d format:%s\n",
           s->w, s->h, s->frame_rate.num, s->frame_rate.den, s->layout,
           av_get_pix_fmt_name(s->sw_format));
    return 0;
}

static int clock_from_us(int64_t us, struct tm *tm_out, double *sec_frac)
{
    time_t sec = (time_t)(us / 1000000);
    struct tm tmbuf, *tm;

#ifdef _WIN32
    tm = (localtime_s(&tmbuf, &sec) == 0) ? &tmbuf : NULL;
#elif HAVE_LOCALTIME_R
    tm = localtime_r(&sec, &tmbuf);
#else
    {
        struct tm *tmp = localtime(&sec);
        if (tmp) {
            tmbuf = *tmp;
            tm = &tmbuf;
        } else {
            tm = NULL;
        }
    }
#endif
    if (!tm)
        return AVERROR_EXTERNAL;
    *tm_out = *tm;
    if (sec_frac)
        *sec_frac = (us % 1000000) / 1000000.0;
    return 0;
}

static int clock_cuda_activate(AVFilterContext *ctx)
{
    ClockCudaContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *frame;
    struct tm tm;
    double sec_frac = 0;
    int key, analog, ret;
    CUcontext dummy;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;

    if (s->re) {
        int64_t now = av_gettime_relative();
        int64_t due, delay;
        if (s->pace_start_us <= 0)
            s->pace_start_us = now;
        due = s->pace_start_us + av_rescale_q(s->pts, s->time_base, AV_TIME_BASE_Q);
        delay = due - now;
        if (delay > 0)
            av_usleep(delay > 1000000 ? 1000000 : delay);
    }

    if (s->duration >= 0 &&
        av_rescale_q(s->pts, s->time_base, AV_TIME_BASE_Q) >= s->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (s->origin_us <= 0)
        s->origin_us = av_gettime();
    ret = clock_from_us(s->origin_us +
                        av_rescale_q(s->pts, s->time_base, AV_TIME_BASE_Q),
                        &tm, &sec_frac);
    if (ret < 0)
        return ret;

    analog = s->layout == CLOCK_LAYOUT_ANALOG;
    if (analog)
        key = tm.tm_hour * 3600000 + tm.tm_min * 60000 +
              tm.tm_sec * 1000 + (int)(sec_frac * 1000);
    else if (s->show_seconds)
        key = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    else
        key = tm.tm_hour * 60 + tm.tm_min;

    if (s->draw_once_reset) {
        av_frame_free(&s->picref);
        av_frame_free(&s->analog_base);
        s->draw_once_reset = 0;
        s->last_drawn_key = -1;
        s->analog_base_key = -1;
    }

    ret = CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    if (analog) {
        int face_key = (tm.tm_year + 1900) * 10000 +
                       (tm.tm_mon + 1) * 100 + tm.tm_mday;
        if (!s->analog_base || s->analog_base_key != face_key) {
            av_frame_free(&s->analog_base);
            s->analog_base = ff_get_video_buffer(outlink, s->w, s->h);
            if (!s->analog_base) {
                CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
                return AVERROR(ENOMEM);
            }
            s->analog_base->width  = s->w;
            s->analog_base->height = s->h;
            ret = clock_cuda_draw(ctx, s->analog_base, &tm, sec_frac, 1);
            if (ret < 0) {
                CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
                return ret;
            }
            s->analog_base_key = face_key;
        }
        frame = ff_get_video_buffer(outlink, s->w, s->h);
        if (!frame) {
            CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
            return AVERROR(ENOMEM);
        }
        frame->width  = s->w;
        frame->height = s->h;
        ret = clock_cuda_copy_plane(ctx, frame, s->analog_base);
        if (ret >= 0)
            ret = clock_cuda_draw(ctx, frame, &tm, sec_frac, 2);
        CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    } else {
        if (!s->picref || s->last_drawn_key != key) {
            if (!s->picref) {
                s->picref = clock_cuda_alloc_frame(outlink, s->w, s->h);
                if (!s->picref) {
                    CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
                    return AVERROR(ENOMEM);
                }
            }
            ret = clock_cuda_draw(ctx, s->picref, &tm, sec_frac, 0);
            if (ret < 0) {
                CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
                return ret;
            }
            s->last_drawn_key = key;
        }
        frame = clock_cuda_alloc_frame(outlink, s->w, s->h);
        if (!frame) {
            CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
            return AVERROR(ENOMEM);
        }
        ret = clock_cuda_copy_plane(ctx, frame, s->picref);
        CHECK_CU(s->hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    }

    frame->pts                 = s->pts;
    frame->duration            = 1;
    frame->flags              &= ~AV_FRAME_FLAG_INTERLACED;
    if (s->nb_frame == 0) {
        frame->flags    |= AV_FRAME_FLAG_KEY;
        frame->pict_type = AV_PICTURE_TYPE_I;
    } else {
        frame->flags    &= ~AV_FRAME_FLAG_KEY;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }
    frame->sample_aspect_ratio = s->sar;
    frame->color_range         = s->eff_range;
    frame->colorspace          = s->eff_csp;
    frame->color_primaries     = s->eff_pri;
    frame->color_trc           = s->eff_trc;

    s->pts++;
    s->nb_frame++;
    return ff_filter_frame(outlink, frame);
}

static int clock_cuda_process_command(AVFilterContext *ctx, const char *cmd,
                                      const char *args, char *res, int res_len,
                                      int flags)
{
    ClockCudaContext *s = ctx->priv;
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

#define OFFSET(x) offsetof(ClockCudaContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define FLAGSR (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption clock_cuda_options[] = {
    { "color", "set foreground / text color", OFFSET(color_rgba), AV_OPT_TYPE_COLOR, {.str = "white"}, 0, 0, FLAGSR },
    { "c",     "set foreground / text color", OFFSET(color_rgba), AV_OPT_TYPE_COLOR, {.str = "white"}, 0, 0, FLAGSR },
    { "bgcolor", "set background color", OFFSET(bg_rgba), AV_OPT_TYPE_COLOR, {.str = "black"}, 0, 0, FLAGSR },
    { "bg",      "set background color", OFFSET(bg_rgba), AV_OPT_TYPE_COLOR, {.str = "black"}, 0, 0, FLAGSR },

    { "size", "set video size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, { .str = "1920x1080" }, 0, 0, FLAGS },
    { "s",    "set video size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, { .str = "1920x1080" }, 0, 0, FLAGS },

    { "rate", "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, FLAGS },
    { "r",    "set video rate", OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "25" }, 0, INT_MAX, FLAGS },

    { "re", "pace output to r (1=realtime, 0=as fast as possible)", OFFSET(re),
      AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },

    { "duration", "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, { .i64 = -1 }, -1, INT64_MAX, FLAGS },
    { "d",        "set video duration", OFFSET(duration), AV_OPT_TYPE_DURATION, { .i64 = -1 }, -1, INT64_MAX, FLAGS },

    { "sar", "set video sample aspect ratio", OFFSET(sar), AV_OPT_TYPE_RATIONAL, { .dbl = 1 },  0, INT_MAX, FLAGS },

    { "layout", "page clock layout", OFFSET(layout), AV_OPT_TYPE_INT,
      { .i64 = CLOCK_LAYOUT_DIGITAL }, 0, 2, FLAGSR, .unit = "layout" },
        { "digital", "date + weekday + ISO week + large HH:MM:SS", 0, AV_OPT_TYPE_CONST,
          { .i64 = CLOCK_LAYOUT_DIGITAL }, 0, 0, FLAGSR, .unit = "layout" },
        { "time", "large digital time only (no date/week)", 0, AV_OPT_TYPE_CONST,
          { .i64 = CLOCK_LAYOUT_TIME }, 0, 0, FLAGSR, .unit = "layout" },
        { "analog", "analog clock (optional date below)", 0, AV_OPT_TYPE_CONST,
          { .i64 = CLOCK_LAYOUT_ANALOG }, 0, 0, FLAGSR, .unit = "layout" },

    { "show_date", "show Chinese date line", OFFSET(show_date), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGSR },
    { "show_week", "show weekday and ISO week number", OFFSET(show_week), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGSR },
    { "show_seconds", "show seconds (digital digits / analog second hand)", OFFSET(show_seconds), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGSR },

    { "font", "digit typeface", OFFSET(font_style), AV_OPT_TYPE_INT,
      { .i64 = 0 }, 0, 2, FLAGSR, .unit = "font" },
    { "fontstyle", "digit typeface", OFFSET(font_style), AV_OPT_TYPE_INT,
      { .i64 = 0 }, 0, 2, FLAGSR, .unit = "font" },
        { "segoe",   "Segoe UI (rounded)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGSR, .unit = "font" },
        { "calibri", "Calibri",            0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGSR, .unit = "font" },
        { "mono",    "Consolas (monospace)", 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, FLAGSR, .unit = "font" },

    { "fontweight", "digit weight", OFFSET(font_weight), AV_OPT_TYPE_INT,
      { .i64 = 1 }, 0, 3, FLAGSR, .unit = "weight" },
    { "weight", "digit weight", OFFSET(font_weight), AV_OPT_TYPE_INT,
      { .i64 = 1 }, 0, 3, FLAGSR, .unit = "weight" },
        { "light",    "light",    0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGSR, .unit = "weight" },
        { "regular",  "regular",  0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGSR, .unit = "weight" },
        { "medium",   "medium",   0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, FLAGSR, .unit = "weight" },
        { "bold",     "bold",     0, AV_OPT_TYPE_CONST, { .i64 = 3 }, 0, 0, FLAGSR, .unit = "weight" },

    { "fontsize", "HH:MM:SS height in pixels (0 = auto ~30% of frame)", OFFSET(font_size),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 4096, FLAGSR },
    { "datesize", "date line height in pixels (0 = auto)", OFFSET(date_size),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 4096, FLAGSR },

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

AVFILTER_DEFINE_CLASS(clock_cuda);

static const AVFilterPad clock_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = clock_cuda_config_props,
    },
};

const FFFilter ff_vsrc_clock_cuda = {
    .p.name         = "clock_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("Generate a page clock (CUDA)"),
    .p.inputs       = NULL,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .p.priv_class   = &clock_cuda_class,
    .priv_size      = sizeof(ClockCudaContext),
    .init           = clock_cuda_init,
    .uninit         = clock_cuda_uninit,
    .activate       = clock_cuda_activate,
    FILTER_OUTPUTS(clock_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .process_command = clock_cuda_process_command,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
