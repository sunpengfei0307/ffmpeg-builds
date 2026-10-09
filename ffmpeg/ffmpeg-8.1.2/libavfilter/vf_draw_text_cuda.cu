/*
 * Blend a straight-alpha RGBA bitmap onto CUDA frames.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

__device__ void dt_sample(const unsigned char *rgba, int pitch, int bw, int bh,
                          int lx, int ly, float *r, float *g, float *b, float *a)
{
    if ((unsigned)lx >= (unsigned)bw || (unsigned)ly >= (unsigned)bh) {
        *r = *g = *b = *a = 0.f;
        return;
    }
    const unsigned char *p = rgba + ly * pitch + lx * 4;
    *r = p[0] / 255.f;
    *g = p[1] / 255.f;
    *b = p[2] / 255.f;
    *a = p[3] / 255.f;
}

__device__ void dt_sample_block(const unsigned char *rgba, int pitch, int bw, int bh,
                                int lx, int ly, int hsub, int vsub,
                                float *r, float *g, float *b, float *a)
{
    const int nx = 1 << hsub;
    const int ny = 1 << vsub;
    float rs = 0, gs = 0, bs = 0, as = 0;
    int n = 0;
    for (int dy = 0; dy < ny; dy++) {
        for (int dx = 0; dx < nx; dx++) {
            float rr, gg, bb, aa;
            dt_sample(rgba, pitch, bw, bh, lx + dx, ly + dy, &rr, &gg, &bb, &aa);
            rs += rr; gs += gg; bs += bb; as += aa;
            n++;
        }
    }
    float inv = 1.f / (float)n;
    *r = rs * inv; *g = gs * inv; *b = bs * inv; *a = as * inv;
}

__device__ void dt_yuv(float r, float g, float b, int full, int depth, int shift,
                       float *y, float *u, float *v)
{
    const float Y  = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float cb = (b - Y) / 1.8556f;
    const float cr = (r - Y) / 1.5748f;
    float yy, uu, vv, scale = 1.f;

    if (depth <= 8) {
        yy = full ? Y * 255.f : Y * 219.f + 16.f;
        uu = full ? (cb + 0.5f) * 255.f : cb * 224.f + 128.f;
        vv = full ? (cr + 0.5f) * 255.f : cr * 224.f + 128.f;
    } else if (depth <= 10) {
        yy = full ? Y * 1023.f : Y * 876.f + 64.f;
        uu = full ? (cb + 0.5f) * 1023.f : cb * 896.f + 512.f;
        vv = full ? (cr + 0.5f) * 1023.f : cr * 896.f + 512.f;
        scale = (float)(1 << shift);
    } else {
        yy = full ? Y * 65535.f : Y * 56064.f + 4096.f;
        uu = full ? (cb + 0.5f) * 65535.f : cb * 57344.f + 32768.f;
        vv = full ? (cr + 0.5f) * 65535.f : cr * 57344.f + 32768.f;
        scale = (float)(1 << shift);
    }
    *y = yy * scale;
    *u = uu * scale;
    *v = vv * scale;
}

__device__ int dt_clamp_store(float v, int depth, int shift)
{
    float lim;
    if (depth <= 8)
        lim = 255.f;
    else if (depth <= 10)
        lim = 1023.f * (float)(1 << shift);
    else
        lim = 65535.f;
    if (v < 0.f) v = 0.f;
    if (v > lim) v = lim;
    return (int)(v + 0.5f);
}

extern "C" {

__global__ void dt_copy_bytes(unsigned char *dst, int dst_pitch,
                              const unsigned char *src, int src_pitch,
                              int row_bytes, int rows)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= row_bytes || y >= rows)
        return;
    dst[y * dst_pitch + x] = src[y * src_pitch + x];
}

/* comp: 0=Y 1=U 2=V. hsub/vsub are 0 for luma; planar chroma uses the plane's subsampling. */
__global__ void dt_blend_y_u8(unsigned char *dst, int pitch,
                              int x0, int y0, int rw, int rh,
                              int box_x, int box_y,
                              const unsigned char *rgba, int rgba_pitch,
                              int bw, int bh,
                              int hsub, int vsub,
                              int full, int depth, int shift, int comp)
{
    const int x = x0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int y = y0 + blockIdx.y * blockDim.y + threadIdx.y;
    float r, g, b, a, yy, uu, vv, srcv;
    if (x >= x0 + rw || y >= y0 + rh)
        return;
    dt_sample_block(rgba, rgba_pitch, bw, bh,
                    (x << hsub) - box_x, (y << vsub) - box_y,
                    hsub, vsub, &r, &g, &b, &a);
    if (a < 0.001f)
        return;
    dt_yuv(r, g, b, full, depth, shift, &yy, &uu, &vv);
    srcv = comp == 1 ? uu : comp == 2 ? vv : yy;
    {
        float dstv = dst[y * pitch + x];
        dst[y * pitch + x] = (unsigned char)dt_clamp_store(dstv * (1.f - a) + srcv * a, depth, shift);
    }
}

__global__ void dt_blend_y_u16(unsigned char *dst, int pitch,
                               int x0, int y0, int rw, int rh,
                               int box_x, int box_y,
                               const unsigned char *rgba, int rgba_pitch,
                               int bw, int bh,
                               int hsub, int vsub,
                               int full, int depth, int shift, int comp)
{
    const int x = x0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int y = y0 + blockIdx.y * blockDim.y + threadIdx.y;
    float r, g, b, a, yy, uu, vv, srcv;
    unsigned short *row;
    if (x >= x0 + rw || y >= y0 + rh)
        return;
    dt_sample_block(rgba, rgba_pitch, bw, bh,
                    (x << hsub) - box_x, (y << vsub) - box_y,
                    hsub, vsub, &r, &g, &b, &a);
    if (a < 0.001f)
        return;
    dt_yuv(r, g, b, full, depth, shift, &yy, &uu, &vv);
    srcv = comp == 1 ? uu : comp == 2 ? vv : yy;
    row = reinterpret_cast<unsigned short *>(dst + y * pitch);
    {
        float dstv = row[x];
        row[x] = (unsigned short)dt_clamp_store(dstv * (1.f - a) + srcv * a, depth, shift);
    }
}

__global__ void dt_blend_uv_u8(unsigned char *dst, int pitch,
                               int cx0, int cy0, int cw, int ch,
                               int box_x, int box_y,
                               const unsigned char *rgba, int rgba_pitch,
                               int bw, int bh,
                               int hsub, int vsub,
                               int full, int depth, int shift)
{
    const int cx = cx0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int cy = cy0 + blockIdx.y * blockDim.y + threadIdx.y;
    float r, g, b, a, yy, uu, vv;
    unsigned char *p;
    if (cx >= cx0 + cw || cy >= cy0 + ch)
        return;
    dt_sample_block(rgba, rgba_pitch, bw, bh,
                    (cx << hsub) - box_x, (cy << vsub) - box_y,
                    hsub, vsub, &r, &g, &b, &a);
    if (a < 0.001f)
        return;
    dt_yuv(r, g, b, full, depth, shift, &yy, &uu, &vv);
    p = dst + cy * pitch + cx * 2;
    p[0] = (unsigned char)dt_clamp_store((float)p[0] * (1.f - a) + uu * a, depth, shift);
    p[1] = (unsigned char)dt_clamp_store((float)p[1] * (1.f - a) + vv * a, depth, shift);
}

__global__ void dt_blend_uv_u16(unsigned char *dst, int pitch,
                                int cx0, int cy0, int cw, int ch,
                                int box_x, int box_y,
                                const unsigned char *rgba, int rgba_pitch,
                                int bw, int bh,
                                int hsub, int vsub,
                                int full, int depth, int shift)
{
    const int cx = cx0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int cy = cy0 + blockIdx.y * blockDim.y + threadIdx.y;
    float r, g, b, a, yy, uu, vv;
    unsigned short *row;
    if (cx >= cx0 + cw || cy >= cy0 + ch)
        return;
    dt_sample_block(rgba, rgba_pitch, bw, bh,
                    (cx << hsub) - box_x, (cy << vsub) - box_y,
                    hsub, vsub, &r, &g, &b, &a);
    if (a < 0.001f)
        return;
    dt_yuv(r, g, b, full, depth, shift, &yy, &uu, &vv);
    row = reinterpret_cast<unsigned short *>(dst + cy * pitch);
    row[cx * 2]     = (unsigned short)dt_clamp_store((float)row[cx * 2]     * (1.f - a) + uu * a, depth, shift);
    row[cx * 2 + 1] = (unsigned short)dt_clamp_store((float)row[cx * 2 + 1] * (1.f - a) + vv * a, depth, shift);
}

/* order: 0=RGBA 1=BGRA 2=RGBX 3=BGRX */
__global__ void dt_blend_packed(unsigned char *dst, int pitch,
                                int x0, int y0, int rw, int rh,
                                int box_x, int box_y,
                                const unsigned char *rgba, int rgba_pitch,
                                int bw, int bh, int order)
{
    const int x = x0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int y = y0 + blockIdx.y * blockDim.y + threadIdx.y;
    float r, g, b, a;
    unsigned char *p;
    int br, bg, bb;
    if (x >= x0 + rw || y >= y0 + rh)
        return;
    dt_sample(rgba, rgba_pitch, bw, bh, x - box_x, y - box_y, &r, &g, &b, &a);
    if (a < 0.001f)
        return;
    p = dst + y * pitch + x * 4;
    if (order == 0 || order == 2) {
        br = p[0]; bg = p[1]; bb = p[2];
    } else {
        bb = p[0]; bg = p[1]; br = p[2];
    }
    br = (int)((float)br * (1.f - a) + r * 255.f * a + 0.5f);
    bg = (int)((float)bg * (1.f - a) + g * 255.f * a + 0.5f);
    bb = (int)((float)bb * (1.f - a) + b * 255.f * a + 0.5f);
    if (br < 0) br = 0; if (br > 255) br = 255;
    if (bg < 0) bg = 0; if (bg > 255) bg = 255;
    if (bb < 0) bb = 0; if (bb > 255) bb = 255;
    if (order == 0 || order == 2) {
        p[0] = (unsigned char)br; p[1] = (unsigned char)bg; p[2] = (unsigned char)bb;
    } else {
        p[0] = (unsigned char)bb; p[1] = (unsigned char)bg; p[2] = (unsigned char)br;
    }
    if (order == 0 || order == 1) {
        int ba = (int)((float)p[3] * (1.f - a) + 255.f * a + 0.5f);
        if (ba < 0) ba = 0;
        if (ba > 255) ba = 255;
        p[3] = (unsigned char)ba;
    }
}

}
