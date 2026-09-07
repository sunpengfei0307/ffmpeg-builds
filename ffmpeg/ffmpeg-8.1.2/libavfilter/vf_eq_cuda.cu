/*
 * CUDA video equalizer kernels
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

extern "C" {

__global__ void eq_uchar(unsigned char *dst, int dst_pitch,
                         const unsigned char *src, int src_pitch,
                         int w, int h, const unsigned char *lut)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;
    dst[y * dst_pitch + x] = lut[src[y * src_pitch + x]];
}

/* NV12 UV plane: two interleaved 8-bit samples */
__global__ void eq_uchar2(uchar2 *dst, int dst_pitch,
                          const uchar2 *src, int src_pitch,
                          int w, int h,
                          const unsigned char *lut_u,
                          const unsigned char *lut_v)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;
    uchar2 s = src[y * src_pitch + x];
    uchar2 d;
    d.x = lut_u[s.x];
    d.y = lut_v[s.y];
    dst[y * dst_pitch + x] = d;
}

/* P010 Y: 16-bit container, 10-bit left-aligned */
__global__ void eq_ushort(unsigned short *dst, int dst_pitch,
                          const unsigned short *src, int src_pitch,
                          int w, int h, const unsigned short *lut)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;
    unsigned short v = src[y * src_pitch + x] >> 6;
    dst[y * dst_pitch + x] = lut[v] << 6;
}

/* P010 UV interleaved */
__global__ void eq_ushort2(ushort2 *dst, int dst_pitch,
                           const ushort2 *src, int src_pitch,
                           int w, int h,
                           const unsigned short *lut_u,
                           const unsigned short *lut_v)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;
    ushort2 s = src[y * src_pitch + x];
    ushort2 d;
    d.x = lut_u[s.x >> 6] << 6;
    d.y = lut_v[s.y >> 6] << 6;
    dst[y * dst_pitch + x] = d;
}

}
