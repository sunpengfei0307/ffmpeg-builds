/*
 * Built-in equal (gallery) + speaker (spotlight) layouts for mixing_cuda.
 * Percentages are 0..100 relative to the output canvas.
 * Pixel constants (PAD/GAP/SIDE/BOTTOM) are converted using out_w/out_h.
 */
#ifndef AVFILTER_VF_MIXING_CUDA_LAYOUT_H
#define AVFILTER_VF_MIXING_CUDA_LAYOUT_H

#include <stddef.h>

#define MIXING_LAYOUT_MAX 32

typedef struct MixingLayoutRect {
    float x, y, w, h;
} MixingLayoutRect;

/**
 * Equal / gallery tiling (16:9-optimal cols×rows + PAD/GAP).
 * Used by adaptive and fixed JSON fallback.
 * @param n       number of tiles (1..MIXING_LAYOUT_MAX)
 * @param out_w   canvas width in px (≤0 → 1920)
 * @param out_h   canvas height in px (≤0 → 1080)
 * @param out     caller buffer of size >= n
 * @return number of rects written, or 0 if n invalid
 */
int ff_mixing_layout_equal(int n, int out_w, int out_h, MixingLayoutRect *out);

/**
 * Speaker spotlight (1080p ref: main 1432x806, thumb 468x256, gap 16).
 * @param n_alive  total alive including speaker (1..MIXING_LAYOUT_MAX)
 * @param style    0=obs (side if ≤3 thumbs / n≤4, else bottom),
 *                 1=grid (always bottom filmstrip)
 * @param out_w    canvas width in px (≤0 → 1920)
 * @param out_h    canvas height in px (≤0 → 1080)
 * @param out      rects[0] is always the main/speaker pane; then thumbnails
 * @return number of rects written
 */
int ff_mixing_layout_speaker(int n_alive, int style, int out_w, int out_h,
                             MixingLayoutRect *out);

#endif /* AVFILTER_VF_MIXING_CUDA_LAYOUT_H */
