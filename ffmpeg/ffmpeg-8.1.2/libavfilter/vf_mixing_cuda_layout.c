/*
 * mixing_cuda built-in layouts: equal gallery + speaker spotlight.
 *
 * Speaker obs (1080p ref): main 1432x806, thumb 468x256, gap 16px;
 * right column top/bottom aligned with main. Other canvases scale by
 * out_w/1920 and out_h/1080.
 */

#include "vf_mixing_cuda_layout.h"

#include <math.h>

#include "libavutil/common.h"

#define REF_W         1920
#define REF_H         1080
#define REF_MAIN_W    1432
#define REF_MAIN_H    806
#define REF_THUMB_W   468
#define REF_THUMB_H   256
#define REF_GAP       16

/* Equal gallery: outer pad + cell gap (px on 1080p, then scaled). */
#define LAY_PAD       8
#define LAY_GAP       16

static void canvas_size(int *w, int *h)
{
    if (*w <= 0)
        *w = REF_W;
    if (*h <= 0)
        *h = REF_H;
}

static int sx(int ref, int out_w)
{
    return FFMAX(2, (ref * out_w + REF_W / 2) / REF_W);
}

static int sy(int ref, int out_h)
{
    return FFMAX(2, (ref * out_h + REF_H / 2) / REF_H);
}

static float pct_x(int px, int out_w)
{
    return 100.f * (float)px / (float)out_w;
}

static float pct_y(int px, int out_h)
{
    return 100.f * (float)px / (float)out_h;
}

static MixingLayoutRect rect_px(int x, int y, int w, int h, int out_w, int out_h)
{
    MixingLayoutRect r;
    r.x = pct_x(x, out_w);
    r.y = pct_y(y, out_h);
    r.w = pct_x(w, out_w);
    r.h = pct_y(h, out_h);
    return r;
}

/* Optimal cols×rows so cols/rows is closest to 16:9 (HTML calcGrid). */
static void calc_grid(int n, int *cols, int *rows)
{
    int c, best_c = 1, best_r = 1;
    float best_diff = 1e9f;

    if (n <= 1) {
        *cols = 1;
        *rows = 1;
        return;
    }
    for (c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        float ratio = (float)c / (float)r;
        float diff = fabsf(ratio - 16.f / 9.f);
        if (diff < best_diff) {
            best_diff = diff;
            best_c = c;
            best_r = r;
        }
    }
    *cols = best_c;
    *rows = best_r;
}

int ff_mixing_layout_equal(int n, int out_w, int out_h, MixingLayoutRect *out)
{
    int cols, rows, i;
    int pad, gap, content_w, content_h, cell_w, cell_h;

    if (!out || n < 1 || n > MIXING_LAYOUT_MAX)
        return 0;

    canvas_size(&out_w, &out_h);

    pad = sx(LAY_PAD, out_w);
    gap = sx(LAY_GAP, out_w);
    /* keep vertical gap consistent with horizontal on non-16:9 canvases */
    if (sy(LAY_GAP, out_h) != gap)
        gap = FFMIN(gap, sy(LAY_GAP, out_h));

    content_w = out_w - 2 * pad;
    content_h = out_h - 2 * pad;
    if (content_w < 2 || content_h < 2)
        return 0;

    calc_grid(n, &cols, &rows);
    cell_w = (content_w - (cols - 1) * gap) / cols;
    cell_h = (content_h - (rows - 1) * gap) / rows;
    if (cell_w < 2 || cell_h < 2)
        return 0;

    for (i = 0; i < n; i++) {
        int c = i % cols;
        int r = i / cols;
        int x = pad + c * (cell_w + gap);
        int y = pad + r * (cell_h + gap);
        out[i] = rect_px(x, y, cell_w, cell_h, out_w, out_h);
    }
    return n;
}

/* Max side thumbs (1 main + 3 side); more → bottom filmstrip. */
#define SPEAKER_SIDE_MAX_THUMBS 3

/*
 * Side strip: main 1432x806 + gap 16 + up to 3 thumbs at fixed 468x256
 * (aspect preserved). Column top-aligned with main; 3 thumbs ≈ bottom-align
 * (3*256+2*16=800 vs main 806).
 */
static int speaker_side(int n_alive, int out_w, int out_h, MixingLayoutRect *out)
{
    int n_thumbs = n_alive - 1;
    int main_w = sx(REF_MAIN_W, out_w);
    int main_h = sy(REF_MAIN_H, out_h);
    int thumb_w = sx(REF_THUMB_W, out_w);
    int thumb_h = sy(REF_THUMB_H, out_h);
    int gap_x = sx(REF_GAP, out_w);
    int gap_y = sy(REF_GAP, out_h);
    int block_w, ox, oy, stack_h, ty, i;

    if (n_thumbs > SPEAKER_SIDE_MAX_THUMBS)
        n_thumbs = SPEAKER_SIDE_MAX_THUMBS;

    if (main_w + gap_x + thumb_w > out_w) {
        int over = main_w + gap_x + thumb_w - out_w;
        main_w = FFMAX(2, main_w - over);
    }
    if (main_h > out_h)
        main_h = out_h;

    stack_h = n_thumbs > 0
              ? n_thumbs * thumb_h + (n_thumbs - 1) * gap_y
              : 0;
    /* Keep main tall enough to cover the side stack (top/bottom align). */
    if (stack_h > main_h)
        main_h = FFMIN(out_h, stack_h);

    block_w = main_w + gap_x + thumb_w;
    ox = (out_w - block_w) / 2;
    oy = (out_h - main_h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    out[0] = rect_px(ox, oy, main_w, main_h, out_w, out_h);

    if (n_thumbs <= 0)
        return 1;

    /* Top-align with main; with 3 thumbs stack≈main → bottom nearly flush. */
    ty = oy;
    for (i = 0; i < n_thumbs; i++) {
        out[i + 1] = rect_px(ox + main_w + gap_x, ty, thumb_w, thumb_h,
                             out_w, out_h);
        ty += thumb_h + gap_y;
    }
    return 1 + n_thumbs;
}

/*
 * Bottom filmstrip: main on top; thumbs keep 468:256 aspect (uniform scale
 * down if the row would exceed canvas width). gap 16.
 */
static int speaker_bottom(int n_alive, int out_w, int out_h, MixingLayoutRect *out)
{
    int n_thumbs = n_alive - 1;
    int main_w = sx(REF_MAIN_W, out_w);
    int main_h = sy(REF_MAIN_H, out_h);
    int thumb_w = sx(REF_THUMB_W, out_w);
    int thumb_h = sy(REF_THUMB_H, out_h);
    int gap_x = sx(REF_GAP, out_w);
    int gap_y = sy(REF_GAP, out_h);
    int block_h, row_w, ox, oy, tx, i;

    if (n_thumbs <= 0) {
        ox = (out_w - main_w) / 2;
        oy = (out_h - main_h) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        out[0] = rect_px(ox, oy, main_w, main_h, out_w, out_h);
        return 1;
    }

    row_w = n_thumbs * thumb_w + (n_thumbs - 1) * gap_x;
    if (row_w > out_w) {
        /* Uniform scale to fit width; preserve 468:256. */
        int avail = out_w - (n_thumbs - 1) * gap_x;
        thumb_w = FFMAX(2, avail / n_thumbs);
        thumb_h = FFMAX(2, thumb_w * REF_THUMB_H / REF_THUMB_W);
        row_w = n_thumbs * thumb_w + (n_thumbs - 1) * gap_x;
    }

    main_w = FFMAX(main_w, row_w);
    if (main_w > out_w)
        main_w = out_w;

    block_h = main_h + gap_y + thumb_h;
    if (block_h > out_h) {
        int over = block_h - out_h;
        main_h = FFMAX(2, main_h - over);
        block_h = main_h + gap_y + thumb_h;
        if (block_h > out_h) {
            thumb_h = FFMAX(2, out_h - main_h - gap_y);
            thumb_w = FFMAX(2, thumb_h * REF_THUMB_W / REF_THUMB_H);
            row_w = n_thumbs * thumb_w + (n_thumbs - 1) * gap_x;
            if (row_w > out_w) {
                int avail = out_w - (n_thumbs - 1) * gap_x;
                thumb_w = FFMAX(2, avail / n_thumbs);
                thumb_h = FFMAX(2, thumb_w * REF_THUMB_H / REF_THUMB_W);
                row_w = n_thumbs * thumb_w + (n_thumbs - 1) * gap_x;
            }
            block_h = main_h + gap_y + thumb_h;
        }
    }

    ox = (out_w - main_w) / 2;
    oy = (out_h - block_h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    out[0] = rect_px(ox, oy, main_w, main_h, out_w, out_h);

    tx = ox + (main_w - row_w) / 2;
    for (i = 0; i < n_thumbs; i++) {
        out[i + 1] = rect_px(tx, oy + main_h + gap_y, thumb_w, thumb_h,
                             out_w, out_h);
        tx += thumb_w + gap_x;
    }
    return n_alive;
}

int ff_mixing_layout_speaker(int n_alive, int style, int out_w, int out_h,
                             MixingLayoutRect *out)
{
    int use_bottom;

    if (!out || n_alive < 1 || n_alive > MIXING_LAYOUT_MAX)
        return 0;

    canvas_size(&out_w, &out_h);

    if (n_alive == 1) {
        int main_w = sx(REF_MAIN_W, out_w);
        int main_h = sy(REF_MAIN_H, out_h);
        int ox = (out_w - main_w) / 2;
        int oy = (out_h - main_h) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        out[0] = rect_px(ox, oy, main_w, main_h, out_w, out_h);
        return 1;
    }

    /* obs: side only with ≤3 thumbs (n≤4); more → bottom. grid: always bottom. */
    use_bottom = (style != 0) || (n_alive > 1 + SPEAKER_SIDE_MAX_THUMBS);
    if (use_bottom)
        return speaker_bottom(n_alive, out_w, out_h, out);
    return speaker_side(n_alive, out_w, out_h, out);
}
