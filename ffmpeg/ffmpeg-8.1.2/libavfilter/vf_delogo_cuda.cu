/*
 * CUDA delogo kernels.
 * Interpolation matches vf_delogo.c; an optional box blur softens the fill.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

template<typename T>
__device__ T delogo_load(const unsigned char *base, int pitch,
                         int w, int h, int x, int y, int step, int ch)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    const T *row = reinterpret_cast<const T *>(base + y * pitch);
    return row[x * step + ch];
}

template<typename T>
__device__ void delogo_store(unsigned char *base, int pitch,
                             int x, int y, int step, int ch, int v, int maxv)
{
    if (v < 0) v = 0;
    if (v > maxv) v = maxv;
    T *row = reinterpret_cast<T *>(base + y * pitch);
    row[x * step + ch] = (T)v;
}

template<typename T>
__device__ void delogo_interp(unsigned char *dst, int pitch,
                              int plane_w, int plane_h,
                              int step, int ch,
                              int x, int y,
                              int x1, int y1, int x2, int y2,
                              int logo_x, int logo_y, int logo_w, int logo_h,
                              int band, int show,
                              int sar_num, int sar_den, int maxv)
{
    if (show && (y == y1 + 1 || y == y2 - 1 || x == x1 + 1 || x == x2 - 1)) {
        delogo_store<T>(dst, pitch, x, y, step, ch, 0, maxv);
        return;
    }

    const double left =
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x1, y,     step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x1, y - 1, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x1, y + 1, step, ch);
    const double right =
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x2, y,     step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x2, y - 1, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x2, y + 1, step, ch);
    const double top =
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x,     y1, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x - 1, y1, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x + 1, y1, step, ch);
    const double bot =
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x,     y2, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x - 1, y2, step, ch) +
        (double)delogo_load<T>(dst, pitch, plane_w, plane_h, x + 1, y2, step, ch);

    const double wl = (double)(x2 - x) * (y - y1) * (y2 - y) * (double)sar_den;
    const double wr = (double)(x - x1) * (y - y1) * (y2 - y) * (double)sar_den;
    const double wt = (double)(x - x1) * (x2 - x) * (y2 - y) * (double)sar_num;
    const double wb = (double)(x - x1) * (x2 - x) * (y - y1) * (double)sar_num;
    const double weight = (wl + wr + wt + wb) * 3.0;
    double interp = left * wl + right * wr + top * wt + bot * wb;
    if (weight > 1e-6)
        interp = (interp + weight * 0.5) / weight;
    else
        interp = left / 3.0;

    const int in_core = band <= 0 ||
        (y >= logo_y + band && y < logo_y + logo_h - band &&
         x >= logo_x + band && x < logo_x + logo_w - band);

    double outv = interp;
    if (!in_core && band > 0) {
        int dist = 0;
        const double srcv = delogo_load<T>(dst, pitch, plane_w, plane_h, x, y, step, ch);
        if (x < logo_x + band)
            dist = logo_x - x + band > dist ? logo_x - x + band : dist;
        else if (x >= logo_x + logo_w - band) {
            int d = x - (logo_x + logo_w - 1 - band);
            dist = d > dist ? d : dist;
        }
        if (y < logo_y + band) {
            int d = logo_y - y + band;
            dist = d > dist ? d : dist;
        } else if (y >= logo_y + logo_h - band) {
            int d = y - (logo_y + logo_h - 1 - band);
            dist = d > dist ? d : dist;
        }
        outv = (srcv * dist + interp * (band - dist)) / (double)band;
    }

    delogo_store<T>(dst, pitch, x, y, step, ch, (int)(outv + 0.5), maxv);
}

extern "C" {

__global__ void delogo_copy_bytes(unsigned char *dst, int dst_pitch,
                                  const unsigned char *src, int src_pitch,
                                  int row_bytes, int rows)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= row_bytes || y >= rows)
        return;
    dst[y * dst_pitch + x] = src[y * src_pitch + x];
}

__global__ void delogo_u8(unsigned char *dst, int pitch,
                          int plane_w, int plane_h,
                          int step, int ch,
                          int x0, int y0, int rw, int rh,
                          int x1, int y1, int x2, int y2,
                          int logo_x, int logo_y, int logo_w, int logo_h,
                          int band, int show,
                          int sar_num, int sar_den, int maxv)
{
    const int x = x0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int y = y0 + blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= x0 + rw || y >= y0 + rh)
        return;
    if (x <= x1 || x >= x2 || y <= y1 || y >= y2)
        return;
    delogo_interp<unsigned char>(dst, pitch, plane_w, plane_h, step, ch,
                                 x, y, x1, y1, x2, y2,
                                 logo_x, logo_y, logo_w, logo_h,
                                 band, show, sar_num, sar_den, maxv);
}

__global__ void delogo_u16(unsigned char *dst, int pitch,
                           int plane_w, int plane_h,
                           int step, int ch,
                           int x0, int y0, int rw, int rh,
                           int x1, int y1, int x2, int y2,
                           int logo_x, int logo_y, int logo_w, int logo_h,
                           int band, int show,
                           int sar_num, int sar_den, int maxv)
{
    const int x = x0 + blockIdx.x * blockDim.x + threadIdx.x;
    const int y = y0 + blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= x0 + rw || y >= y0 + rh)
        return;
    if (x <= x1 || x >= x2 || y <= y1 || y >= y2)
        return;
    delogo_interp<unsigned short>(dst, pitch, plane_w, plane_h, step, ch,
                                  x, y, x1, y1, x2, y2,
                                  logo_x, logo_y, logo_w, logo_h,
                                  band, show, sar_num, sar_den, maxv);
}

__global__ void delogo_blur_u8(unsigned char *scratch, int spitch,
                               const unsigned char *src, int pitch,
                               int step, int ch,
                               int x0, int y0, int rw, int rh, int radius)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= rw || ly >= rh)
        return;

    const int x = x0 + lx;
    const int y = y0 + ly;
    int sum = 0;
    int n = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        int yy = y + dy;
        if (yy < y0) yy = y0;
        if (yy >= y0 + rh) yy = y0 + rh - 1;
        for (int dx = -radius; dx <= radius; dx++) {
            int xx = x + dx;
            if (xx < x0) xx = x0;
            if (xx >= x0 + rw) xx = x0 + rw - 1;
            sum += src[yy * pitch + xx * step + ch];
            n++;
        }
    }
    scratch[ly * spitch + lx] = (unsigned char)(sum / n);
}

__global__ void delogo_blur_u16(unsigned short *scratch, int spitch,
                                const unsigned char *src, int pitch,
                                int step, int ch,
                                int x0, int y0, int rw, int rh, int radius)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= rw || ly >= rh)
        return;

    const int x = x0 + lx;
    const int y = y0 + ly;
    int sum = 0;
    int n = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        int yy = y + dy;
        if (yy < y0) yy = y0;
        if (yy >= y0 + rh) yy = y0 + rh - 1;
        const unsigned short *row = reinterpret_cast<const unsigned short *>(src + yy * pitch);
        for (int dx = -radius; dx <= radius; dx++) {
            int xx = x + dx;
            if (xx < x0) xx = x0;
            if (xx >= x0 + rw) xx = x0 + rw - 1;
            sum += row[xx * step + ch];
            n++;
        }
    }
    scratch[ly * spitch + lx] = (unsigned short)(sum / n);
}

__global__ void delogo_scatter_u8(unsigned char *dst, int pitch,
                                  int step, int ch,
                                  const unsigned char *scratch, int spitch,
                                  int x0, int y0, int rw, int rh)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= rw || ly >= rh)
        return;
    dst[(y0 + ly) * pitch + (x0 + lx) * step + ch] = scratch[ly * spitch + lx];
}

__global__ void delogo_scatter_u16(unsigned char *dst, int pitch,
                                   int step, int ch,
                                   const unsigned short *scratch, int spitch,
                                   int x0, int y0, int rw, int rh)
{
    const int lx = blockIdx.x * blockDim.x + threadIdx.x;
    const int ly = blockIdx.y * blockDim.y + threadIdx.y;
    if (lx >= rw || ly >= rh)
        return;
    unsigned short *row = reinterpret_cast<unsigned short *>(dst + (y0 + ly) * pitch);
    row[(x0 + lx) * step + ch] = scratch[ly * spitch + lx];
}

}
