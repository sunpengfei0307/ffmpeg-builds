/*
 * 16:9 preset layouts for mixing_cuda.
 * Geometry matches the director console landscape layoutGeometry()
 * (gap in 1080p px, then inset as a percent of 1920x1080).
 * Percentages are 0..100 of the output canvas.
 */
#ifndef AVFILTER_VF_MIXING_CUDA_LAYOUT_H
#define AVFILTER_VF_MIXING_CUDA_LAYOUT_H

#define MIXING_LAYOUT_MAX 32

typedef struct MixingLayoutRect {
    float x, y, w, h;
} MixingLayoutRect;

/**
 * Canonical preset id, or NULL if name is unknown.
 * Accepts console ids and legacy aliases (1/2/4/9, grid, quad, hsplit, vsplit, 2v1).
 */
const char *ff_mixing_layout_canon(const char *name);

/**
 * Landscape 16:9 preset. Writes one rect per slot of that preset.
 * @return slot count, or 0 if name is unknown / out is too small
 */
int ff_mixing_layout_by_name(const char *name, MixingLayoutRect *out, int cap);

/**
 * Smallest default preset whose slot count is >= n (round up):
 * 1 single, 2 split2, 3 grid3, 4 grid4, 5–6 grid6, 7–9 grid9, 10–16 grid16.
 * n>16 is a 4-column extension of grid16 (gap 8); the name is "grid4col".
 */
const char *ff_mixing_layout_ceil_name(int n);

/**
 * Full geometry of the ceiling preset for n inputs.
 * A count of 5 returns all 6 cells of grid6; the caller fills the first n.
 * @return number of rects, or 0 if n/out invalid
 */
int ff_mixing_layout_for_count(int n, MixingLayoutRect *out, int cap);

#endif /* AVFILTER_VF_MIXING_CUDA_LAYOUT_H */
