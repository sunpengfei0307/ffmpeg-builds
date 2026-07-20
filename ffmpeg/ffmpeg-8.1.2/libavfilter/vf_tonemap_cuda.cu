/*
 * CUDA bidirectional tonemap filter kernels
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

#include "vf_tonemap_cuda.h"

#define REFERENCE_WHITE 100.0f

static __device__ float3 make_f3(float x, float y, float z)
{
    float3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

static __device__ float3 mul3x3(const float *m, float3 v)
{
    return make_f3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                   m[3] * v.x + m[4] * v.y + m[5] * v.z,
                   m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

static __device__ float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static __device__ float hable_f(float in)
{
    const float a = 0.15f, b = 0.50f, c = 0.10f, d = 0.20f, e = 0.02f, f = 0.30f;
    return (in * (in * a + b * c) + d * e) / (in * (in * a + b) + d * f) - e / f;
}

static __device__ float eotf_st2084(float x)
{
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    float p = powf(fmaxf(x, 0.0f), 1.0f / m2);
    float num = fmaxf(p - c1, 0.0f);
    float den = c2 - c3 * p;
    return powf(num / fmaxf(den, 1e-6f), 1.0f / m1) * 10000.0f / REFERENCE_WHITE;
}

static __device__ float oetf_st2084(float L)
{
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    float Y = fmaxf(L * REFERENCE_WHITE / 10000.0f, 0.0f);
    float Ym = powf(Y, m1);
    return powf((c1 + c2 * Ym) / (1.0f + c3 * Ym), m2);
}

static __device__ float eotf_bt1886(float x)
{
    return powf(fmaxf(x, 0.0f), 2.4f);
}

static __device__ float oetf_bt1886(float x)
{
    return powf(fmaxf(x, 0.0f), 1.0f / 2.4f);
}

static __device__ float eotf_srgb(float x)
{
    x = clampf(x, 0.0f, 1.0f);
    return x < 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

static __device__ float oetf_srgb(float x)
{
    x = fmaxf(x, 0.0f);
    return x < 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

/* HLG inverse OETF -> scene linear, then approximate display linear */
static __device__ float inverse_oetf_hlg(float x)
{
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;
    x = clampf(x, 0.0f, 1.0f);
    return x <= 0.5f ? (x * x) / 3.0f : (expf((x - c) / a) + b) / 12.0f;
}

static __device__ float oetf_hlg(float x)
{
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;
    x = fmaxf(x, 0.0f);
    return x <= 1.0f / 12.0f ? sqrtf(3.0f * x) : a * logf(12.0f * x - b) + c;
}

static __device__ float3 apply_trc_in(float3 c, int trc)
{
    switch (trc) {
    case TONEMAP_CUDA_TRC_PQ:        return make_f3(eotf_st2084(c.x), eotf_st2084(c.y), eotf_st2084(c.z));
    case TONEMAP_CUDA_TRC_HLG:       return make_f3(inverse_oetf_hlg(c.x), inverse_oetf_hlg(c.y), inverse_oetf_hlg(c.z));
    case TONEMAP_CUDA_TRC_SRGB:      return make_f3(eotf_srgb(c.x), eotf_srgb(c.y), eotf_srgb(c.z));
    case TONEMAP_CUDA_TRC_BT2020_10:
    case TONEMAP_CUDA_TRC_BT709:
    default:                         return make_f3(eotf_bt1886(c.x), eotf_bt1886(c.y), eotf_bt1886(c.z));
    }
}

static __device__ float3 apply_trc_out(float3 c, int trc)
{
    switch (trc) {
    case TONEMAP_CUDA_TRC_PQ:        return make_f3(oetf_st2084(c.x), oetf_st2084(c.y), oetf_st2084(c.z));
    case TONEMAP_CUDA_TRC_HLG:       return make_f3(oetf_hlg(c.x), oetf_hlg(c.y), oetf_hlg(c.z));
    case TONEMAP_CUDA_TRC_SRGB:      return make_f3(oetf_srgb(c.x), oetf_srgb(c.y), oetf_srgb(c.z));
    case TONEMAP_CUDA_TRC_BT2020_10:
    case TONEMAP_CUDA_TRC_BT709:
    default:                         return make_f3(oetf_bt1886(c.x), oetf_bt1886(c.y), oetf_bt1886(c.z));
    }
}

static __device__ float tonemap_forward(float s, float peak, int algo, float param)
{
    switch (algo) {
    case TONEMAP_CUDA_NONE:     return s;
    case TONEMAP_CUDA_LINEAR:   return s * param / peak;
    case TONEMAP_CUDA_GAMMA: {
        float p = s > 0.05f ? s / peak : 0.05f / peak;
        float v = powf(p, 1.0f / param);
        return s > 0.05f ? v : (s * v / 0.05f);
    }
    case TONEMAP_CUDA_CLIP:     return clampf(s * param, 0.0f, 1.0f);
    case TONEMAP_CUDA_REINHARD: return s / (s + param) * (peak + param) / peak;
    case TONEMAP_CUDA_HABLE:    return hable_f(s) / hable_f(peak);
    case TONEMAP_CUDA_MOBIUS: {
        float j = param, a, b;
        if (s <= j)
            return s;
        a = -j * j * (peak - 1.0f) / (j * j - 2.0f * j + peak);
        b = (j * j - 2.0f * j * peak + peak) / fmaxf(peak - 1.0f, 1e-6f);
        return (b * b + 2.0f * b * j + j * j) / (b - a) * (s + a) / (s + b);
    }
    default: return s;
    }
}

/* Approximate inverse for sdr2hdr */
static __device__ float tonemap_inverse(float s, float peak, int algo, float param)
{
    s = clampf(s, 0.0f, 1.0f);
    switch (algo) {
    case TONEMAP_CUDA_NONE:
        return s;
    case TONEMAP_CUDA_LINEAR:
        return s * peak / fmaxf(param, 1e-6f);
    case TONEMAP_CUDA_GAMMA:
        return powf(s, param) * peak;
    case TONEMAP_CUDA_REINHARD: {
        /* s = x/(x+p) * (peak+p)/peak  => x = s*p*peak / ((peak+p) - s*peak) */
        float den = (peak + param) - s * peak;
        return s * param * peak / fmaxf(den, 1e-6f);
    }
    case TONEMAP_CUDA_HABLE: {
        /* Newton inverse of hable_f(x)/hable_f(peak) */
        float target = s * hable_f(peak);
        float x = s * peak;
        for (int i = 0; i < 8; i++) {
            float y = hable_f(x) - target;
            float xp = x + 1e-3f;
            float yp = hable_f(xp) - target;
            float d = (yp - y) / 1e-3f;
            if (fabsf(d) < 1e-8f)
                break;
            x = fmaxf(x - y / d, 0.0f);
        }
        return x;
    }
    default:
        return s * peak;
    }
}

static __device__ float3 map_rgb(float3 rgb, const TonemapCUDAParams *p)
{
    float sig = fmaxf(fmaxf(rgb.x, fmaxf(rgb.y, rgb.z)), 1e-6f);
    float peak = fmaxf(p->peak, 1.0f);
    float sig_old = sig;

    if (p->desat > 0.0f) {
        float luma = p->luma_dst[0] * rgb.x + p->luma_dst[1] * rgb.y + p->luma_dst[2] * rgb.z;
        float coeff = fmaxf(sig - 0.18f, 1e-6f) / fmaxf(sig, 1e-6f);
        coeff = powf(coeff, 10.0f / p->desat);
        rgb.x = rgb.x * (1.0f - coeff) + luma * coeff;
        rgb.y = rgb.y * (1.0f - coeff) + luma * coeff;
        rgb.z = rgb.z * (1.0f - coeff) + luma * coeff;
        sig = fmaxf(fmaxf(rgb.x, fmaxf(rgb.y, rgb.z)), 1e-6f);
        sig_old = sig;
    }

    if (p->mode == TONEMAP_CUDA_MODE_SDR2HDR) {
        sig = tonemap_inverse(sig / fmaxf(p->target_peak, 1e-6f), peak, p->tonemap, p->param);
        /* scale into HDR linear relative to REFERENCE_WHITE units already in eotf */
        rgb.x *= sig / sig_old;
        rgb.y *= sig / sig_old;
        rgb.z *= sig / sig_old;
    } else {
        float tpeak = fmaxf(p->target_peak, 1.0f);
        if (tpeak > 1.0f) {
            sig /= tpeak;
            peak /= tpeak;
        }
        sig = tonemap_forward(sig, peak, p->tonemap, p->param);
        sig = fminf(sig, 1.0f);
        rgb.x *= sig / sig_old;
        rgb.y *= sig / sig_old;
        rgb.z *= sig / sig_old;
    }
    return rgb;
}

static __device__ float sample_y_norm(const void *src_y, int pitch, int x, int y,
                                      int bit_depth, int full)
{
    if (bit_depth > 8) {
        unsigned short v = ((const unsigned short *)src_y)[y * pitch + x] >> 6;
        if (full)
            return v / 1023.0f;
        return clampf((v - 64.0f) / 876.0f, 0.0f, 1.0f);
    } else {
        unsigned char v = ((const unsigned char *)src_y)[y * pitch + x];
        if (full)
            return v / 255.0f;
        return clampf((v - 16.0f) / 219.0f, 0.0f, 1.0f);
    }
}

static __device__ void sample_uv_norm(const void *src_uv, int pitch, int x, int y,
                                      int bit_depth, int full, float *u, float *v)
{
    if (bit_depth > 8) {
        ushort2 uv = ((const ushort2 *)src_uv)[y * pitch + x];
        float U = (uv.x >> 6) / 1023.0f;
        float V = (uv.y >> 6) / 1023.0f;
        if (full) {
            *u = U - 0.5f;
            *v = V - 0.5f;
        } else {
            *u = ((uv.x >> 6) - 512.0f) / 896.0f;
            *v = ((uv.y >> 6) - 512.0f) / 896.0f;
        }
    } else {
        uchar2 uv = ((const uchar2 *)src_uv)[y * pitch + x];
        if (full) {
            *u = uv.x / 255.0f - 0.5f;
            *v = uv.y / 255.0f - 0.5f;
        } else {
            *u = (uv.x - 128.0f) / 224.0f;
            *v = (uv.y - 128.0f) / 224.0f;
        }
    }
}

static __device__ void store_y(void *dst_y, int pitch, int x, int y,
                               float Y, int bit_depth, int full)
{
    Y = clampf(Y, 0.0f, 1.0f);
    if (bit_depth > 8) {
        unsigned short v;
        if (full)
            v = (unsigned short)(Y * 1023.0f + 0.5f) << 6;
        else
            v = (unsigned short)(Y * 876.0f + 64.0f + 0.5f) << 6;
        ((unsigned short *)dst_y)[y * pitch + x] = v;
    } else {
        unsigned char v;
        if (full)
            v = (unsigned char)(Y * 255.0f + 0.5f);
        else
            v = (unsigned char)(Y * 219.0f + 16.0f + 0.5f);
        ((unsigned char *)dst_y)[y * pitch + x] = v;
    }
}

static __device__ void store_uv(void *dst_uv, int pitch, int x, int y,
                                float U, float V, int bit_depth, int full)
{
    if (bit_depth > 8) {
        ushort2 out;
        if (full) {
            out.x = (unsigned short)(clampf(U + 0.5f, 0.0f, 1.0f) * 1023.0f + 0.5f) << 6;
            out.y = (unsigned short)(clampf(V + 0.5f, 0.0f, 1.0f) * 1023.0f + 0.5f) << 6;
        } else {
            out.x = (unsigned short)(clampf(U * 896.0f + 512.0f, 0.0f, 1023.0f) + 0.5f) << 6;
            out.y = (unsigned short)(clampf(V * 896.0f + 512.0f, 0.0f, 1023.0f) + 0.5f) << 6;
        }
        ((ushort2 *)dst_uv)[y * pitch + x] = out;
    } else {
        uchar2 out;
        if (full) {
            out.x = (unsigned char)(clampf(U + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            out.y = (unsigned char)(clampf(V + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        } else {
            out.x = (unsigned char)(clampf(U * 224.0f + 128.0f, 0.0f, 255.0f) + 0.5f);
            out.y = (unsigned char)(clampf(V * 224.0f + 128.0f, 0.0f, 255.0f) + 0.5f);
        }
        ((uchar2 *)dst_uv)[y * pitch + x] = out;
    }
}

static __device__ void process_pixel(float3 yuv, const TonemapCUDAParams *p, float3 *out_yuv)
{
    float3 rgb = mul3x3(p->yuv2rgb, yuv);
    rgb = apply_trc_in(rgb, p->trc_in);
    if (!p->rgb2rgb_passthrough)
        rgb = mul3x3(p->rgb2rgb, rgb);
    rgb = map_rgb(rgb, p);
    rgb = apply_trc_out(rgb, p->trc_out);
    *out_yuv = mul3x3(p->rgb2yuv, rgb);
}

extern "C" {

__global__ void tonemap_cuda(const void *src_y, const void *src_uv,
                             void *dst_y, void *dst_uv,
                             int src_y_pitch, int src_uv_pitch,
                             int dst_y_pitch, int dst_uv_pitch,
                             int width, int height,
                             TonemapCUDAParams p)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    int ux = x >> 1;
    int uy = y >> 1;
    float U, V;
    sample_uv_norm(src_uv, src_uv_pitch, ux, uy, p.src_bit_depth, p.full_range_in, &U, &V);

    float Yn = sample_y_norm(src_y, src_y_pitch, x, y, p.src_bit_depth, p.full_range_in);
    float3 yuv = make_f3(Yn, U, V);
    float3 out;
    process_pixel(yuv, &p, &out);
    store_y(dst_y, dst_y_pitch, x, y, out.x, p.dst_bit_depth, p.full_range_out);

    /* chroma: average 2x2 contributions from the top-left of each UV block */
    if ((x & 1) == 0 && (y & 1) == 0) {
        float3 acc = make_f3(0, 0, 0);
        int count = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int xx = x + dx;
                int yy = y + dy;
                if (xx >= width || yy >= height)
                    continue;
                float y2 = sample_y_norm(src_y, src_y_pitch, xx, yy, p.src_bit_depth, p.full_range_in);
                float3 yuv2 = make_f3(y2, U, V);
                float3 o2;
                process_pixel(yuv2, &p, &o2);
                acc.y += o2.y;
                acc.z += o2.z;
                count++;
            }
        }
        if (count > 0)
            store_uv(dst_uv, dst_uv_pitch, ux, uy, acc.y / count, acc.z / count,
                     p.dst_bit_depth, p.full_range_out);
    }
}

}
