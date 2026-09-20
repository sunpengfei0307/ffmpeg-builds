/*
 * Shared types for clock_cuda host + kernels.
 */
#ifndef AVFILTER_VSRC_CLOCK_CUDA_H
#define AVFILTER_VSRC_CLOCK_CUDA_H

enum { CLOCK_MAX_BLITS = 48 };
enum { CLOCK_MAX_CARDS = 16 };

typedef struct ClockBlit {
    float ax, ay, aw, ah;
    float dx, dy, dw, dh;
} ClockBlit;

typedef struct ClockCard {
    float dx, dy, dw, dh, radius;
    float y_top, y_bot;
} ClockCard;

#endif /* AVFILTER_VSRC_CLOCK_CUDA_H */
