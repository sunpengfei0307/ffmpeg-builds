/*
 * CUDA kernels for color_cuda video source.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

extern "C" __global__ void color_fill_y_u8(unsigned char *dst, int pitch,
                                           int width, int height,
                                           unsigned char val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height)
        return;
    dst[y * pitch + x] = val;
}

extern "C" __global__ void color_fill_uv_nv12(unsigned char *dst, int pitch,
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

extern "C" __global__ void color_fill_y_u16(unsigned char *dst, int pitch,
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

extern "C" __global__ void color_fill_uv_p010(unsigned char *dst, int pitch,
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
