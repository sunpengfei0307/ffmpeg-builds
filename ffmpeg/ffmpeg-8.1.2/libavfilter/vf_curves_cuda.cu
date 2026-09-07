/*
 * CUDA curves filter kernels
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

extern "C" {

static __device__ float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* BT.709: YUV (normalized) -> RGB */
static __device__ void yuv_to_rgb_bt709(float Y, float U, float V,
                                       float *R, float *G, float *B)
{
    *R = Y + 1.5748f * V;
    *G = Y - 0.187324f * U - 0.468124f * V;
    *B = Y + 1.8556f * U;
}

/* BT.709: RGB -> YUV (normalized) */
static __device__ void rgb_to_yuv_bt709(float R, float G, float B,
                                       float *Y, float *U, float *V)
{
    *Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    *U = -0.114572f * R - 0.385428f * G + 0.5f * B;
    *V = 0.5f * R - 0.454153f * G - 0.045847f * B;
}

static __device__ float sample_y8(const unsigned char *src, int pitch, int x, int y, int full)
{
    float v = src[y * pitch + x];
    return full ? v / 255.0f : clampf((v - 16.0f) / 219.0f, 0.0f, 1.0f);
}

static __device__ void sample_uv8(const uchar2 *src, int pitch, int x, int y, int full,
                                  float *U, float *V)
{
    uchar2 uv = src[y * pitch + x];
    if (full) {
        *U = uv.x / 255.0f - 0.5f;
        *V = uv.y / 255.0f - 0.5f;
    } else {
        *U = (uv.x - 128.0f) / 224.0f;
        *V = (uv.y - 128.0f) / 224.0f;
    }
}

static __device__ void store_y8(unsigned char *dst, int pitch, int x, int y, float Y, int full)
{
    Y = clampf(Y, 0.0f, 1.0f);
    dst[y * pitch + x] = full
        ? (unsigned char)(Y * 255.0f + 0.5f)
        : (unsigned char)(Y * 219.0f + 16.0f + 0.5f);
}

static __device__ void store_uv8(uchar2 *dst, int pitch, int x, int y,
                                 float U, float V, int full)
{
    uchar2 out;
    if (full) {
        out.x = (unsigned char)(clampf(U + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        out.y = (unsigned char)(clampf(V + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    } else {
        out.x = (unsigned char)(clampf(U * 224.0f + 128.0f, 0.0f, 255.0f) + 0.5f);
        out.y = (unsigned char)(clampf(V * 224.0f + 128.0f, 0.0f, 255.0f) + 0.5f);
    }
    dst[y * pitch + x] = out;
}

static __device__ float sample_y10(const unsigned short *src, int pitch, int x, int y, int full)
{
    float v = src[y * pitch + x] >> 6;
    return full ? v / 1023.0f : clampf((v - 64.0f) / 876.0f, 0.0f, 1.0f);
}

static __device__ void sample_uv10(const ushort2 *src, int pitch, int x, int y, int full,
                                   float *U, float *V)
{
    ushort2 uv = src[y * pitch + x];
    if (full) {
        *U = (uv.x >> 6) / 1023.0f - 0.5f;
        *V = (uv.y >> 6) / 1023.0f - 0.5f;
    } else {
        *U = ((uv.x >> 6) - 512.0f) / 896.0f;
        *V = ((uv.y >> 6) - 512.0f) / 896.0f;
    }
}

static __device__ void store_y10(unsigned short *dst, int pitch, int x, int y, float Y, int full)
{
    Y = clampf(Y, 0.0f, 1.0f);
    unsigned short v = full
        ? (unsigned short)(Y * 1023.0f + 0.5f)
        : (unsigned short)(Y * 876.0f + 64.0f + 0.5f);
    dst[y * pitch + x] = v << 6;
}

static __device__ void store_uv10(ushort2 *dst, int pitch, int x, int y,
                                  float U, float V, int full)
{
    ushort2 out;
    if (full) {
        out.x = (unsigned short)(clampf(U + 0.5f, 0.0f, 1.0f) * 1023.0f + 0.5f) << 6;
        out.y = (unsigned short)(clampf(V + 0.5f, 0.0f, 1.0f) * 1023.0f + 0.5f) << 6;
    } else {
        out.x = (unsigned short)(clampf(U * 896.0f + 512.0f, 0.0f, 1023.0f) + 0.5f) << 6;
        out.y = (unsigned short)(clampf(V * 896.0f + 512.0f, 0.0f, 1023.0f) + 0.5f) << 6;
    }
    dst[y * pitch + x] = out;
}

static __device__ float apply_lut8(const unsigned char *lut, float v)
{
    int i = (int)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    return lut[i] / 255.0f;
}

static __device__ float apply_lut10(const unsigned short *lut, float v)
{
    int i = (int)(clampf(v, 0.0f, 1.0f) * 1023.0f + 0.5f);
    return lut[i] / 1023.0f;
}

static __device__ void curves_rgb8(float R, float G, float B,
                                  const unsigned char *lut_r,
                                  const unsigned char *lut_g,
                                  const unsigned char *lut_b,
                                  float *Ro, float *Go, float *Bo)
{
    *Ro = apply_lut8(lut_r, clampf(R, 0.0f, 1.0f));
    *Go = apply_lut8(lut_g, clampf(G, 0.0f, 1.0f));
    *Bo = apply_lut8(lut_b, clampf(B, 0.0f, 1.0f));
}

static __device__ void curves_rgb10(float R, float G, float B,
                                   const unsigned short *lut_r,
                                   const unsigned short *lut_g,
                                   const unsigned short *lut_b,
                                   float *Ro, float *Go, float *Bo)
{
    *Ro = apply_lut10(lut_r, clampf(R, 0.0f, 1.0f));
    *Go = apply_lut10(lut_g, clampf(G, 0.0f, 1.0f));
    *Bo = apply_lut10(lut_b, clampf(B, 0.0f, 1.0f));
}

/**
 * NV12 fused curves: YUV->RGB (BT.709) -> LUT -> RGB->YUV
 * Threads cover full luma; UV written by even (x,y) with 2x2 RGB average.
 */
__global__ void curves_nv12(const unsigned char *src_y, const uchar2 *src_uv,
                            unsigned char *dst_y, uchar2 *dst_uv,
                            int src_y_pitch, int src_uv_pitch,
                            int dst_y_pitch, int dst_uv_pitch,
                            int width, int height,
                            const unsigned char *lut_r,
                            const unsigned char *lut_g,
                            const unsigned char *lut_b,
                            int full)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const int ux = x >> 1;
    const int uy = y >> 1;
    float U, V, Yn, R, G, B, Ro, Go, Bo, Yo, Uo, Vo;

    sample_uv8(src_uv, src_uv_pitch, ux, uy, full, &U, &V);
    Yn = sample_y8(src_y, src_y_pitch, x, y, full);
    yuv_to_rgb_bt709(Yn, U, V, &R, &G, &B);
    curves_rgb8(R, G, B, lut_r, lut_g, lut_b, &Ro, &Go, &Bo);
    rgb_to_yuv_bt709(Ro, Go, Bo, &Yo, &Uo, &Vo);
    store_y8(dst_y, dst_y_pitch, x, y, Yo, full);

    if ((x & 1) == 0 && (y & 1) == 0) {
        float ar = 0.0f, ag = 0.0f, ab = 0.0f;
        int count = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int xx = x + dx, yy = y + dy;
                float y2, r2, g2, b2, r3, g3, b3;
                if (xx >= width || yy >= height)
                    continue;
                y2 = sample_y8(src_y, src_y_pitch, xx, yy, full);
                yuv_to_rgb_bt709(y2, U, V, &r2, &g2, &b2);
                curves_rgb8(r2, g2, b2, lut_r, lut_g, lut_b, &r3, &g3, &b3);
                ar += r3;
                ag += g3;
                ab += b3;
                count++;
            }
        }
        if (count > 0) {
            float inv = 1.0f / (float)count;
            ar *= inv;
            ag *= inv;
            ab *= inv;
        }
        rgb_to_yuv_bt709(ar, ag, ab, &Yo, &Uo, &Vo);
        store_uv8(dst_uv, dst_uv_pitch, ux, uy, Uo, Vo, full);
    }
}

/**
 * P010 fused curves (10-bit left-aligned in 16-bit container).
 */
__global__ void curves_p010(const unsigned short *src_y, const ushort2 *src_uv,
                            unsigned short *dst_y, ushort2 *dst_uv,
                            int src_y_pitch, int src_uv_pitch,
                            int dst_y_pitch, int dst_uv_pitch,
                            int width, int height,
                            const unsigned short *lut_r,
                            const unsigned short *lut_g,
                            const unsigned short *lut_b,
                            int full)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const int ux = x >> 1;
    const int uy = y >> 1;
    float U, V, Yn, R, G, B, Ro, Go, Bo, Yo, Uo, Vo;

    sample_uv10(src_uv, src_uv_pitch, ux, uy, full, &U, &V);
    Yn = sample_y10(src_y, src_y_pitch, x, y, full);
    yuv_to_rgb_bt709(Yn, U, V, &R, &G, &B);
    curves_rgb10(R, G, B, lut_r, lut_g, lut_b, &Ro, &Go, &Bo);
    rgb_to_yuv_bt709(Ro, Go, Bo, &Yo, &Uo, &Vo);
    store_y10(dst_y, dst_y_pitch, x, y, Yo, full);

    if ((x & 1) == 0 && (y & 1) == 0) {
        float ar = 0.0f, ag = 0.0f, ab = 0.0f;
        int count = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int xx = x + dx, yy = y + dy;
                float y2, r2, g2, b2, r3, g3, b3;
                if (xx >= width || yy >= height)
                    continue;
                y2 = sample_y10(src_y, src_y_pitch, xx, yy, full);
                yuv_to_rgb_bt709(y2, U, V, &r2, &g2, &b2);
                curves_rgb10(r2, g2, b2, lut_r, lut_g, lut_b, &r3, &g3, &b3);
                ar += r3;
                ag += g3;
                ab += b3;
                count++;
            }
        }
        if (count > 0) {
            float inv = 1.0f / (float)count;
            ar *= inv;
            ag *= inv;
            ab *= inv;
        }
        rgb_to_yuv_bt709(ar, ag, ab, &Yo, &Uo, &Vo);
        store_uv10(dst_uv, dst_uv_pitch, ux, uy, Uo, Vo, full);
    }
}

}
