/*
 * CUDA kernels for HLG to PQ conversion (vf_hlg2pq_cuda).
 *
 * Per sample: P010 limited-range YCbCr -> linear BT.2020 RGB -> HLG scene
 * linear (inverse OETF) -> HLG OOTF scaled to peak_luminance -> PQ non-linear
 * (ST 2084 inverse EOTF) -> BT.2020 RGB -> limited-range YCbCr.
 *
 * Chroma: each 4:2:0 UV texel averages four converted luma-neighbourhood
 * samples (see hlg2pq_uv) to reduce subsampling artefacts.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "cuda/vector_helpers.cuh"

/* ARIB STD-B67 / BT.2100 HLG inverse OETF coefficients. */
#define HLG_A 0.17883277f
#define HLG_B 0.28466892f
#define HLG_C 0.55991073f

/* SMPTE ST 2084 (PQ) EOTF constants. */
#define PQ_M1 0.1593017578125f
#define PQ_M2 78.84375f
#define PQ_C1 0.8359375f
#define PQ_C2 18.8515625f
#define PQ_C3 18.6875f

#define REFERENCE_WHITE 100.0f       /* HLG nominal diffuse white (nits) */
#define ST2084_MAX_LUMINANCE 10000.0f

/* BT.2020 luma/chroma derivation coefficients. */
#define KR 0.2627f
#define KB 0.0593f
#define KG (1.0f - KR - KB)

__device__ inline float clampf(float v, float lo, float hi)
{
    return fminf(fmaxf(v, lo), hi);
}

/* Map stored P010/P016 samples to [0,1] (10-bit lives in MSBs of uint16). */
__device__ inline float normalize_raw(unsigned short val, int bit_depth)
{
    if (bit_depth == 10)
        return (float)(val >> 6) / 1023.0f;
    return (float)val / (float)((1 << bit_depth) - 1);
}

__device__ inline unsigned short denormalize_raw(float val, int bit_depth)
{
    if (bit_depth == 10) {
        int code = (int)(clampf(val, 0.0f, 1.0f) * 1023.0f + 0.5f);
        return (unsigned short)(code << 6);
    }
    float scale = (float)((1 << bit_depth) - 1);
    return (unsigned short)(clampf(val, 0.0f, 1.0f) * scale + 0.5f);
}

/* BT.2020 10-bit limited range (studio swing) in normalized [0,1] domain. */
__device__ inline float luma_limited_to_signal(float raw)
{
    const float black = 64.0f / 1023.0f;
    const float white = 940.0f / 1023.0f;
    return clampf((raw - black) / (white - black), 0.0f, 1.0f);
}

__device__ inline float luma_signal_to_limited(float sig)
{
    const float black = 64.0f / 1023.0f;
    const float white = 940.0f / 1023.0f;
    return black + clampf(sig, 0.0f, 1.0f) * (white - black);
}

__device__ inline float chroma_limited_to_signal(float raw)
{
    const float mid = 512.0f / 1023.0f;
    const float range = 896.0f / 1023.0f;
    return clampf((raw - mid) / range, -0.5f, 0.5f);
}

__device__ inline float chroma_signal_to_limited(float sig)
{
    const float mid = 512.0f / 1023.0f;
    const float range = 896.0f / 1023.0f;
    return mid + clampf(sig, -0.5f, 0.5f) * range;
}

__device__ inline void ycbcr_to_rgb(float y, float cb, float cr,
                                    float *r, float *g, float *b)
{
    *r = y + 2.0f * (1.0f - KR) * cr;
    *b = y + 2.0f * (1.0f - KB) * cb;
    *g = (y - KR * (*r) - KB * (*b)) / KG;
}

__device__ inline void rgb_to_ycbcr(float r, float g, float b,
                                    float *y, float *cb, float *cr)
{
    *y = KR * r + KG * g + KB * b;
    *cb = (b - *y) / (2.0f * (1.0f - KB));
    *cr = (r - *y) / (2.0f * (1.0f - KR));
}

/* HLG OETF^-1: non-linear HLG signal -> scene-linear light. */
__device__ inline float inverse_oetf_hlg(float x)
{
    float a = 4.0f * x * x;
    float b = expf((x - HLG_C) / HLG_A) + HLG_B;
    return x < 0.5f ? a : b;
}

/* PQ EOTF^-1: absolute luminance (nits) -> non-linear PQ code [0,1]. */
__device__ inline float inverse_eotf_st2084(float nits)
{
    float y = clampf(nits / ST2084_MAX_LUMINANCE, 0.0f, 1.0f);
    float y_m1 = powf(y, PQ_M1);
    return powf((PQ_C1 + PQ_C2 * y_m1) / (1.0f + PQ_C3 * y_m1), PQ_M2);
}

/*
 * HLG RGB (non-linear) -> PQ RGB (non-linear), including HLG OOTF with
 * luminance-dependent gamma and scaling to peak_luminance (mastering target).
 */
__device__ inline void hlg_to_pq_rgb(float rh, float gh, float bh,
                                     float *rp, float *gp, float *bp,
                                     float peak_luminance)
{
    float rs = inverse_oetf_hlg(clampf(rh, 0.0f, 1.0f));
    float gs = inverse_oetf_hlg(clampf(gh, 0.0f, 1.0f));
    float bs = inverse_oetf_hlg(clampf(bh, 0.0f, 1.0f));
    float ys = KR * rs + KG * gs + KB * bs;
    float gamma = 1.2f + 0.42f * log10f(peak_luminance / 1000.0f);
    float factor;

    gamma = fmaxf(gamma, 1.0f);
    factor = ys > 0.0f ? (peak_luminance / REFERENCE_WHITE) *
                         powf(ys, gamma - 1.0f) / powf(12.0f, gamma)
                       : 0.0f;

    *rp = inverse_eotf_st2084(rs * factor * REFERENCE_WHITE);
    *gp = inverse_eotf_st2084(gs * factor * REFERENCE_WHITE);
    *bp = inverse_eotf_st2084(bs * factor * REFERENCE_WHITE);
}

/* Full colour transform for one (x,y) luma coordinate. */
__device__ inline void convert_sample(cudaTextureObject_t y_tex,
                                      cudaTextureObject_t uv_tex,
                                      int x, int y, int bit_depth,
                                      float peak_luminance,
                                      float *yp, float *cbp, float *crp)
{
    unsigned short y_raw = tex2D<unsigned short>(y_tex, x, y);
    ushort2 uv_raw = tex2D<ushort2>(uv_tex, x >> 1, y >> 1);
    float y_sig = luma_limited_to_signal(normalize_raw(y_raw, bit_depth));
    float cb_sig = chroma_limited_to_signal(normalize_raw(uv_raw.x, bit_depth));
    float cr_sig = chroma_limited_to_signal(normalize_raw(uv_raw.y, bit_depth));
    float rh, gh, bh, rp, gp, bp;

    ycbcr_to_rgb(y_sig, cb_sig, cr_sig, &rh, &gh, &bh);
    hlg_to_pq_rgb(rh, gh, bh, &rp, &gp, &bp, peak_luminance);
    rgb_to_ycbcr(rp, gp, bp, yp, cbp, crp);
}

/* One thread per output luma sample. */
extern "C" __global__ void hlg2pq_y(cudaTextureObject_t src_y_tex,
                                    cudaTextureObject_t src_uv_tex,
                                    unsigned short *dst,
                                    int src_width, int src_height,
                                    int dst_width, int dst_height,
                                    int dst_pitch,
                                    float peak_luminance, int bit_depth,
                                    int limited_range)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    float yp, cbp, crp;

    if (x >= dst_width || y >= dst_height)
        return;

    convert_sample(src_y_tex, src_uv_tex,
                   min(x, src_width - 1), min(y, src_height - 1),
                   bit_depth, peak_luminance, &yp, &cbp, &crp);

    dst[y * (dst_pitch / sizeof(unsigned short)) + x] =
        denormalize_raw(limited_range ? luma_signal_to_limited(yp)
                                      : clampf(yp, 0.0f, 1.0f),
                        bit_depth);
}

/*
 * One thread per 4:2:0 chroma site: average CB/CR from a 2x2 luma block so
 * chroma is consistent with independently converted luma samples.
 */
extern "C" __global__ void hlg2pq_uv(cudaTextureObject_t src_y_tex,
                                     cudaTextureObject_t src_uv_tex,
                                     unsigned short *dst,
                                     int src_width, int src_height,
                                     int dst_width, int dst_height,
                                     int dst_pitch,
                                     float peak_luminance, int bit_depth,
                                     int limited_range)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    float cb_sum = 0.0f, cr_sum = 0.0f;
    int i;
    ushort2 out;
    ushort2 *dst_uv;

    if (x >= dst_width || y >= dst_height)
        return;

    for (i = 0; i < 4; i++) { /* four luma positions under this UV cell */
        int lx = min((x << 1) + (i & 1), src_width - 1);
        int ly = min((y << 1) + (i >> 1), src_height - 1);
        float yp, cbp, crp;

        convert_sample(src_y_tex, src_uv_tex, lx, ly, bit_depth,
                       peak_luminance, &yp, &cbp, &crp);
        cb_sum += cbp;
        cr_sum += crp;
    }

    out.x = denormalize_raw(limited_range ? chroma_signal_to_limited(cb_sum * 0.25f)
                                          : clampf(cb_sum * 0.25f + 0.5f, 0.0f, 1.0f),
                            bit_depth);
    out.y = denormalize_raw(limited_range ? chroma_signal_to_limited(cr_sum * 0.25f)
                                          : clampf(cr_sum * 0.25f + 0.5f, 0.0f, 1.0f),
                            bit_depth);

    dst_uv = (ushort2 *)(dst + y * (dst_pitch / sizeof(unsigned short)));
    dst_uv[x] = out;
}
