/*
 * CUDA drawtext. Glyphs are rasterized with FreeType and blended on the GPU.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/cuda_check.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
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

#define DT_TEXT      768
#define DT_MAX_LINES 8
#define DT_MAX_QUEUE 16
#define DT_GLYPHS    128
#define DT_SPANS     48

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
    int shift;
    int semi;
    int packed;
} FormatInfo;

static const FormatInfo supported_formats[] = {
    { AV_PIX_FMT_YUV420P,   8,  0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV422P,   8,  0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV444P,   8,  0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV420P10, 10, 0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV422P10, 10, 0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV444P10, 10, 0, 0, PACK_NONE },
    { AV_PIX_FMT_YUV444P16, 16, 0, 0, PACK_NONE },
    { AV_PIX_FMT_NV12,      8,  0, 1, PACK_NONE },
    { AV_PIX_FMT_NV16,      8,  0, 1, PACK_NONE },
    { AV_PIX_FMT_P010,      10, 6, 1, PACK_NONE },
    { AV_PIX_FMT_P210,      10, 6, 1, PACK_NONE },
    { AV_PIX_FMT_P016,      16, 0, 1, PACK_NONE },
    { AV_PIX_FMT_P216,      16, 0, 1, PACK_NONE },
    { AV_PIX_FMT_0RGB32,    8,  0, 0, PACK_BGR0 },
    { AV_PIX_FMT_0BGR32,    8,  0, 0, PACK_RGB0 },
    { AV_PIX_FMT_RGB32,     8,  0, 0, PACK_BGRA },
    { AV_PIX_FMT_BGR32,     8,  0, 0, PACK_RGBA },
};

typedef struct Glyph {
    uint32_t cp;
    int index;
    int advance;
    int left, top;
    int bw, bh;
    uint8_t *gray;
    int valid;
} Glyph;

typedef struct DTItem {
    double pts;
    char text[DT_TEXT];
} DTItem;

typedef struct DrawTextCUDAContext {
    const AVClass *class;

    char *text;
    char *font;
    int fontindex;
    int fontsize;
    int spacing;
    int line_spacing;
    int bold;
    int underline;
    uint8_t font_rgba[4];
    double alpha;
    int box;
    uint8_t box_rgba[4];
    double boxalpha;
    int x, y, w, h;
    int boxpad;
    int lines;
    int timeline;

    char line[DT_MAX_LINES][DT_TEXT];
    int nlines;
    DTItem queue[DT_MAX_QUEUE];
    int nq;
    int dirty;
    int font_dirty;
    int need_upload;
    int have_frame;
    double last_frame_sec;
    char last_prefix[24];

    FT_Library library;
    FT_Face face;
    int opened_size;
    int cached_bold;
    Glyph glyphs[DT_GLYPHS];
    int nglyph;

    uint8_t *bmp;
    int bmp_cap;
    int bmp_w, bmp_h;

    AVCUDADeviceContext *hwctx;
    enum AVPixelFormat sw_format;
    const FormatInfo *fmt;
    CUmodule cu_module;
    CUfunction cu_copy;
    CUfunction cu_y8, cu_y16, cu_uv8, cu_uv16, cu_packed;
    CUdeviceptr dev_bmp;
    int dev_cap;
} DrawTextCUDAContext;

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

static int max_lines(const DrawTextCUDAContext *s)
{
    int n = s->lines;
    if (n < 1)
        n = 1;
    if (n > DT_MAX_LINES)
        n = DT_MAX_LINES;
    return n;
}

static int scale_alpha(int a, double mul)
{
    int v;
    if (mul < 0)
        mul = 0;
    if (mul > 1)
        mul = 1;
    v = (int)(a * mul + 0.5);
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return v;
}

static int utf8_next(const uint8_t *s, int avail, uint32_t *cp)
{
    if (avail <= 0 || !s[0])
        return 0;
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0 && avail >= 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return *cp >= 0x80 ? 2 : 1;
    }
    if ((s[0] & 0xF0) == 0xE0 && avail >= 3 &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && avail >= 4 &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
              ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = s[0];
    return 1;
}

static void format_timeline(char *buf, size_t n, int64_t pts, AVRational tb)
{
    int64_t total_ms, h;
    int m, sec, milli;

    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) {
        snprintf(buf, n, "[00:00:00.000]");
        return;
    }
    total_ms = av_rescale_rnd(pts, (int64_t)tb.num * 1000, tb.den, AV_ROUND_NEAR_INF);
    if (total_ms < 0)
        total_ms = 0;
    h = total_ms / 3600000;
    m = (int)((total_ms / 60000) % 60);
    sec = (int)((total_ms / 1000) % 60);
    milli = (int)(total_ms % 1000);
    snprintf(buf, n, "[%02" PRId64 ":%02d:%02d.%03d]", h, m, sec, milli);
}

static void unescape_text(char *dst, int dst_sz, const char *src)
{
    int i = 0;
    if (!src)
        src = "";
    while (*src && i < dst_sz - 1) {
        if (src[0] == '\\' && src[1] == 'n') {
            dst[i++] = '\n';
            src += 2;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

static void push_line(DrawTextCUDAContext *s, const char *start, int len)
{
    int maxl = max_lines(s);
    if (len < 0)
        len = 0;
    if (len >= DT_TEXT)
        len = DT_TEXT - 1;
    while (s->nlines >= maxl) {
        memmove(s->line[0], s->line[1], (size_t)(s->nlines - 1) * DT_TEXT);
        s->nlines--;
    }
    memcpy(s->line[s->nlines], start, len);
    s->line[s->nlines][len] = 0;
    s->nlines++;
}

static void push_text(DrawTextCUDAContext *s, const char *text)
{
    const char *p, *start;
    if (!text)
        return;
    start = text;
    for (p = text; *p; p++) {
        if (*p == '\n') {
            push_line(s, start, (int)(p - start));
            start = p + 1;
        }
    }
    push_line(s, start, (int)(p - start));
}

static void trim_lines(DrawTextCUDAContext *s)
{
    int maxl = max_lines(s);
    while (s->nlines > maxl) {
        memmove(s->line[0], s->line[1], (size_t)(s->nlines - 1) * DT_TEXT);
        s->nlines--;
        s->dirty = 1;
    }
}

static void queue_remove(DrawTextCUDAContext *s, int idx)
{
    if (idx < 0 || idx >= s->nq)
        return;
    if (idx + 1 < s->nq)
        memmove(&s->queue[idx], &s->queue[idx + 1],
                (size_t)(s->nq - idx - 1) * sizeof(s->queue[0]));
    s->nq--;
}

static int ingest_text(AVFilterContext *ctx, double pts, const char *text)
{
    DrawTextCUDAContext *s = ctx->priv;

    if (pts <= 0) {
        if (!text || !text[0])
            s->nlines = 0;
        else
            push_text(s, text);
        s->dirty = 1;
        return 0;
    }

    if (s->have_frame && pts > s->last_frame_sec + 0.0005) {
        av_log(ctx, AV_LOG_VERBOSE,
               "draw_text_cuda: 丢弃晚于画面的文字 pts=%.3f 画面=%.3f\n",
               pts, s->last_frame_sec);
        return 0;
    }
    if (!text || !text[0])
        return 0;
    if (s->nq >= DT_MAX_QUEUE)
        queue_remove(s, 0);
    s->queue[s->nq].pts = pts;
    av_strlcpy(s->queue[s->nq].text, text, DT_TEXT);
    s->nq++;
    return 0;
}

static void drain_queue(AVFilterContext *ctx, double frame_sec)
{
    DrawTextCUDAContext *s = ctx->priv;
    int i = 0;

    while (i < s->nq) {
        if (s->queue[i].pts > frame_sec + 0.0005) {
            av_log(ctx, AV_LOG_VERBOSE,
                   "draw_text_cuda: 丢弃晚于画面的文字 pts=%.3f 画面=%.3f\n",
                   s->queue[i].pts, frame_sec);
            queue_remove(s, i);
            continue;
        }
        push_text(s, s->queue[i].text);
        s->dirty = 1;
        queue_remove(s, i);
    }
}

static void clear_glyphs(DrawTextCUDAContext *s)
{
    for (int i = 0; i < DT_GLYPHS; i++) {
        av_freep(&s->glyphs[i].gray);
        s->glyphs[i].valid = 0;
    }
    s->nglyph = 0;
}

extern const unsigned char ff_vf_draw_text_cuda_msyh_data[];
extern const unsigned char ff_vf_draw_text_cuda_msyh_end[];

static int open_font(AVFilterContext *ctx)
{
    DrawTextCUDAContext *s = ctx->priv;
    FT_Face neu = NULL;
    FT_Error err;
    const char *opened = NULL;

    if (!s->library) {
        err = FT_Init_FreeType(&s->library);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "draw_text_cuda: FreeType 初始化失败 %d\n", (int)err);
            return AVERROR_EXTERNAL;
        }
    }

    if (s->font && s->font[0]) {
        err = FT_New_Face(s->library, s->font, s->fontindex, &neu);
        if (err) {
            av_log(ctx, AV_LOG_ERROR, "draw_text_cuda: 打不开字体 %s (%d)\n", s->font, (int)err);
            return AVERROR(EINVAL);
        }
        opened = s->font;
    } else {
        FT_Long size = (FT_Long)(ff_vf_draw_text_cuda_msyh_end -
                                 ff_vf_draw_text_cuda_msyh_data);
        err = FT_New_Memory_Face(s->library, ff_vf_draw_text_cuda_msyh_data,
                                 size, s->fontindex, &neu);
        if (err) {
            av_log(ctx, AV_LOG_ERROR,
                   "draw_text_cuda: 打不开内置微软雅黑 (%d)\n", (int)err);
            return AVERROR_EXTERNAL;
        }
        opened = "内置微软雅黑";
    }

    if (s->face)
        FT_Done_Face(s->face);
    s->face = neu;
    FT_Select_Charmap(s->face, FT_ENCODING_UNICODE);
    clear_glyphs(s);
    s->opened_size = -1;
    s->cached_bold = -1;
    s->font_dirty = 0;
    s->dirty = 1;
    av_log(ctx, AV_LOG_INFO, "draw_text_cuda: 字体 %s\n", opened);
    return 0;
}

static int kerning(DrawTextCUDAContext *s, int prev, int index)
{
    FT_Vector d;
    if (!prev || !index)
        return 0;
    if (FT_Get_Kerning(s->face, prev, index, FT_KERNING_DEFAULT, &d))
        return 0;
    return (int)(d.x >> 6);
}

static Glyph *glyph_get(DrawTextCUDAContext *s, uint32_t cp)
{
    FT_GlyphSlot slot;
    FT_Bitmap *bm;
    Glyph *g;
    int pitch, bh, bw;

    for (int i = 0; i < s->nglyph; i++)
        if (s->glyphs[i].valid && s->glyphs[i].cp == cp)
            return &s->glyphs[i];

    if (FT_Load_Char(s->face, cp, FT_LOAD_RENDER))
        return NULL;
    slot = s->face->glyph;
    if (s->bold)
        FT_GlyphSlot_Embolden(slot);

    if (s->nglyph < DT_GLYPHS) {
        g = &s->glyphs[s->nglyph++];
        memset(g, 0, sizeof(*g));
    } else {
        av_freep(&s->glyphs[0].gray);
        memmove(&s->glyphs[0], &s->glyphs[1], (DT_GLYPHS - 1) * sizeof(Glyph));
        g = &s->glyphs[DT_GLYPHS - 1];
        memset(g, 0, sizeof(*g));
    }
    bm = &slot->bitmap;
    g->cp = cp;
    g->index = (int)slot->glyph_index;
    g->advance = (int)(slot->advance.x >> 6);
    if (g->advance <= 0)
        g->advance = FFMAX(1, (int)bm->width);
    g->left = slot->bitmap_left;
    g->top = slot->bitmap_top;
    g->bw = (int)bm->width;
    g->bh = (int)bm->rows;
    g->valid = 1;
    bw = g->bw;
    bh = g->bh;
    if (bw > 0 && bh > 0 && bm->buffer &&
        (bm->pixel_mode == FT_PIXEL_MODE_GRAY || bm->pixel_mode == FT_PIXEL_MODE_MONO)) {
        g->gray = av_malloc((size_t)bw * bh);
        if (!g->gray)
            return g;
        pitch = bm->pitch;
        if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
            const uint8_t *src = bm->buffer;
            int ap = pitch;
            if (ap < 0) {
                src += ap * (bh - 1);
                ap = -ap;
            }
            for (int r = 0; r < bh; r++)
                memcpy(g->gray + r * bw, src + r * ap, bw);
        } else {
            const uint8_t *src = bm->buffer;
            int ap = pitch < 0 ? -pitch : pitch;
            if (pitch < 0)
                src += pitch * (bh - 1);
            for (int r = 0; r < bh; r++) {
                for (int c = 0; c < bw; c++) {
                    int bit = (src[r * ap + (c >> 3)] >> (7 - (c & 7))) & 1;
                    g->gray[r * bw + c] = bit ? 255 : 0;
                }
            }
        }
    }
    return g;
}

static void blend_px(uint8_t *d, int sr, int sg, int sb, int sa)
{
    int da, out_a;
    if (sa <= 0)
        return;
    if (sa >= 255) {
        d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255;
        return;
    }
    da = d[3];
    out_a = sa + da * (255 - sa) / 255;
    if (!out_a)
        return;
    d[0] = (sr * sa + d[0] * da * (255 - sa) / 255) / out_a;
    d[1] = (sg * sa + d[1] * da * (255 - sa) / 255) / out_a;
    d[2] = (sb * sa + d[2] * da * (255 - sa) / 255) / out_a;
    d[3] = out_a;
}

static void fill_rect(uint8_t *bmp, int bw, int bh, int x0, int y0, int x1, int y1,
                      int r, int g, int b, int a)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    for (int y = y0; y < y1; y++) {
        uint8_t *row = bmp + (y * bw + x0) * 4;
        for (int x = x0; x < x1; x++, row += 4)
            blend_px(row, r, g, b, a);
    }
}

typedef struct Span {
    int off, len;
} Span;

static int content_limit(const DrawTextCUDAContext *s)
{
    int c;
    if (s->w <= 0)
        return 0;
    c = s->w - 2 * s->boxpad;
    return c < 8 ? 8 : c;
}

static void commit_span(Span *spans, int *n, int max_spans, int off, int len)
{
    if (*n == max_spans) {
        memmove(spans, spans + 1, (size_t)(max_spans - 1) * sizeof(Span));
        (*n)--;
    }
    spans[*n].off = off;
    spans[*n].len = len < 0 ? 0 : len;
    (*n)++;
}

static int layout_spans(DrawTextCUDAContext *s, const char *text, Span *spans)
{
    int limit = content_limit(s);
    int len = (int)strlen(text);
    int i = 0, line_off = 0, line_px = 0, prev = 0, any = 0, n = 0;

    if (!len)
        return 0;
    while (i < len) {
        uint32_t cp;
        int nb = utf8_next((const uint8_t *)text + i, len - i, &cp);
        Glyph *g;
        int adv, kern, step;
        if (nb <= 0)
            break;
        if (cp == '\n') {
            commit_span(spans, &n, DT_SPANS, line_off, i - line_off);
            i += nb;
            line_off = i;
            line_px = 0;
            prev = 0;
            any = 0;
            continue;
        }
        g = glyph_get(s, cp);
        adv = g ? g->advance : FFMAX(1, s->fontsize / 2);
        kern = (g && any) ? kerning(s, prev, g->index) : 0;
        step = (any ? s->spacing : 0) + kern + adv;
        if (limit > 0 && any && line_px + step > limit) {
            commit_span(spans, &n, DT_SPANS, line_off, i - line_off);
            line_off = i;
            line_px = 0;
            prev = 0;
            any = 0;
            continue;
        }
        line_px += step;
        if (g)
            prev = g->index;
        any = 1;
        i += nb;
    }
    if (line_off < len)
        commit_span(spans, &n, DT_SPANS, line_off, len - line_off);
    if (n > max_lines(s)) {
        int drop = n - max_lines(s);
        memmove(spans, spans + drop, (size_t)(n - drop) * sizeof(Span));
        n -= drop;
    }
    return n;
}

static int measure_span(DrawTextCUDAContext *s, const char *text, Span sp)
{
    int i = 0, pen = 0, prev = 0, any = 0;
    while (i < sp.len) {
        uint32_t cp;
        int nb = utf8_next((const uint8_t *)text + sp.off + i, sp.len - i, &cp);
        Glyph *g;
        int adv;
        if (nb <= 0 || cp == '\n')
            break;
        g = glyph_get(s, cp);
        adv = g ? g->advance : FFMAX(1, s->fontsize / 2);
        if (any)
            pen += s->spacing;
        if (g && any)
            pen += kerning(s, prev, g->index);
        else if (g && !any)
            pen += 0;
        pen += adv;
        if (g)
            prev = g->index;
        any = 1;
        i += nb;
    }
    return pen;
}

static void draw_span(DrawTextCUDAContext *s, uint8_t *bmp, int bw, int bh,
                      const char *text, Span sp, int baseline, int font_a)
{
    int i = 0, pen = s->boxpad, prev = 0, any = 0;
    int r = s->font_rgba[0], gch = s->font_rgba[1], b = s->font_rgba[2];
    int start = pen;

    while (i < sp.len) {
        uint32_t cp;
        int nb = utf8_next((const uint8_t *)text + sp.off + i, sp.len - i, &cp);
        Glyph *glyph;
        int adv;
        if (nb <= 0 || cp == '\n')
            break;
        glyph = glyph_get(s, cp);
        adv = glyph ? glyph->advance : FFMAX(1, s->fontsize / 2);
        if (any)
            pen += s->spacing;
        if (glyph && prev)
            pen += kerning(s, prev, glyph->index);
        if (glyph && glyph->gray && glyph->bw > 0 && glyph->bh > 0) {
            int gx = pen + glyph->left;
            int gy = baseline - glyph->top;
            for (int row = 0; row < glyph->bh; row++) {
                int dy = gy + row;
                if (dy < 0 || dy >= bh)
                    continue;
                for (int col = 0; col < glyph->bw; col++) {
                    int dx = gx + col;
                    int gray, sa;
                    if (dx < 0 || dx >= bw)
                        continue;
                    gray = glyph->gray[row * glyph->bw + col];
                    sa = gray * font_a / 255;
                    blend_px(bmp + (dy * bw + dx) * 4, r, gch, b, sa);
                }
            }
        }
        pen += adv;
        if (glyph)
            prev = glyph->index;
        any = 1;
        i += nb;
    }

    if (s->underline && pen > start) {
        int uy = baseline + FFMAX(1, s->fontsize / 12);
        int uh = FFMAX(1, s->fontsize / 16);
        fill_rect(bmp, bw, bh, start, uy, pen, uy + uh, r, gch, b, font_a);
    }
}

static int render_bitmap(AVFilterContext *ctx, const char *prefix)
{
    DrawTextCUDAContext *s = ctx->priv;
    char body[DT_MAX_LINES * DT_TEXT + 64];
    Span spans[DT_SPANS];
    int nspan, body_n = 0, asc, desc, line_h, max_w = 0;
    int bw, bh, font_a, box_a, sz;
    uint8_t *bmp;

    body[0] = 0;
    if (prefix && prefix[0]) {
        body_n = snprintf(body, sizeof(body), "%s", prefix);
        if (body_n < 0 || body_n >= (int)sizeof(body))
            body_n = (int)sizeof(body) - 1;
        if (s->nlines > 0 && s->line[0][0] && body_n < (int)sizeof(body) - 1) {
            body[body_n++] = ' ';
            body[body_n] = 0;
        }
    }
    for (int i = 0; i < s->nlines && body_n < (int)sizeof(body) - 1; i++) {
        int remain = (int)sizeof(body) - body_n;
        int add;
        if (i > 0 && remain > 1) {
            body[body_n++] = '\n';
            body[body_n] = 0;
            remain--;
        }
        add = snprintf(body + body_n, remain, "%s", s->line[i]);
        if (add < 0)
            break;
        body_n += add >= remain ? remain - 1 : add;
    }

    if (!body[0]) {
        s->bmp_w = s->bmp_h = 0;
        s->dirty = 0;
        av_strlcpy(s->last_prefix, prefix ? prefix : "", sizeof(s->last_prefix));
        return 0;
    }

    sz = s->fontsize;
    if (sz < 8)
        sz = 8;
    if (sz > 512)
        sz = 512;
    if (!s->face)
        return AVERROR(EINVAL);
    if (sz != s->opened_size || s->bold != s->cached_bold) {
        if (FT_Set_Pixel_Sizes(s->face, 0, sz))
            return AVERROR_EXTERNAL;
        clear_glyphs(s);
        s->opened_size = sz;
        s->cached_bold = s->bold;
    }

    nspan = layout_spans(s, body, spans);
    if (nspan <= 0) {
        s->bmp_w = s->bmp_h = 0;
        s->dirty = 0;
        return 0;
    }

    asc = (int)(s->face->size->metrics.ascender >> 6);
    desc = (int)(-(s->face->size->metrics.descender >> 6));
    if (asc < 1)
        asc = sz;
    if (desc < 0)
        desc = 0;
    line_h = asc + desc + s->line_spacing;
    if (line_h < sz)
        line_h = sz;

    for (int i = 0; i < nspan; i++) {
        int w = measure_span(s, body, spans[i]);
        if (w > max_w)
            max_w = w;
    }
    bw = s->w > 0 ? s->w : max_w + 2 * s->boxpad;
    bh = s->h > 0 ? s->h : nspan * line_h + 2 * s->boxpad;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    if (bw > 4096) bw = 4096;
    if (bh > 2160) bh = 2160;

    if (bw * bh * 4 > s->bmp_cap) {
        uint8_t *grown = av_realloc(s->bmp, (size_t)bw * bh * 4);
        if (!grown)
            return AVERROR(ENOMEM);
        s->bmp = grown;
        s->bmp_cap = bw * bh * 4;
    }
    bmp = s->bmp;
    memset(bmp, 0, (size_t)bw * bh * 4);

    box_a = scale_alpha(s->box_rgba[3], s->boxalpha);
    if (s->box && box_a > 0)
        fill_rect(bmp, bw, bh, 0, 0, bw, bh,
                  s->box_rgba[0], s->box_rgba[1], s->box_rgba[2], box_a);

    font_a = scale_alpha(s->font_rgba[3], s->alpha);
    for (int i = 0; i < nspan; i++) {
        int baseline = s->boxpad + asc + i * line_h;
        if (baseline - asc >= bh)
            break;
        draw_span(s, bmp, bw, bh, body, spans[i], baseline, font_a);
    }

    s->bmp_w = bw;
    s->bmp_h = bh;
    s->dirty = 0;
    s->need_upload = 1;
    av_strlcpy(s->last_prefix, prefix ? prefix : "", sizeof(s->last_prefix));
    return 0;
}

static void build_body_prefix(DrawTextCUDAContext *s, int64_t pts, AVRational tb,
                              char *prefix, size_t prefix_sz)
{
    if (s->timeline)
        format_timeline(prefix, prefix_sz, pts, tb);
    else
        prefix[0] = 0;
}

static av_cold void draw_text_cuda_uninit(AVFilterContext *ctx)
{
    DrawTextCUDAContext *s = ctx->priv;

    clear_glyphs(s);
    av_freep(&s->bmp);
    s->bmp_cap = 0;
    if (s->face) {
        FT_Done_Face(s->face);
        s->face = NULL;
    }
    if (s->library) {
        FT_Done_FreeType(s->library);
        s->library = NULL;
    }

    if (s->hwctx) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        if (cu->cuCtxPushCurrent(s->hwctx->cuda_ctx) >= 0) {
            if (s->dev_bmp)
                cu->cuMemFree(s->dev_bmp);
            if (s->cu_module)
                cu->cuModuleUnload(s->cu_module);
            cu->cuCtxPopCurrent(&dummy);
        }
    }
    s->dev_bmp = 0;
    s->cu_module = NULL;
    s->hwctx = NULL;
}

static av_cold int draw_text_cuda_init(AVFilterContext *ctx)
{
    DrawTextCUDAContext *s = ctx->priv;
    char buf[DT_TEXT];
    int ret;

    s->opened_size = -1;
    s->cached_bold = -1;
    s->last_frame_sec = 0;
    s->dirty = 1;
    if (s->text && s->text[0]) {
        unescape_text(buf, sizeof(buf), s->text);
        push_text(s, buf);
    }
    ret = open_font(ctx);
    return ret;
}

static int draw_text_cuda_load(AVFilterContext *ctx)
{
    DrawTextCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    CUcontext dummy;
    int ret;

    extern const unsigned char ff_vf_draw_text_cuda_ptx_data[];
    extern const unsigned int ff_vf_draw_text_cuda_ptx_len;

    ret = CHECK_CU(cu->cuCtxPushCurrent(device_hwctx->cuda_ctx));
    if (ret < 0)
        return ret;

    ret = ff_cuda_load_module(ctx, device_hwctx, &s->cu_module,
                              ff_vf_draw_text_cuda_ptx_data, ff_vf_draw_text_cuda_ptx_len);
    if (ret < 0)
        goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_copy, s->cu_module, "dt_copy_bytes"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_y8, s->cu_module, "dt_blend_y_u8"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_y16, s->cu_module, "dt_blend_y_u16"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_uv8, s->cu_module, "dt_blend_uv_u8"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_uv16, s->cu_module, "dt_blend_uv_u16"));
    if (ret < 0) goto end;
    ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_packed, s->cu_module, "dt_blend_packed"));

end:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    return ret;
}

static int draw_text_cuda_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DrawTextCUDAContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *frames;
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "draw_text_cuda 需要 CUDA 硬件帧输入\n");
        return AVERROR(EINVAL);
    }
    frames = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    s->hwctx = frames->device_ctx->hwctx;
    s->sw_format = frames->sw_format;
    s->fmt = find_format(s->sw_format);
    if (!s->fmt) {
        av_log(ctx, AV_LOG_ERROR, "draw_text_cuda 不支持像素格式 %s\n",
               av_get_pix_fmt_name(s->sw_format));
        return AVERROR(EINVAL);
    }
    ret = draw_text_cuda_load(ctx);
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

static int launch_copy(AVFilterContext *ctx, AVFrame *out, const AVFrame *in)
{
    DrawTextCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->sw_format);
    const int nb = s->fmt->packed ? 1 : (s->fmt->semi ? 2 : 3);

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
        int ret;

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

static int upload_bitmap(AVFilterContext *ctx)
{
    DrawTextCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    int bytes = s->bmp_w * s->bmp_h * 4;
    int ret;

    if (!s->need_upload || bytes <= 0)
        return 0;
    if (bytes > s->dev_cap) {
        if (s->dev_bmp) {
            cu->cuMemFree(s->dev_bmp);
            s->dev_bmp = 0;
            s->dev_cap = 0;
        }
        ret = CHECK_CU(cu->cuMemAlloc(&s->dev_bmp, bytes));
        if (ret < 0)
            return ret;
        s->dev_cap = bytes;
    }
    ret = CHECK_CU(cu->cuMemcpyHtoD(s->dev_bmp, s->bmp, bytes));
    if (ret < 0)
        return ret;
    s->need_upload = 0;
    return 0;
}

static int blend_plane(AVFilterContext *ctx, AVFrame *frame, int plane, int comp,
                       int x0, int y0, int rw, int rh, int hsub, int vsub, int full)
{
    DrawTextCUDAContext *s = ctx->priv;
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    int pitch = frame->linesize[plane];
    int rgba_pitch = s->bmp_w * 4;
    int box_x = s->x, box_y = s->y, bw = s->bmp_w, bh = s->bmp_h;
    int depth = s->fmt->depth, shift = s->fmt->shift;
    CUdeviceptr dst = (CUdeviceptr)frame->data[plane];
    CUdeviceptr rgba = s->dev_bmp;
    CUfunction fn = depth == 8 ? s->cu_y8 : s->cu_y16;
    void *args[] = {
        &dst, &pitch, &x0, &y0, &rw, &rh, &box_x, &box_y,
        &rgba, &rgba_pitch, &bw, &bh, &hsub, &vsub, &full, &depth, &shift, &comp
    };
    if (rw <= 0 || rh <= 0)
        return 0;
    return CHECK_CU(cu->cuLaunchKernel(fn, DIV_UP(rw, BLOCK_X), DIV_UP(rh, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args, NULL));
}

static int blend_frame(AVFilterContext *ctx, AVFrame *frame)
{
    DrawTextCUDAContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->sw_format);
    const int full = frame->color_range == AVCOL_RANGE_JPEG;
    const int luma_x0 = FFMAX(s->x, 0);
    const int luma_y0 = FFMAX(s->y, 0);
    const int luma_x1 = FFMIN(s->x + s->bmp_w, frame->width);
    const int luma_y1 = FFMIN(s->y + s->bmp_h, frame->height);
    int ret;

    ret = upload_bitmap(ctx);
    if (ret < 0)
        return ret;
    if (luma_x1 <= luma_x0 || luma_y1 <= luma_y0)
        return 0;

    if (s->fmt->packed) {
        AVCUDADeviceContext *device_hwctx = s->hwctx;
        CudaFunctions *cu = device_hwctx->internal->cuda_dl;
        int order = s->fmt->packed == PACK_RGBA ? 0 :
                    s->fmt->packed == PACK_BGRA ? 1 :
                    s->fmt->packed == PACK_RGB0 ? 2 : 3;
        int pitch = frame->linesize[0];
        int rgba_pitch = s->bmp_w * 4;
        int box_x = s->x, box_y = s->y, bw = s->bmp_w, bh = s->bmp_h;
        int x0 = luma_x0, y0 = luma_y0;
        int rw = luma_x1 - luma_x0, rh = luma_y1 - luma_y0;
        CUdeviceptr dst = (CUdeviceptr)frame->data[0];
        CUdeviceptr rgba = s->dev_bmp;
        void *args[] = {
            &dst, &pitch, &x0, &y0, &rw, &rh, &box_x, &box_y,
            &rgba, &rgba_pitch, &bw, &bh, &order
        };
        return CHECK_CU(cu->cuLaunchKernel(s->cu_packed,
                                           DIV_UP(rw, BLOCK_X), DIV_UP(rh, BLOCK_Y), 1,
                                           BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args, NULL));
    }

    ret = blend_plane(ctx, frame, 0, 0, luma_x0, luma_y0,
                      luma_x1 - luma_x0, luma_y1 - luma_y0, 0, 0, full);
    if (ret < 0)
        return ret;

    if (s->fmt->semi) {
        AVCUDADeviceContext *device_hwctx = s->hwctx;
        CudaFunctions *cu = device_hwctx->internal->cuda_dl;
        const int hsub = desc->log2_chroma_w;
        const int vsub = desc->log2_chroma_h;
        int x0 = luma_x0 >> hsub;
        int y0 = luma_y0 >> vsub;
        int x1 = AV_CEIL_RSHIFT(luma_x1, hsub);
        int y1 = AV_CEIL_RSHIFT(luma_y1, vsub);
        int rw = x1 - x0, rh = y1 - y0;
        int pitch = frame->linesize[1];
        int rgba_pitch = s->bmp_w * 4;
        int box_x = s->x, box_y = s->y, bw = s->bmp_w, bh = s->bmp_h;
        int depth = s->fmt->depth, shift = s->fmt->shift;
        CUdeviceptr dst = (CUdeviceptr)frame->data[1];
        CUdeviceptr rgba = s->dev_bmp;
        CUfunction fn = depth == 8 ? s->cu_uv8 : s->cu_uv16;
        void *args[] = {
            &dst, &pitch, &x0, &y0, &rw, &rh, &box_x, &box_y,
            &rgba, &rgba_pitch, &bw, &bh, &hsub, &vsub, &full, &depth, &shift
        };
        if (rw > 0 && rh > 0)
            return CHECK_CU(cu->cuLaunchKernel(fn, DIV_UP(rw, BLOCK_X), DIV_UP(rh, BLOCK_Y), 1,
                                               BLOCK_X, BLOCK_Y, 1, 0, s->hwctx->stream, args, NULL));
        return 0;
    }

    {
        const int hsub = desc->log2_chroma_w;
        const int vsub = desc->log2_chroma_h;
        int x0 = luma_x0 >> hsub;
        int y0 = luma_y0 >> vsub;
        int rw = AV_CEIL_RSHIFT(luma_x1, hsub) - x0;
        int rh = AV_CEIL_RSHIFT(luma_y1, vsub) - y0;
        ret = blend_plane(ctx, frame, 1, 1, x0, y0, rw, rh, hsub, vsub, full);
        if (ret < 0)
            return ret;
        return blend_plane(ctx, frame, 2, 2, x0, y0, rw, rh, hsub, vsub, full);
    }
}

static int draw_text_cuda_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    DrawTextCUDAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *outl = ff_filter_link(outlink);
    AVCUDADeviceContext *device_hwctx = s->hwctx;
    double frame_sec = 0;
    char prefix[24];
    AVFrame *out = NULL;
    CUcontext dummy;
    int ret, direct, overlap;

    if (in->pts != AV_NOPTS_VALUE && inlink->time_base.den)
        frame_sec = av_q2d(inlink->time_base) * (double)in->pts;
    s->last_frame_sec = frame_sec;
    s->have_frame = 1;
    trim_lines(s);
    drain_queue(ctx, frame_sec);

    if (s->font_dirty) {
        ret = open_font(ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "draw_text_cuda: 更换字体失败，沿用上一套\n");
            s->font_dirty = 0;
        }
    }

    build_body_prefix(s, in->pts, inlink->time_base, prefix, sizeof(prefix));
    if (s->dirty || strcmp(prefix, s->last_prefix)) {
        if (s->face) {
            ret = render_bitmap(ctx, prefix);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "draw_text_cuda: 渲染文字失败\n");
                return ff_filter_frame(outlink, in);
            }
        }
    }

    overlap = s->bmp_w > 0 && s->bmp_h > 0 &&
              s->x < in->width && s->y < in->height &&
              s->x + s->bmp_w > 0 && s->y + s->bmp_h > 0;
    if (!overlap || !s->bmp)
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
    ret = blend_frame(ctx, out);
pop:
    CHECK_CU(device_hwctx->internal->cuda_dl->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;
    if (!direct)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    if (out && out != in)
        av_frame_free(&out);
    av_frame_free(&in);
    return ret < 0 ? ret : AVERROR_EXTERNAL;
}

static int draw_text_cuda_process_command(AVFilterContext *ctx, const char *cmd,
                                          const char *args, char *res, int res_len,
                                          int flags)
{
    DrawTextCUDAContext *s = ctx->priv;
    const char *mapped = cmd;
    int ret;

    if (!strcmp(cmd, "text")) {
        const char *body = args ? args : "";
        double pts = 0;
        char buf[DT_TEXT];
        while (*body == ' ')
            body++;
        if (!strncmp(body, "pts=", 4)) {
            char *end = NULL;
            pts = strtod(body + 4, &end);
            if (end == body + 4)
                return AVERROR(EINVAL);
            if (*end == ':' || *end == ' ')
                body = end + 1;
            else
                body = end;
            while (*body == ' ')
                body++;
        }
        unescape_text(buf, sizeof(buf), body);
        return ingest_text(ctx, pts, buf);
    }
    if (!strcmp(cmd, "clear")) {
        s->nlines = 0;
        s->nq = 0;
        s->dirty = 1;
        return 0;
    }
    if (!strcmp(cmd, "reload")) {
        s->font_dirty = 1;
        s->dirty = 1;
        return 0;
    }
    if (!strcmp(cmd, "fontfile"))
        mapped = "font";
    else if (!strcmp(cmd, "interval"))
        mapped = "spacing";
    else if (!strcmp(cmd, "color"))
        mapped = "fontcolor";

    ret = ff_filter_process_command(ctx, mapped, args, res, res_len, flags);
    if (ret < 0)
        return ret;
    if (!strcmp(mapped, "font") || !strcmp(mapped, "fontindex"))
        s->font_dirty = 1;
    if (strcmp(mapped, "x") && strcmp(mapped, "y"))
        s->dirty = 1;
    return 0;
}

#define OFFSET(x) offsetof(DrawTextCUDAContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define FLAGSR (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption draw_text_cuda_options[] = {
    { "text",         "initial text",                OFFSET(text),         AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "font",         "font file path",              OFFSET(font),         AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGSR },
    { "fontindex",    "face index inside a collection", OFFSET(fontindex), AV_OPT_TYPE_INT,    { .i64 = 0 }, 0, 32, FLAGSR },
    { "fontsize",     "font pixel size",             OFFSET(fontsize),     AV_OPT_TYPE_INT,    { .i64 = 36 }, 8, 512, FLAGSR },
    { "spacing",      "extra pixels between glyphs", OFFSET(spacing),      AV_OPT_TYPE_INT,    { .i64 = 0 }, -32, 200, FLAGSR },
    { "interval",     "extra pixels between glyphs", OFFSET(spacing),      AV_OPT_TYPE_INT,    { .i64 = 0 }, -32, 200, FLAGSR },
    { "line_spacing", "extra pixels between lines",  OFFSET(line_spacing), AV_OPT_TYPE_INT,    { .i64 = 4 }, -32, 200, FLAGSR },
    { "bold",         "synthetic bold",              OFFSET(bold),         AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, FLAGSR },
    { "underline",    "underline",                   OFFSET(underline),    AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, FLAGSR },
    { "fontcolor",    "text color",                  OFFSET(font_rgba),    AV_OPT_TYPE_COLOR,  { .str = "white" }, 0, 0, FLAGSR },
    { "color",        "text color",                  OFFSET(font_rgba),    AV_OPT_TYPE_COLOR,  { .str = "white" }, 0, 0, FLAGSR },
    { "alpha",        "text opacity",                OFFSET(alpha),        AV_OPT_TYPE_DOUBLE, { .dbl = 1 }, 0, 1, FLAGSR },
    { "box",          "draw background box",         OFFSET(box),          AV_OPT_TYPE_BOOL,   { .i64 = 1 }, 0, 1, FLAGSR },
    { "boxcolor",     "box color",                   OFFSET(box_rgba),     AV_OPT_TYPE_COLOR,  { .str = "black@0.6" }, 0, 0, FLAGSR },
    { "boxalpha",     "box opacity multiplier",      OFFSET(boxalpha),     AV_OPT_TYPE_DOUBLE, { .dbl = 1 }, 0, 1, FLAGSR },
    { "x",            "box left",                    OFFSET(x),            AV_OPT_TYPE_INT,    { .i64 = 24 }, INT_MIN, INT_MAX, FLAGSR },
    { "y",            "box top",                     OFFSET(y),            AV_OPT_TYPE_INT,    { .i64 = 24 }, INT_MIN, INT_MAX, FLAGSR },
    { "w",            "box width, 0=auto",           OFFSET(w),            AV_OPT_TYPE_INT,    { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "h",            "box height, 0=auto",          OFFSET(h),            AV_OPT_TYPE_INT,    { .i64 = 0 }, 0, INT_MAX, FLAGSR },
    { "boxpad",       "padding inside the box",      OFFSET(boxpad),       AV_OPT_TYPE_INT,    { .i64 = 8 }, 0, 128, FLAGSR },
    { "lines",        "max visible lines",           OFFSET(lines),        AV_OPT_TYPE_INT,    { .i64 = 1 }, 1, DT_MAX_LINES, FLAGSR },
    { "timeline",     "prepend frame timestamp",     OFFSET(timeline),     AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, FLAGSR },
    { NULL }
};

AVFILTER_DEFINE_CLASS(draw_text_cuda);

static const AVFilterPad inputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .filter_frame = draw_text_cuda_filter_frame,
}};

static const AVFilterPad outputs[] = {{
    .name         = "default",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = draw_text_cuda_config_props,
}};

const FFFilter ff_vf_draw_text_cuda = {
    .p.name         = "draw_text_cuda",
    .p.description  = NULL_IF_CONFIG_SMALL("Draw text on CUDA frames."),
    .p.priv_class   = &draw_text_cuda_class,
    .p.flags        = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size      = sizeof(DrawTextCUDAContext),
    .init           = draw_text_cuda_init,
    .uninit         = draw_text_cuda_uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
    .process_command = draw_text_cuda_process_command,
};
