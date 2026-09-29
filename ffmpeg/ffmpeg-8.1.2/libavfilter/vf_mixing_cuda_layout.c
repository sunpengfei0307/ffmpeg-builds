/*
 * 16:9 preset layouts. Numbers follow the director console
 * layoutGeometry() in landscape mode (portrait presets are not included).
 *
 * gap is a 1080p pixel inset converted with gx = gap/1920*100, gy = gap/1080*100.
 * Default gaps: single 0, split2/split2h/grid4 16, pip/pip_tl/left3/grid6 12,
 * speaker 8, grid3 14, grid9 10, grid16 8.
 */

#include "vf_mixing_cuda_layout.h"

#include <math.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/common.h"

#define REF_W 1920.0
#define REF_H 1080.0

static void gap_pct(int gap_px, double *gx, double *gy)
{
    double px = gap_px > 0 ? (double)gap_px : 0.0;
    *gx = px / REF_W * 100.0;
    *gy = px / REF_H * 100.0;
}

static MixingLayoutRect box(double x, double y, double w, double h,
                            double gx, double gy)
{
    MixingLayoutRect r;
    r.x = (float)(x + gx / 2.0);
    r.y = (float)(y + gy / 2.0);
    r.w = (float)fmax(0.5, w - gx);
    r.h = (float)fmax(0.5, h - gy);
    return r;
}

static int fill_grid(int count, int cols, int gap_px,
                     MixingLayoutRect *out, int cap)
{
    double gx, gy;
    int rows, i, n;

    if (!out || count < 1 || cols < 1 || cap < 1)
        return 0;
    n = FFMIN(count, cap);
    rows = (n + cols - 1) / cols;
    gap_pct(gap_px, &gx, &gy);
    for (i = 0; i < n; i++) {
        int c = i % cols;
        int r = i / cols;
        out[i] = box((c * 100.0) / cols, (r * 100.0) / rows,
                     100.0 / cols, 100.0 / rows, gx, gy);
    }
    return n;
}

const char *ff_mixing_layout_canon(const char *name)
{
    if (!name || !*name)
        return NULL;
    while (*name == ' ' || *name == '\t')
        name++;
    if (!av_strcasecmp(name, "single") || !strcmp(name, "1"))
        return "single";
    if (!av_strcasecmp(name, "split2") || !strcmp(name, "2") ||
        !av_strcasecmp(name, "hsplit"))
        return "split2";
    if (!av_strcasecmp(name, "split2h") || !av_strcasecmp(name, "vsplit") ||
        !av_strcasecmp(name, "triple"))
        return "split2h";
    if (!av_strcasecmp(name, "pip"))
        return "pip";
    if (!av_strcasecmp(name, "pip_tl"))
        return "pip_tl";
    if (!av_strcasecmp(name, "speaker") || !av_strcasecmp(name, "2v1"))
        return "speaker";
    if (!av_strcasecmp(name, "left3"))
        return "left3";
    if (!av_strcasecmp(name, "grid3"))
        return "grid3";
    if (!av_strcasecmp(name, "grid4") || !strcmp(name, "4") ||
        !av_strcasecmp(name, "grid") || !av_strcasecmp(name, "quad"))
        return "grid4";
    if (!av_strcasecmp(name, "grid6"))
        return "grid6";
    if (!av_strcasecmp(name, "grid9") || !strcmp(name, "9"))
        return "grid9";
    if (!av_strcasecmp(name, "grid16"))
        return "grid16";
    if (!av_strcasecmp(name, "grid4col"))
        return "grid4col";
    return NULL;
}

/* Speaker: main 1432x806 @1080p, three 16:9 thumbs sharing that height, gap 8. */
static int layout_speaker(MixingLayoutRect *out, int cap)
{
    const double gpx = 8.0;
    double main_w, main_h, gap_x, gap_y, thumb_h, thumb_w;
    double block_w, ox, oy, side_x;

    if (!out || cap < 4)
        return 0;
    main_w = 1432.0 / REF_W * 100.0;
    main_h = 806.0 / REF_H * 100.0;
    gap_x = gpx / REF_W * 100.0;
    gap_y = gpx / REF_H * 100.0;
    thumb_h = fmax(0.5, (main_h - 2.0 * gap_y) / 3.0);
    thumb_w = thumb_h;
    block_w = main_w + gap_x + thumb_w;
    ox = (100.0 - block_w) / 2.0;
    oy = (100.0 - main_h) / 2.0;
    side_x = ox + main_w + gap_x;
    out[0].x = (float)ox;
    out[0].y = (float)oy;
    out[0].w = (float)main_w;
    out[0].h = (float)main_h;
    out[1].x = (float)side_x;
    out[1].y = (float)oy;
    out[1].w = (float)thumb_w;
    out[1].h = (float)thumb_h;
    out[2].x = (float)side_x;
    out[2].y = (float)(oy + thumb_h + gap_y);
    out[2].w = (float)thumb_w;
    out[2].h = (float)thumb_h;
    out[3].x = (float)side_x;
    out[3].y = (float)(oy + 2.0 * (thumb_h + gap_y));
    out[3].w = (float)thumb_w;
    out[3].h = (float)thumb_h;
    return 4;
}

static int layout_pip(MixingLayoutRect *out, int cap, int top_left)
{
    double gx, gy;

    if (!out || cap < 2)
        return 0;
    gap_pct(12, &gx, &gy);
    out[0].x = 0.f;
    out[0].y = 0.f;
    out[0].w = 100.f;
    out[0].h = 100.f;
    if (top_left)
        out[1] = box(2.0, 2.0, 28.0, 28.0, gx, gy);
    else
        out[1] = box(70.0, 68.0, 28.0, 28.0, gx, gy);
    return 2;
}

int ff_mixing_layout_by_name(const char *name, MixingLayoutRect *out, int cap)
{
    const char *id = ff_mixing_layout_canon(name);
    double gx, gy;

    if (!id || !out || cap < 1)
        return 0;
    if (!strcmp(id, "single")) {
        out[0].x = 0.f;
        out[0].y = 0.f;
        out[0].w = 100.f;
        out[0].h = 100.f;
        return 1;
    }
    if (!strcmp(id, "split2"))
        return fill_grid(2, 2, 16, out, cap);
    if (!strcmp(id, "split2h"))
        return fill_grid(2, 1, 16, out, cap);
    if (!strcmp(id, "pip"))
        return layout_pip(out, cap, 0);
    if (!strcmp(id, "pip_tl"))
        return layout_pip(out, cap, 1);
    if (!strcmp(id, "speaker"))
        return layout_speaker(out, cap);
    if (!strcmp(id, "left3")) {
        if (cap < 4)
            return 0;
        gap_pct(12, &gx, &gy);
        out[0] = box(0.0, 0.0, 70.0, 100.0, gx, gy);
        out[1] = box(70.0, 0.0, 30.0, 33.33, gx, gy);
        out[2] = box(70.0, 33.33, 30.0, 33.33, gx, gy);
        out[3] = box(70.0, 66.66, 30.0, 33.34, gx, gy);
        return 4;
    }
    if (!strcmp(id, "grid3")) {
        if (cap < 3)
            return 0;
        gap_pct(14, &gx, &gy);
        out[0] = box(0.0, 0.0, 100.0, 58.0, gx, gy);
        out[1] = box(0.0, 58.0, 50.0, 42.0, gx, gy);
        out[2] = box(50.0, 58.0, 50.0, 42.0, gx, gy);
        return 3;
    }
    if (!strcmp(id, "grid4"))
        return fill_grid(4, 2, 16, out, cap);
    if (!strcmp(id, "grid6"))
        return fill_grid(6, 3, 12, out, cap);
    if (!strcmp(id, "grid9"))
        return fill_grid(9, 3, 10, out, cap);
    if (!strcmp(id, "grid16"))
        return fill_grid(16, 4, 8, out, cap);
    if (!strcmp(id, "grid4col"))
        return 0;
    return 0;
}

const char *ff_mixing_layout_ceil_name(int n)
{
    if (n <= 1)
        return "single";
    if (n == 2)
        return "split2";
    if (n == 3)
        return "grid3";
    if (n == 4)
        return "grid4";
    if (n <= 6)
        return "grid6";
    if (n <= 9)
        return "grid9";
    if (n <= 16)
        return "grid16";
    return "grid4col";
}

int ff_mixing_layout_for_count(int n, MixingLayoutRect *out, int cap)
{
    const char *id;
    int nr;

    if (!out || cap < 1)
        return 0;
    if (n < 1)
        n = 1;
    if (n > MIXING_LAYOUT_MAX)
        n = MIXING_LAYOUT_MAX;
    id = ff_mixing_layout_ceil_name(n);
    if (!strcmp(id, "grid4col"))
        return fill_grid(n, 4, 8, out, cap);
    nr = ff_mixing_layout_by_name(id, out, cap);
    return nr;
}
