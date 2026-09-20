/*
 * CUDA kernels for clock_cuda video source.
 */

#include "vsrc_clock_cuda.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

__device__ float atlas_sample(const unsigned char *atlas, int aw, int ah,
                              float x, float y)
{
    int x0, y0, x1, y1;
    float fx, fy, a00, a10, a01, a11, a0, a1;

    if (aw <= 1 || ah <= 1)
        return 0.f;
    x = fminf(fmaxf(x, 0.f), (float)(aw - 1) - 1e-3f);
    y = fminf(fmaxf(y, 0.f), (float)(ah - 1) - 1e-3f);
    x0 = (int)floorf(x);
    y0 = (int)floorf(y);
    x1 = x0 + 1 < aw ? x0 + 1 : aw - 1;
    y1 = y0 + 1 < ah ? y0 + 1 : ah - 1;
    fx = x - (float)x0;
    fy = y - (float)y0;
    a00 = atlas[y0 * aw + x0];
    a10 = atlas[y0 * aw + x1];
    a01 = atlas[y1 * aw + x0];
    a11 = atlas[y1 * aw + x1];
    a0 = a00 + (a10 - a00) * fx;
    a1 = a01 + (a11 - a01) * fx;
    return (a0 + (a1 - a0) * fy) * (1.f / 255.f);
}

__device__ float blit_alpha(const unsigned char *atlas, int aw, int ah,
                            const ClockBlit *blits, int nblits,
                            float px, float py)
{
    float a = 0.f;
    int i;

    for (i = 0; i < nblits; i++) {
        ClockBlit b = blits[i];
        float u, v;
        if (px < b.dx || py < b.dy || px >= b.dx + b.dw || py >= b.dy + b.dh)
            continue;
        if (b.dw < 1.f || b.dh < 1.f)
            continue;
        u = (px + 0.5f - b.dx) / b.dw;
        v = (py + 0.5f - b.dy) / b.dh;
        a = fmaxf(a, atlas_sample(atlas, aw, ah,
                                  b.ax + u * b.aw, b.ay + v * b.ah));
    }
    /* Mild edge contrast; weight comes from the baked typeface. */
    a = fminf(1.f, fmaxf(0.f, (a - 0.04f) * 1.20f));
    return a;
}

extern "C" __global__ void clock_fill_y_u8(unsigned char *dst, int pitch,
                                           int width, int height,
                                           unsigned char val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    dst[y * pitch + x] = val;
}

extern "C" __global__ void clock_fill_uv_nv12(unsigned char *dst, int pitch,
                                              int width, int height,
                                              unsigned char u, unsigned char v)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned char *row;
    if (x >= width || y >= height)
        return;
    row = dst + y * pitch;
    row[(x << 1) + 0] = u;
    row[(x << 1) + 1] = v;
}

extern "C" __global__ void clock_fill_y_u16(unsigned char *dst, int pitch,
                                            int width, int height,
                                            unsigned short val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    if (x >= width || y >= height)
        return;
    row = (unsigned short *)(dst + y * pitch);
    row[x] = val;
}

extern "C" __global__ void clock_fill_uv_p010(unsigned char *dst, int pitch,
                                              int width, int height,
                                              unsigned short u, unsigned short v)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    if (x >= width || y >= height)
        return;
    row = (unsigned short *)(dst + y * pitch);
    row[(x << 1) + 0] = u;
    row[(x << 1) + 1] = v;
}

extern "C" __global__ void clock_blit_y_u8(unsigned char *dst, int pitch,
                                           int width, int height,
                                           const unsigned char *atlas,
                                           int atlas_w, int atlas_h,
                                           const ClockBlit *blits, int nblits,
                                           unsigned char y_fg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = blit_alpha(atlas, atlas_w, atlas_h, blits, nblits, (float)x, (float)y);
    if (a <= 0.001f)
        return;
    yv = dst[y * pitch + x] * (1.f - a) + (float)y_fg * a;
    dst[y * pitch + x] = (unsigned char)(yv + 0.5f);
}

extern "C" __global__ void clock_blit_uv_nv12(unsigned char *dst, int pitch,
                                              int width, int height,
                                              const unsigned char *atlas,
                                              int atlas_w, int atlas_h,
                                              const ClockBlit *blits, int nblits,
                                              unsigned char u_fg, unsigned char v_fg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned char *row;
    float a, uv;
    if (x >= width || y >= height)
        return;
    a = blit_alpha(atlas, atlas_w, atlas_h, blits, nblits,
                   (float)(x * 2) + 0.5f, (float)(y * 2) + 0.5f);
    if (a <= 0.001f)
        return;
    row = dst + y * pitch;
    uv = row[(x << 1) + 0] * (1.f - a) + (float)u_fg * a;
    row[(x << 1) + 0] = (unsigned char)(uv + 0.5f);
    uv = row[(x << 1) + 1] * (1.f - a) + (float)v_fg * a;
    row[(x << 1) + 1] = (unsigned char)(uv + 0.5f);
}

extern "C" __global__ void clock_blit_y_u16(unsigned char *dst, int pitch,
                                            int width, int height,
                                            const unsigned char *atlas,
                                            int atlas_w, int atlas_h,
                                            const ClockBlit *blits, int nblits,
                                            unsigned short y_fg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = blit_alpha(atlas, atlas_w, atlas_h, blits, nblits, (float)x, (float)y);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    yv = (float)row[x] * (1.f - a) + (float)y_fg * a;
    row[x] = (unsigned short)(yv + 0.5f);
}

extern "C" __global__ void clock_blit_uv_p010(unsigned char *dst, int pitch,
                                              int width, int height,
                                              const unsigned char *atlas,
                                              int atlas_w, int atlas_h,
                                              const ClockBlit *blits, int nblits,
                                              unsigned short u_fg, unsigned short v_fg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, uv;
    if (x >= width || y >= height)
        return;
    a = blit_alpha(atlas, atlas_w, atlas_h, blits, nblits,
                   (float)(x * 2) + 0.5f, (float)(y * 2) + 0.5f);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    uv = (float)row[(x << 1) + 0] * (1.f - a) + (float)u_fg * a;
    row[(x << 1) + 0] = (unsigned short)(uv + 0.5f);
    uv = (float)row[(x << 1) + 1] * (1.f - a) + (float)v_fg * a;
    row[(x << 1) + 1] = (unsigned short)(uv + 0.5f);
}

__device__ float sd_segment(float px, float py, float ax, float ay,
                            float bx, float by, float r)
{
    float pax = px - ax, pay = py - ay;
    float bax = bx - ax, bay = by - ay;
    float denom = bax * bax + bay * bay;
    float h = denom > 1e-8f ? (pax * bax + pay * bay) / denom : 0.f;
    float dx, dy;
    h = fminf(fmaxf(h, 0.f), 1.f);
    dx = pax - bax * h;
    dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy) - r;
}

__device__ float analog_cover(float px, float py, float radius,
                              float h_ang, float m_ang, float s_ang,
                              int show_seconds, int mode)
{
    float dist = sqrtf(px * px + py * py);
    float cov = 0.f;
    int i;
    float hx, hy, mx, my, sx, sy;
    float ring_w = fmaxf(radius * 0.007f, 1.8f);
    float t_hour = fmaxf(radius * 0.018f, 3.2f);
    float t_min  = fmaxf(radius * 0.010f, 2.0f);
    float t_sec  = fmaxf(radius * 0.0055f, 1.8f);
    float t_cap  = fmaxf(radius * 0.022f, 5.5f);
    float t_mark = fmaxf(radius * 0.0085f, 2.4f);
    float t_min_mark = fmaxf(radius * 0.0045f, 1.5f);

    if (mode & 1) {
        cov = fmaxf(cov, fminf(fmaxf(0.5f - (fabsf(dist - radius) - ring_w), 0.f), 1.f));
        for (i = 0; i < 60; i++) {
            float ang = (float)i * (float)(M_PI / 30.0);
            float c = __cosf(ang), s = __sinf(ang);
            float dx = s, dy = -c;
            float inner = (i % 5 == 0) ? 0.80f : 0.90f;
            float thick = (i % 5 == 0) ? t_mark : t_min_mark;
            float d = sd_segment(px, py,
                                 dx * radius * inner, dy * radius * inner,
                                 dx * radius * 0.97f, dy * radius * 0.97f,
                                 thick);
            cov = fmaxf(cov, fminf(fmaxf(0.5f - d, 0.f), 1.f));
        }
        cov = fmaxf(cov, fminf(fmaxf(0.5f - (dist - t_cap), 0.f), 1.f));
    }

    if (mode & 2) {
        hx = __sinf(h_ang) * radius * 0.50f;
        hy = -__cosf(h_ang) * radius * 0.50f;
        cov = fmaxf(cov, fminf(fmaxf(0.5f - sd_segment(px, py, 0, 0, hx, hy, t_hour), 0.f), 1.f));
        mx = __sinf(m_ang) * radius * 0.72f;
        my = -__cosf(m_ang) * radius * 0.72f;
        cov = fmaxf(cov, fminf(fmaxf(0.5f - sd_segment(px, py, 0, 0, mx, my, t_min), 0.f), 1.f));
        if (show_seconds) {
            sx = __sinf(s_ang) * radius * 0.84f;
            sy = -__cosf(s_ang) * radius * 0.84f;
            cov = fmaxf(cov, fminf(fmaxf(0.5f - sd_segment(px, py, 0, 0, sx, sy, t_sec), 0.f), 1.f));
        }
        cov = fmaxf(cov, fminf(fmaxf(0.5f - (dist - t_cap), 0.f), 1.f));
    }
    return fminf(cov, 1.f);
}

extern "C" __global__ void clock_analog_y_u8(unsigned char *dst, int pitch,
                                             int width, int height,
                                             float cx, float cy, float radius,
                                             float h_ang, float m_ang, float s_ang,
                                             unsigned char y_fg, int show_seconds,
                                             int mode)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = analog_cover((float)x + 0.5f - cx, (float)y + 0.5f - cy,
                     radius, h_ang, m_ang, s_ang, show_seconds, mode);
    if (a <= 0.001f)
        return;
    yv = dst[y * pitch + x] * (1.f - a) + (float)y_fg * a;
    dst[y * pitch + x] = (unsigned char)(yv + 0.5f);
}

extern "C" __global__ void clock_analog_uv_nv12(unsigned char *dst, int pitch,
                                                int width, int height,
                                                float cx, float cy, float radius,
                                                float h_ang, float m_ang, float s_ang,
                                                unsigned char u_fg, unsigned char v_fg,
                                                int show_seconds, int mode)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned char *row;
    float a, uv;
    if (x >= width || y >= height)
        return;
    a = analog_cover((float)(x * 2) + 0.5f - cx, (float)(y * 2) + 0.5f - cy,
                     radius, h_ang, m_ang, s_ang, show_seconds, mode);
    if (a <= 0.001f)
        return;
    row = dst + y * pitch;
    uv = row[(x << 1) + 0] * (1.f - a) + (float)u_fg * a;
    row[(x << 1) + 0] = (unsigned char)(uv + 0.5f);
    uv = row[(x << 1) + 1] * (1.f - a) + (float)v_fg * a;
    row[(x << 1) + 1] = (unsigned char)(uv + 0.5f);
}

extern "C" __global__ void clock_analog_y_u16(unsigned char *dst, int pitch,
                                              int width, int height,
                                              float cx, float cy, float radius,
                                              float h_ang, float m_ang, float s_ang,
                                              unsigned short y_fg, int show_seconds,
                                              int mode)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = analog_cover((float)x + 0.5f - cx, (float)y + 0.5f - cy,
                     radius, h_ang, m_ang, s_ang, show_seconds, mode);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    yv = (float)row[x] * (1.f - a) + (float)y_fg * a;
    row[x] = (unsigned short)(yv + 0.5f);
}

extern "C" __global__ void clock_analog_uv_p010(unsigned char *dst, int pitch,
                                                int width, int height,
                                                float cx, float cy, float radius,
                                                float h_ang, float m_ang, float s_ang,
                                                unsigned short u_fg, unsigned short v_fg,
                                                int show_seconds, int mode)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, uv;
    if (x >= width || y >= height)
        return;
    a = analog_cover((float)(x * 2) + 0.5f - cx, (float)(y * 2) + 0.5f - cy,
                     radius, h_ang, m_ang, s_ang, show_seconds, mode);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    uv = (float)row[(x << 1) + 0] * (1.f - a) + (float)u_fg * a;
    row[(x << 1) + 0] = (unsigned short)(uv + 0.5f);
    uv = (float)row[(x << 1) + 1] * (1.f - a) + (float)v_fg * a;
    row[(x << 1) + 1] = (unsigned short)(uv + 0.5f);
}

__device__ float sd_round_rect(float px, float py,
                               float x0, float y0, float w, float h, float r)
{
    float cx = x0 + w * 0.5f;
    float cy = y0 + h * 0.5f;
    float hx = w * 0.5f - r;
    float hy = h * 0.5f - r;
    float qx, qy, outside, inside;

    if (hx < 1.f)
        hx = 1.f;
    if (hy < 1.f)
        hy = 1.f;
    qx = fabsf(px - cx) - hx;
    qy = fabsf(py - cy) - hy;
    outside = sqrtf(fmaxf(qx, 0.f) * fmaxf(qx, 0.f) +
                    fmaxf(qy, 0.f) * fmaxf(qy, 0.f));
    inside = fminf(fmaxf(qx, qy), 0.f);
    return outside + inside - r;
}

__device__ float card_cover_y(float px, float py, const ClockCard *cards,
                              int ncards, float *y_out)
{
    float best_a = 0.f, best_y = 0.f;
    int i;

    for (i = 0; i < ncards; i++) {
        ClockCard c = cards[i];
        float d, a, t, slit, yv, mid;
        if (c.dw < 1.f || c.dh < 1.f)
            continue;
        d = sd_round_rect(px, py, c.dx, c.dy, c.dw, c.dh, c.radius);
        a = fminf(fmaxf(0.5f - d, 0.f), 1.f);
        if (a <= 0.001f)
            continue;
        t = fminf(fmaxf((py - c.dy) / c.dh, 0.f), 1.f);
        yv = c.y_top + (c.y_bot - c.y_top) * t;
        mid = c.dy + c.dh * 0.5f;
        slit = fminf(fmaxf(1.2f - fabsf(py - mid), 0.f), 1.f);
        yv -= (c.y_top - c.y_bot) * 0.35f * slit;
        if (a >= best_a) {
            best_a = a;
            best_y = yv;
        }
    }
    *y_out = best_y;
    return best_a;
}

extern "C" __global__ void clock_card_y_u8(unsigned char *dst, int pitch,
                                           int width, int height,
                                           const ClockCard *cards, int ncards)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = card_cover_y((float)x + 0.5f, (float)y + 0.5f, cards, ncards, &yv);
    if (a <= 0.001f)
        return;
    dst[y * pitch + x] = (unsigned char)(dst[y * pitch + x] * (1.f - a) + yv * a + 0.5f);
}

extern "C" __global__ void clock_card_uv_nv12(unsigned char *dst, int pitch,
                                              int width, int height,
                                              const ClockCard *cards, int ncards,
                                              unsigned char u, unsigned char v)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned char *row;
    float a, dummy;
    if (x >= width || y >= height)
        return;
    a = card_cover_y((float)(x * 2) + 0.5f, (float)(y * 2) + 0.5f,
                     cards, ncards, &dummy);
    if (a <= 0.001f)
        return;
    row = dst + y * pitch;
    row[(x << 1) + 0] = (unsigned char)(row[(x << 1) + 0] * (1.f - a) + u * a + 0.5f);
    row[(x << 1) + 1] = (unsigned char)(row[(x << 1) + 1] * (1.f - a) + v * a + 0.5f);
}

extern "C" __global__ void clock_card_y_u16(unsigned char *dst, int pitch,
                                            int width, int height,
                                            const ClockCard *cards, int ncards)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, yv;
    if (x >= width || y >= height)
        return;
    a = card_cover_y((float)x + 0.5f, (float)y + 0.5f, cards, ncards, &yv);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    row[x] = (unsigned short)((float)row[x] * (1.f - a) + yv * a + 0.5f);
}

extern "C" __global__ void clock_card_uv_p010(unsigned char *dst, int pitch,
                                              int width, int height,
                                              const ClockCard *cards, int ncards,
                                              unsigned short u, unsigned short v)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned short *row;
    float a, dummy;
    if (x >= width || y >= height)
        return;
    a = card_cover_y((float)(x * 2) + 0.5f, (float)(y * 2) + 0.5f,
                     cards, ncards, &dummy);
    if (a <= 0.001f)
        return;
    row = (unsigned short *)(dst + y * pitch);
    row[(x << 1) + 0] = (unsigned short)((float)row[(x << 1) + 0] * (1.f - a) + u * a + 0.5f);
    row[(x << 1) + 1] = (unsigned short)((float)row[(x << 1) + 1] * (1.f - a) + v * a + 0.5f);
}
