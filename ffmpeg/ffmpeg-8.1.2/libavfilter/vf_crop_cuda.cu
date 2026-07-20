/*
 * CUDA physical crop filter kernels
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

template <typename T>
__device__ void crop_impl(T *dst, int dst_pitch,
                          const T *src, int src_pitch,
                          int dst_w, int dst_h,
                          int src_x, int src_y)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h)
        return;

    dst[y * dst_pitch + x] = src[(y + src_y) * src_pitch + (x + src_x)];
}

extern "C" {

__global__ void crop_uchar(unsigned char *dst, int dst_pitch,
                           const unsigned char *src, int src_pitch,
                           int dst_w, int dst_h,
                           int src_x, int src_y)
{
    crop_impl(dst, dst_pitch, src, src_pitch, dst_w, dst_h, src_x, src_y);
}

__global__ void crop_uchar2(uchar2 *dst, int dst_pitch,
                            const uchar2 *src, int src_pitch,
                            int dst_w, int dst_h,
                            int src_x, int src_y)
{
    crop_impl(dst, dst_pitch, src, src_pitch, dst_w, dst_h, src_x, src_y);
}

__global__ void crop_ushort(unsigned short *dst, int dst_pitch,
                            const unsigned short *src, int src_pitch,
                            int dst_w, int dst_h,
                            int src_x, int src_y)
{
    crop_impl(dst, dst_pitch, src, src_pitch, dst_w, dst_h, src_x, src_y);
}

__global__ void crop_ushort2(ushort2 *dst, int dst_pitch,
                             const ushort2 *src, int src_pitch,
                             int dst_w, int dst_h,
                             int src_x, int src_y)
{
    crop_impl(dst, dst_pitch, src, src_pitch, dst_w, dst_h, src_x, src_y);
}

}
