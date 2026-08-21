/*
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
__device__ void pad_impl(T* dst, int dst_pitch, int dst_w, int dst_h,
                         const T* src, int src_pitch, int src_w, int src_h,
                         int roi_x, int roi_y, T fill_val)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h) {
        return;
    }

    if (x >= roi_x && x < (roi_x + src_w) && y >= roi_y && y < (roi_y + src_h)) {
        const int src_x = x - roi_x;
        const int src_y = y - roi_y;
        dst[y * dst_pitch + x] = src[src_y * src_pitch + src_x];
    } else {
        dst[y * dst_pitch + x] = fill_val;
    }
}

__device__ float bilinear_sample_u8(const unsigned char *src, int src_pitch,
                                   int src_w, int src_h, float fx, float fy)
{
    fx = fminf(fmaxf(fx, 0.f), (float)(src_w - 1));
    fy = fminf(fmaxf(fy, 0.f), (float)(src_h - 1));

    const int x0 = (int)floorf(fx);
    const int y0 = (int)floorf(fy);
    const int x1 = min(x0 + 1, src_w - 1);
    const int y1 = min(y0 + 1, src_h - 1);
    const float dx = fx - (float)x0;
    const float dy = fy - (float)y0;

    const float v00 = src[y0 * src_pitch + x0];
    const float v10 = src[y0 * src_pitch + x1];
    const float v01 = src[y1 * src_pitch + x0];
    const float v11 = src[y1 * src_pitch + x1];

    return (1.f - dx) * (1.f - dy) * v00 + dx * (1.f - dy) * v10
         + (1.f - dx) * dy * v01 + dx * dy * v11;
}

__device__ float bilinear_sample_u16(const unsigned short *src, int src_pitch,
                                    int src_w, int src_h, float fx, float fy)
{
    fx = fminf(fmaxf(fx, 0.f), (float)(src_w - 1));
    fy = fminf(fmaxf(fy, 0.f), (float)(src_h - 1));

    const int x0 = (int)floorf(fx);
    const int y0 = (int)floorf(fy);
    const int x1 = min(x0 + 1, src_w - 1);
    const int y1 = min(y0 + 1, src_h - 1);
    const float dx = fx - (float)x0;
    const float dy = fy - (float)y0;

    const float v00 = src[y0 * src_pitch + x0];
    const float v10 = src[y0 * src_pitch + x1];
    const float v01 = src[y1 * src_pitch + x0];
    const float v11 = src[y1 * src_pitch + x1];

    return (1.f - dx) * (1.f - dy) * v00 + dx * (1.f - dy) * v10
         + (1.f - dx) * dy * v01 + dx * dy * v11;
}

__device__ void pad_scale_u8(unsigned char* dst, int dst_pitch, int dst_w, int dst_h,
                             const unsigned char* src, int src_pitch, int src_w, int src_h,
                             int roi_x, int roi_y, int scaled_w, int scaled_h,
                             unsigned char fill_val)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h)
        return;

    if (x < roi_x || x >= roi_x + scaled_w || y < roi_y || y >= roi_y + scaled_h) {
        dst[y * dst_pitch + x] = fill_val;
        return;
    }

    const float src_fx = ((float)(x - roi_x) + 0.5f) * (float)src_w / (float)scaled_w - 0.5f;
    const float src_fy = ((float)(y - roi_y) + 0.5f) * (float)src_h / (float)scaled_h - 0.5f;
    dst[y * dst_pitch + x] = (unsigned char)(bilinear_sample_u8(src, src_pitch, src_w, src_h,
                                                                src_fx, src_fy) + 0.5f);
}

__device__ void pad_scale_u8x2(uchar2* dst, int dst_pitch, int dst_w, int dst_h,
                               const uchar2* src, int src_pitch, int src_w, int src_h,
                               int roi_x, int roi_y, int scaled_w, int scaled_h,
                               uchar2 fill_val)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h)
        return;

    if (x < roi_x || x >= roi_x + scaled_w || y < roi_y || y >= roi_y + scaled_h) {
        dst[y * dst_pitch + x] = fill_val;
        return;
    }

    const float src_fx = ((float)(x - roi_x) + 0.5f) * (float)src_w / (float)scaled_w - 0.5f;
    const float src_fy = ((float)(y - roi_y) + 0.5f) * (float)src_h / (float)scaled_h - 0.5f;

    float fx = fminf(fmaxf(src_fx, 0.f), (float)(src_w - 1));
    float fy = fminf(fmaxf(src_fy, 0.f), (float)(src_h - 1));
    const int x0 = (int)floorf(fx);
    const int y0 = (int)floorf(fy);
    const int x1 = min(x0 + 1, src_w - 1);
    const int y1 = min(y0 + 1, src_h - 1);
    const float dx = fx - (float)x0;
    const float dy = fy - (float)y0;

    const uchar2 v00 = src[y0 * src_pitch + x0];
    const uchar2 v10 = src[y0 * src_pitch + x1];
    const uchar2 v01 = src[y1 * src_pitch + x0];
    const uchar2 v11 = src[y1 * src_pitch + x1];

    uchar2 out;
    out.x = (unsigned char)((1.f - dx) * (1.f - dy) * v00.x + dx * (1.f - dy) * v10.x
                          + (1.f - dx) * dy * v01.x + dx * dy * v11.x + 0.5f);
    out.y = (unsigned char)((1.f - dx) * (1.f - dy) * v00.y + dx * (1.f - dy) * v10.y
                          + (1.f - dx) * dy * v01.y + dx * dy * v11.y + 0.5f);
    dst[y * dst_pitch + x] = out;
}

__device__ void pad_scale_u16(unsigned short* dst, int dst_pitch, int dst_w, int dst_h,
                              const unsigned short* src, int src_pitch, int src_w, int src_h,
                              int roi_x, int roi_y, int scaled_w, int scaled_h,
                              unsigned short fill_val)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h)
        return;

    if (x < roi_x || x >= roi_x + scaled_w || y < roi_y || y >= roi_y + scaled_h) {
        dst[y * dst_pitch + x] = fill_val;
        return;
    }

    const float src_fx = ((float)(x - roi_x) + 0.5f) * (float)src_w / (float)scaled_w - 0.5f;
    const float src_fy = ((float)(y - roi_y) + 0.5f) * (float)src_h / (float)scaled_h - 0.5f;
    dst[y * dst_pitch + x] = (unsigned short)(bilinear_sample_u16(src, src_pitch, src_w, src_h,
                                                                  src_fx, src_fy) + 0.5f);
}

__device__ void pad_scale_u16x2(ushort2* dst, int dst_pitch, int dst_w, int dst_h,
                                const ushort2* src, int src_pitch, int src_w, int src_h,
                                int roi_x, int roi_y, int scaled_w, int scaled_h,
                                ushort2 fill_val)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= dst_w || y >= dst_h)
        return;

    if (x < roi_x || x >= roi_x + scaled_w || y < roi_y || y >= roi_y + scaled_h) {
        dst[y * dst_pitch + x] = fill_val;
        return;
    }

    const float src_fx = ((float)(x - roi_x) + 0.5f) * (float)src_w / (float)scaled_w - 0.5f;
    const float src_fy = ((float)(y - roi_y) + 0.5f) * (float)src_h / (float)scaled_h - 0.5f;

    float fx = fminf(fmaxf(src_fx, 0.f), (float)(src_w - 1));
    float fy = fminf(fmaxf(src_fy, 0.f), (float)(src_h - 1));
    const int x0 = (int)floorf(fx);
    const int y0 = (int)floorf(fy);
    const int x1 = min(x0 + 1, src_w - 1);
    const int y1 = min(y0 + 1, src_h - 1);
    const float dx = fx - (float)x0;
    const float dy = fy - (float)y0;

    const ushort2 v00 = src[y0 * src_pitch + x0];
    const ushort2 v10 = src[y0 * src_pitch + x1];
    const ushort2 v01 = src[y1 * src_pitch + x0];
    const ushort2 v11 = src[y1 * src_pitch + x1];

    ushort2 out;
    out.x = (unsigned short)((1.f - dx) * (1.f - dy) * v00.x + dx * (1.f - dy) * v10.x
                           + (1.f - dx) * dy * v01.x + dx * dy * v11.x + 0.5f);
    out.y = (unsigned short)((1.f - dx) * (1.f - dy) * v00.y + dx * (1.f - dy) * v10.y
                           + (1.f - dx) * dy * v01.y + dx * dy * v11.y + 0.5f);
    dst[y * dst_pitch + x] = out;
}


extern "C" {

__global__ void pad_uchar(unsigned char* dst, int dst_pitch, int dst_w, int dst_h,
                          const unsigned char* src, int src_pitch, int src_w, int src_h,
                          int roi_x, int roi_y, unsigned char fill_val)
{
    pad_impl<unsigned char>(dst, dst_pitch, dst_w, dst_h,
                            src, src_pitch, src_w, src_h,
                            roi_x, roi_y, fill_val);
}

__global__ void pad_uchar2(uchar2* dst, int dst_pitch, int dst_w, int dst_h,
                           const uchar2* src, int src_pitch, int src_w, int src_h,
                           int roi_x, int roi_y, uchar2 fill_val)
{
    pad_impl<uchar2>(dst, dst_pitch, dst_w, dst_h,
                     src, src_pitch, src_w, src_h,
                     roi_x, roi_y, fill_val);
}

__global__ void pad_ushort(unsigned short* dst, int dst_pitch, int dst_w, int dst_h,
                           const unsigned short* src, int src_pitch, int src_w, int src_h,
                           int roi_x, int roi_y, unsigned short fill_val)
{
    pad_impl<unsigned short>(dst, dst_pitch, dst_w, dst_h,
                             src, src_pitch, src_w, src_h,
                             roi_x, roi_y, fill_val);
}

__global__ void pad_ushort2(ushort2* dst, int dst_pitch, int dst_w, int dst_h,
                            const ushort2* src, int src_pitch, int src_w, int src_h,
                            int roi_x, int roi_y, ushort2 fill_val)
{
    pad_impl<ushort2>(dst, dst_pitch, dst_w, dst_h,
                      src, src_pitch, src_w, src_h,
                      roi_x, roi_y, fill_val);
}

__global__ void pad_scale_uchar(unsigned char* dst, int dst_pitch, int dst_w, int dst_h,
                                const unsigned char* src, int src_pitch, int src_w, int src_h,
                                int roi_x, int roi_y, int scaled_w, int scaled_h,
                                unsigned char fill_val)
{
    pad_scale_u8(dst, dst_pitch, dst_w, dst_h,
                 src, src_pitch, src_w, src_h,
                 roi_x, roi_y, scaled_w, scaled_h, fill_val);
}

__global__ void pad_scale_uchar2(uchar2* dst, int dst_pitch, int dst_w, int dst_h,
                                 const uchar2* src, int src_pitch, int src_w, int src_h,
                                 int roi_x, int roi_y, int scaled_w, int scaled_h,
                                 uchar2 fill_val)
{
    pad_scale_u8x2(dst, dst_pitch, dst_w, dst_h,
                   src, src_pitch, src_w, src_h,
                   roi_x, roi_y, scaled_w, scaled_h, fill_val);
}

__global__ void pad_scale_ushort(unsigned short* dst, int dst_pitch, int dst_w, int dst_h,
                                 const unsigned short* src, int src_pitch, int src_w, int src_h,
                                 int roi_x, int roi_y, int scaled_w, int scaled_h,
                                 unsigned short fill_val)
{
    pad_scale_u16(dst, dst_pitch, dst_w, dst_h,
                  src, src_pitch, src_w, src_h,
                  roi_x, roi_y, scaled_w, scaled_h, fill_val);
}

__global__ void pad_scale_ushort2(ushort2* dst, int dst_pitch, int dst_w, int dst_h,
                                  const ushort2* src, int src_pitch, int src_w, int src_h,
                                  int roi_x, int roi_y, int scaled_w, int scaled_h,
                                  ushort2 fill_val)
{
    pad_scale_u16x2(dst, dst_pitch, dst_w, dst_h,
                    src, src_pitch, src_w, src_h,
                    roi_x, roi_y, scaled_w, scaled_h, fill_val);
}

}
