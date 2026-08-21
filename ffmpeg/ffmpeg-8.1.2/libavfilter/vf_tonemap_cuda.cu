/*
 * CUDA bidirectional tonemap kernels (libplacebo-inspired).
 *
 * This file is part of FFmpeg.
 */

#include "vf_tonemap_cuda.h"

#define REFERENCE_WHITE 100.0f
#define PQ_MAX_NITS     10000.0f

static __device__ float3 make_f3(float x, float y, float z)
{
    float3 v;
    v.x = x;
    v.y = y;
    v.z = z;
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

static __device__ float mixf(float a, float b, float t)
{
    return a * (1.0f - t) + b * t;
}

/* ---- Transfer: PQ (ST 2084) ---- */
static __device__ float eotf_st2084(float x)
{
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    float p = powf(fmaxf(x, 0.0f), 1.0f / m2);
    float num = fmaxf(p - c1, 0.0f);
    float den = c2 - c3 * p;
    return powf(num / fmaxf(den, 1e-6f), 1.0f / m1) * PQ_MAX_NITS / REFERENCE_WHITE;
}

static __device__ float oetf_st2084(float L)
{
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    float Y = fmaxf(L * REFERENCE_WHITE / PQ_MAX_NITS, 0.0f);
    float Ym = powf(Y, m1);
    return powf((c1 + c2 * Ym) / (1.0f + c3 * Ym), m2);
}

/* Relative linear <-> PQ code value helpers for BT.2390 */
static __device__ float linear_to_pq(float L)
{
    return oetf_st2084(L);
}

static __device__ float pq_to_linear(float e)
{
    return eotf_st2084(e);
}

/* ---- Transfer: BT.1886 / sRGB ---- */
static __device__ float eotf_bt1886(float x) { return powf(fmaxf(x, 0.0f), 2.4f); }
static __device__ float oetf_bt1886(float x) { return powf(fmaxf(x, 0.0f), 1.0f / 2.4f); }

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

/* ---- Transfer: HLG (ARIB STD-B67) with OOTF ---- */
static __device__ float inverse_oetf_hlg(float x)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    x = clampf(x, 0.0f, 1.0f);
    return x <= 0.5f ? (x * x) / 3.0f : (expf((x - c) / a) + b) / 12.0f;
}

static __device__ float oetf_hlg(float x)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    x = fmaxf(x, 0.0f);
    return x <= 1.0f / 12.0f ? sqrtf(3.0f * x) : a * logf(12.0f * x - b) + c;
}

/* BT.2100 HLG OOTF: scene-linear -> display-linear */
static __device__ float3 hlg_ootf(float3 rgb, float peak_nits)
{
    float Ys = 0.2627f * rgb.x + 0.6780f * rgb.y + 0.0593f * rgb.z;
    float gamma = 1.2f + 0.42f * log10f(fmaxf(peak_nits, 1000.0f) / 1000.0f);
    float scale = Ys > 1e-6f ? powf(Ys, gamma - 1.0f) : 0.0f;
    /* Map nominal peak to relative luminance peak_nits/REFERENCE_WHITE */
    float gain = (peak_nits / REFERENCE_WHITE);
    return make_f3(rgb.x * scale * gain, rgb.y * scale * gain, rgb.z * scale * gain);
}

static __device__ float3 hlg_inverse_ootf(float3 rgb, float peak_nits)
{
    float Yd = 0.2627f * rgb.x + 0.6780f * rgb.y + 0.0593f * rgb.z;
    float gain = peak_nits / REFERENCE_WHITE;
    float Ys = Yd / fmaxf(gain, 1e-6f);
    float gamma = 1.2f + 0.42f * log10f(fmaxf(peak_nits, 1000.0f) / 1000.0f);
    float scale = Ys > 1e-6f ? powf(Ys, (1.0f / gamma) - 1.0f) : 0.0f;
    return make_f3(rgb.x * scale / gain, rgb.y * scale / gain, rgb.z * scale / gain);
}

static __device__ float3 apply_trc_in(float3 c, int trc, float hlg_peak_nits)
{
    switch (trc) {
    case TONEMAP_CUDA_TRC_PQ:
        return make_f3(eotf_st2084(c.x), eotf_st2084(c.y), eotf_st2084(c.z));
    case TONEMAP_CUDA_TRC_HLG: {
        float3 sc = make_f3(inverse_oetf_hlg(c.x), inverse_oetf_hlg(c.y), inverse_oetf_hlg(c.z));
        return hlg_ootf(sc, hlg_peak_nits);
    }
    case TONEMAP_CUDA_TRC_SRGB:
        return make_f3(eotf_srgb(c.x), eotf_srgb(c.y), eotf_srgb(c.z));
    case TONEMAP_CUDA_TRC_BT2020_10:
    case TONEMAP_CUDA_TRC_BT709:
    default:
        return make_f3(eotf_bt1886(c.x), eotf_bt1886(c.y), eotf_bt1886(c.z));
    }
}

static __device__ float3 apply_trc_out(float3 c, int trc, float hlg_peak_nits)
{
    switch (trc) {
    case TONEMAP_CUDA_TRC_PQ:
        return make_f3(oetf_st2084(c.x), oetf_st2084(c.y), oetf_st2084(c.z));
    case TONEMAP_CUDA_TRC_HLG: {
        float3 sc = hlg_inverse_ootf(c, hlg_peak_nits);
        return make_f3(oetf_hlg(sc.x), oetf_hlg(sc.y), oetf_hlg(sc.z));
    }
    case TONEMAP_CUDA_TRC_SRGB:
        return make_f3(oetf_srgb(c.x), oetf_srgb(c.y), oetf_srgb(c.z));
    case TONEMAP_CUDA_TRC_BT2020_10:
    case TONEMAP_CUDA_TRC_BT709:
    default:
        return make_f3(oetf_bt1886(c.x), oetf_bt1886(c.y), oetf_bt1886(c.z));
    }
}

static __device__ float hable_f(float in)
{
    /* Identical coefficients to libplacebo / Uncharted2. */
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((in * (A * in + C * B) + D * E) / (in * (A * in + B) + D * F)) - E / F;
}

/* BT.2390 EETF (hermite roll-off in PQ domain).
 * L/src_peak/dst_peak are relative linear (nits/100). Returns relative linear. */
static __device__ float tonemap_bt2390(float L, float src_peak, float dst_peak, float knee_offset)
{
    float src_pq = fmaxf(linear_to_pq(src_peak), 1e-6f);
    float dst_pq = fmaxf(linear_to_pq(dst_peak), 1e-6f);
    float max_lum = clampf(dst_pq / src_pq, 1e-3f, 1.0f);
    /* Spec-ish knee; keep away from 0/1 to avoid hermite blow-ups. */
    float ks = clampf((1.5f * max_lum - 0.5f) * fmaxf(knee_offset, 0.5f),
                      0.1f * max_lum, 0.95f * max_lum);
    float x = clampf(linear_to_pq(fmaxf(L, 0.0f)) / src_pq, 0.0f, 1.0f);
    float e;

    if (x < ks) {
        e = x;
    } else {
        float t = (x - ks) / fmaxf(1.0f - ks, 1e-6f);
        float t2 = t * t, t3 = t2 * t;
        /* Hermite from (ks,ks) to (1,max_lum) with end slope 0. */
        e = (2.0f * t3 - 3.0f * t2 + 1.0f) * ks +
            (-2.0f * t3 + 3.0f * t2) * max_lum;
    }
    e = clampf(e, 0.0f, max_lum);
    return pq_to_linear(e * src_pq);
}

/* Single-pivot spline (libplacebo pl_tone_map_spline approximation). */
static __device__ float tonemap_spline(float s, float src_peak, float dst_peak,
                                       float knee, float contrast)
{
    float pivot = clampf(knee, 0.05f, 0.95f) * fminf(src_peak, dst_peak);
    float slope = mixf(1.0f, dst_peak / fmaxf(src_peak, 1e-6f), contrast);
    if (s <= pivot)
        return s * slope;
    float t = (s - pivot) / fmaxf(src_peak - pivot, 1e-6f);
    float y0 = pivot * slope;
    float y1 = dst_peak;
    /* Smoothstep blend to dst peak */
    float u = t * t * (3.0f - 2.0f * t);
    return mixf(y0, y1, u);
}

/* BT.2446a-ish: power mapping between peaks (works both directions). */
static __device__ float tonemap_bt2446a(float s, float src_peak, float dst_peak)
{
    float a = logf(fmaxf(dst_peak, 1e-6f)) / logf(fmaxf(src_peak, 1e-6f));
    return powf(fmaxf(s, 0.0f), a);
}

/*
 * Forward tonemap aligned with vf_tonemap / tonemap_opencl:
 * algorithms produce a display-relative factor in ~[0,1] (1 = dst peak),
 * then multiplied by dst_peak for absolute relative luminance.
 */
static __device__ float tonemap_forward(float s, const TonemapCUDAParams *p)
{
    float src_peak = fmaxf(p->src_peak, 1e-6f);
    float dst_peak = fmaxf(p->dst_peak, 1e-6f);
    int algo = p->tonemap;
    float out01;

    if (algo == TONEMAP_CUDA_AUTO)
        algo = TONEMAP_CUDA_SPLINE;

    s = fmaxf(s * p->exposure, 0.0f);

    switch (algo) {
    case TONEMAP_CUDA_NONE:
        out01 = clampf(s / src_peak, 0.0f, 1.0f);
        break;
    case TONEMAP_CUDA_CLIP: {
        /*
         * libplacebo pl_tone_map_clip: identity curve in PQ between peaks.
         * Linear-light clamp(s,0,1) (CPU vf_tonemap) leaves HDR midtones
         * unmapped and turns near-white stage lights magenta after max-RGB
         * rescale — wrong for PQ/HLG→SDR. param scales the PQ axis (1=default).
         */
        float param = p->param;
        float src_pq, dst_pq, e;
        if (!(param > 1e-6f) || !(param < 1e6f))
            param = 1.0f;
        src_pq = fmaxf(linear_to_pq(src_peak), 1e-6f);
        dst_pq = fmaxf(linear_to_pq(dst_peak), 1e-6f);
        e = clampf(linear_to_pq(s) * param, 0.0f, src_pq);
        return fmaxf(pq_to_linear(e / src_pq * dst_pq), 0.0f);
    }
    case TONEMAP_CUDA_LINEAR:
        out01 = clampf(s * p->param / src_peak, 0.0f, 1.0f);
        break;
    case TONEMAP_CUDA_GAMMA: {
        float n = s > 0.05f ? s / src_peak : 0.05f / src_peak;
        float v = powf(fmaxf(n, 0.0f), 1.0f / fmaxf(p->param, 1e-3f));
        out01 = s > 0.05f ? v : (s * v / 0.05f);
        out01 = clampf(out01, 0.0f, 1.0f);
        break;
    }
    case TONEMAP_CUDA_REINHARD: {
        float c = fmaxf(p->reinhard_contrast, 1e-3f);
        float k = (1.0f - c) / c;
        out01 = clampf(s / (s + k) * (src_peak + k) / src_peak, 0.0f, 1.0f);
        break;
    }
    case TONEMAP_CUDA_HABLE:
        /* Classic Uncharted2 / vf_tonemap / tonemap_opencl: maps peak → 1. */
        out01 = clampf(hable_f(s) / fmaxf(hable_f(src_peak), 1e-6f), 0.0f, 1.0f);
        break;
    case TONEMAP_CUDA_MOBIUS: {
        /* Match vf_tonemap.c mobius(): absolute signal, 1.0 ≈ reference white. */
        float j = p->knee > 0.0f ? p->knee : p->param;
        float peak = src_peak;
        float a, b, m;
        if (!(j > 0.0f) || !(j < 1e6f))
            j = 0.3f;
        if (peak <= 1.0f + 1e-3f)
            return fminf(s, dst_peak);
        j = clampf(j, 1e-4f, peak - 1e-4f);
        if (s <= j)
            return fminf(s, dst_peak);
        a = -j * j * (peak - 1.0f) / fmaxf(j * j - 2.0f * j + peak, 1e-6f);
        b = (j * j - 2.0f * j * peak + peak) / fmaxf(peak - 1.0f, 1e-6f);
        m = (b * b + 2.0f * b * j + j * j) / fmaxf(b - a, 1e-6f) *
            (s + a) / fmaxf(s + b, 1e-6f);
        return clampf(m, 0.0f, dst_peak);
    }
    case TONEMAP_CUDA_BT2390:
        return fmaxf(tonemap_bt2390(s, src_peak, dst_peak, p->knee_offset), 0.0f);
    case TONEMAP_CUDA_SPLINE:
        return fmaxf(tonemap_spline(s, src_peak, dst_peak, p->knee, p->spline_contrast), 0.0f);
    case TONEMAP_CUDA_BT2446A:
        return fmaxf(tonemap_bt2446a(s, src_peak, dst_peak), 0.0f);
    default:
        out01 = clampf(hable_f(s) / fmaxf(hable_f(src_peak), 1e-6f), 0.0f, 1.0f);
        break;
    }

    return out01 * dst_peak;
}

static __device__ float tonemap_inverse(float s, const TonemapCUDAParams *p)
{
    float src_peak = fmaxf(p->src_peak, 1e-6f); /* SDR side */
    float dst_peak = fmaxf(p->dst_peak, 1e-6f); /* HDR side */
    int algo = p->tonemap;

    if (algo == TONEMAP_CUDA_AUTO)
        algo = TONEMAP_CUDA_SPLINE;

    s = clampf(s, 0.0f, src_peak);

    switch (algo) {
    case TONEMAP_CUDA_NONE:
    case TONEMAP_CUDA_LINEAR:
        return s * (dst_peak / src_peak) / fmaxf(p->param, 1e-6f) / fmaxf(p->exposure, 1e-6f);
    case TONEMAP_CUDA_GAMMA:
        return powf(s / src_peak, fmaxf(p->param, 1e-3f)) * dst_peak / fmaxf(p->exposure, 1e-6f);
    case TONEMAP_CUDA_REINHARD: {
        float c = fmaxf(p->reinhard_contrast, 1e-3f);
        float k = (1.0f - c) / c;
        float n = s / src_peak;
        return (n * k) / fmaxf(1.0f - n, 1e-6f) * (dst_peak / src_peak);
    }
    case TONEMAP_CUDA_HABLE: {
        /* Invert classic hable: hable_f(x)/hable_f(dst_peak)*src_peak ≈ s. */
        float x = s * (dst_peak / fmaxf(src_peak, 1e-6f));
        float hpeak = fmaxf(hable_f(dst_peak), 1e-6f);
        for (int i = 0; i < 8; i++) {
            float y = hable_f(x) / hpeak * src_peak - s;
            float d = (hable_f(x + 1e-3f) - hable_f(x)) / 1e-3f / hpeak * src_peak;
            if (fabsf(d) < 1e-8f)
                break;
            x = fmaxf(x - y / d, 0.0f);
        }
        return x / fmaxf(p->exposure, 1e-6f);
    }
    case TONEMAP_CUDA_BT2446A:
        return tonemap_bt2446a(s, src_peak, dst_peak) / fmaxf(p->exposure, 1e-6f);
    case TONEMAP_CUDA_SPLINE: {
        /* Approximate inverse of single-pivot spline */
        float pivot = clampf(p->knee, 0.05f, 0.95f) * fminf(src_peak, dst_peak);
        float slope = mixf(1.0f, dst_peak / src_peak, p->spline_contrast);
        float y0 = pivot * slope;
        if (s <= y0)
            return (s / fmaxf(slope, 1e-6f)) / fmaxf(p->exposure, 1e-6f);
        float u = (s - y0) / fmaxf(dst_peak - y0, 1e-6f);
        u = clampf(u, 0.0f, 1.0f);
        /* Inverse smoothstep approximation */
        float t = sqrtf(u);
        return (pivot + t * (src_peak - pivot)) / fmaxf(p->exposure, 1e-6f);
    }
    default:
        return s * (dst_peak / src_peak) / fmaxf(p->exposure, 1e-6f);
    }
}

static __device__ float3 adjust_color(float3 rgb, const TonemapCUDAParams *p)
{
    float luma = p->luma_dst[0] * rgb.x + p->luma_dst[1] * rgb.y + p->luma_dst[2] * rgb.z;
    rgb.x = (rgb.x - luma) * p->saturation + luma;
    rgb.y = (rgb.y - luma) * p->saturation + luma;
    rgb.z = (rgb.z - luma) * p->saturation + luma;
    rgb.x = rgb.x * p->contrast + p->brightness;
    rgb.y = rgb.y * p->contrast + p->brightness;
    rgb.z = rgb.z * p->contrast + p->brightness;
    return rgb;
}

static __device__ float3 repair_gamut_negatives(float3 rgb, const float *luma_c)
{
    float luma, cmin, t;

    luma = luma_c[0] * rgb.x + luma_c[1] * rgb.y + luma_c[2] * rgb.z;
    cmin = fminf(rgb.x, fminf(rgb.y, rgb.z));
    if (cmin < -1e-6f && fabsf(luma - cmin) > 1e-6f) {
        t = clampf((-cmin) / (luma - cmin), 0.0f, 1.0f);
        rgb.x = mixf(rgb.x, luma, t);
        rgb.y = mixf(rgb.y, luma, t);
        rgb.z = mixf(rgb.z, luma, t);
    }
    rgb.x = fmaxf(rgb.x, 0.0f);
    rgb.y = fmaxf(rgb.y, 0.0f);
    rgb.z = fmaxf(rgb.z, 0.0f);
    return rgb;
}

/* libplacebo PL_GAMUT_DESATURATE — mix toward luma into [0, peak]. */
static __device__ float3 soft_gamut_map(float3 rgb, float peak, const float *luma_c)
{
    float luma, cmax, t;

    rgb = repair_gamut_negatives(rgb, luma_c);

    luma = luma_c[0] * rgb.x + luma_c[1] * rgb.y + luma_c[2] * rgb.z;
    luma = clampf(luma, 0.0f, peak);
    cmax = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));

    if (cmax > peak + 1e-6f && fabsf(luma - cmax) > 1e-6f) {
        t = clampf((peak - cmax) / (luma - cmax), 0.0f, 1.0f);
        rgb.x = mixf(rgb.x, luma, t);
        rgb.y = mixf(rgb.y, luma, t);
        rgb.z = mixf(rgb.z, luma, t);
    }

    rgb.x = clampf(rgb.x, 0.0f, peak);
    rgb.y = clampf(rgb.y, 0.0f, peak);
    rgb.z = clampf(rgb.z, 0.0f, peak);
    return rgb;
}

/*
 * Hue-preserving gamut fit for perceptual/clip: repair negatives, then
 * uniform max-RGB scale into [0, peak]. Keeps LED/stage chroma that
 * desaturate-to-luma would wash out.
 */
static __device__ float3 fit_gamut_hue_preserve(float3 rgb, float peak, const float *luma_c)
{
    float cmax, s;

    rgb = repair_gamut_negatives(rgb, luma_c);
    cmax = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
    if (cmax > peak + 1e-6f) {
        s = peak / cmax;
        rgb.x *= s;
        rgb.y *= s;
        rgb.z *= s;
    }
    rgb.x = fmaxf(rgb.x, 0.0f);
    rgb.y = fmaxf(rgb.y, 0.0f);
    rgb.z = fmaxf(rgb.z, 0.0f);
    return rgb;
}

static __device__ float3 apply_gamut_desaturate(float3 rgb, const TonemapCUDAParams *p,
                                                const float *luma_c)
{
    float luma, mx, mn, strength, coeff, chroma_coeff;

    if (p->desat <= 0.0f)
        return rgb;

    luma = luma_c[0] * rgb.x + luma_c[1] * rgb.y + luma_c[2] * rgb.z;
    mx = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
    mn = fminf(rgb.x, fminf(rgb.y, rgb.z));
    strength = p->desat;

    coeff = clampf((mx - 0.35f) / 0.90f, 0.0f, 1.0f);
    coeff = coeff * coeff;
    chroma_coeff = clampf(((mx - mn) / fmaxf(mx, 1e-6f) - 0.55f) / 0.45f, 0.0f, 1.0f);
    chroma_coeff = chroma_coeff * chroma_coeff * 0.55f;
    coeff = clampf(fmaxf(coeff, chroma_coeff) * strength, 0.0f, 0.85f);
    rgb.x = mixf(rgb.x, luma, coeff);
    rgb.y = mixf(rgb.y, luma, coeff);
    rgb.z = mixf(rgb.z, luma, coeff);
    return rgb;
}

static __device__ float tonemap_signal(float s, const TonemapCUDAParams *p)
{
    float sig;
    if (p->mode == TONEMAP_CUDA_MODE_SDR2HDR)
        sig = tonemap_inverse(s, p);
    else
        sig = tonemap_forward(s, p);
    if (!(sig < 1e20f) || sig < 0.0f)
        sig = fminf(s, fmaxf(p->dst_peak, 1.0f));
    return fmaxf(sig, 0.0f);
}

/*
 * Tone-map in SOURCE primaries (VLC/libplacebo: tone_map then adapt_colors).
 * perceptual/desaturate: luma scale (hue lock). clip: max-RGB.
 */
static __device__ float3 map_rgb(float3 rgb, const TonemapCUDAParams *p)
{
    float sig, sig_old, scale, Y, mx;
    float3 out;
    const float *luma_c = p->luma_src;

    rgb.x = fmaxf(rgb.x, 0.0f);
    rgb.y = fmaxf(rgb.y, 0.0f);
    rgb.z = fmaxf(rgb.z, 0.0f);

    if (p->gamut_mode == TONEMAP_CUDA_GAMUT_DESATURATE)
        rgb = apply_gamut_desaturate(rgb, p, luma_c);

    mx = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
    Y  = luma_c[0] * rgb.x + luma_c[1] * rgb.y + luma_c[2] * rgb.z;

    if (p->gamut_mode == TONEMAP_CUDA_GAMUT_CLIP)
        sig_old = fmaxf(mx, 1e-6f);
    else
        sig_old = fmaxf(Y, 1e-6f);

    sig = tonemap_signal(sig_old, p);
    scale = sig / sig_old;
    out.x = rgb.x * scale;
    out.y = rgb.y * scale;
    out.z = rgb.z * scale;
    return out;
}

/*
 * Near-neutral magenta only (R&B both above G on gray/silver/white).
 * Do not touch chromatic reds — that washed stage banners brick-red.
 */
static __device__ float3 reduce_neutral_magenta(float3 rgb)
{
    float g = rgb.y;
    float mx = fmaxf(rgb.x, fmaxf(g, rgb.z));
    float mn = fminf(rgb.x, fminf(g, rgb.z));
    float chroma = mx - mn;
    float mag, t;

    if (chroma > 0.08f * fmaxf(mx, 1e-6f) && chroma > 0.02f)
        return rgb;

    mag = fminf(rgb.x, rgb.z) - g;
    if (mag > 0.0f) {
        t = clampf(mag * 2.5f, 0.0f, 0.55f);
        rgb.x = mixf(rgb.x, g, t);
        rgb.z = mixf(rgb.z, g, t);
    }
    return rgb;
}

static __device__ float3 process_pixel_lrgb(float3 yuv, const TonemapCUDAParams *p)
{
    float3 rgb = mul3x3(p->yuv2rgb, yuv);
    float peak = fmaxf(p->dst_peak, 1e-6f);

    rgb = apply_trc_in(rgb, p->trc_in, p->hlg_peak_nits);
    /* VLC order: tonemap in src primaries, then adapt to dst. */
    rgb = map_rgb(rgb, p);
    if (!p->rgb2rgb_passthrough)
        rgb = mul3x3(p->rgb2rgb, rgb);
    if (p->gamut_mode == TONEMAP_CUDA_GAMUT_DESATURATE)
        rgb = soft_gamut_map(rgb, peak, p->luma_dst);
    else
        rgb = fit_gamut_hue_preserve(rgb, peak, p->luma_dst);
    rgb = reduce_neutral_magenta(rgb);
    return adjust_color(rgb, p);
}

static __device__ float sample_y_norm(const void *src_y, int pitch, int x, int y,
                                      int bit_depth, int full)
{
    if (bit_depth > 8) {
        unsigned short v = ((const unsigned short *)src_y)[y * pitch + x] >> 6;
        return full ? v / 1023.0f : clampf((v - 64.0f) / 876.0f, 0.0f, 1.0f);
    }
    unsigned char v = ((const unsigned char *)src_y)[y * pitch + x];
    return full ? v / 255.0f : clampf((v - 16.0f) / 219.0f, 0.0f, 1.0f);
}

static __device__ void sample_uv_norm(const void *src_uv, int pitch, int x, int y,
                                      int bit_depth, int full, float *u, float *v)
{
    if (bit_depth > 8) {
        ushort2 uv = ((const ushort2 *)src_uv)[y * pitch + x];
        if (full) {
            *u = (uv.x >> 6) / 1023.0f - 0.5f;
            *v = (uv.y >> 6) / 1023.0f - 0.5f;
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
        unsigned short v = full
            ? (unsigned short)(Y * 1023.0f + 0.5f) << 6
            : (unsigned short)(Y * 876.0f + 64.0f + 0.5f) << 6;
        ((unsigned short *)dst_y)[y * pitch + x] = v;
    } else {
        unsigned char v = full
            ? (unsigned char)(Y * 255.0f + 0.5f)
            : (unsigned char)(Y * 219.0f + 16.0f + 0.5f);
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

static __device__ float3 lrgb_to_yuv(float3 rgb, const TonemapCUDAParams *p)
{
    rgb = apply_trc_out(rgb, p->trc_out, p->hlg_peak_nits);
    /* Prevent out-of-range gamma RGB from blowing up U/V (magenta cast). */
    rgb.x = clampf(rgb.x, 0.0f, 1.0f);
    rgb.y = clampf(rgb.y, 0.0f, 1.0f);
    rgb.z = clampf(rgb.z, 0.0f, 1.0f);
    return mul3x3(p->rgb2yuv, rgb);
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

    int ux = x >> 1, uy = y >> 1;
    float U, V;
    sample_uv_norm(src_uv, src_uv_pitch, ux, uy, p.src_bit_depth, p.full_range_in, &U, &V);

    float Yn = sample_y_norm(src_y, src_y_pitch, x, y, p.src_bit_depth, p.full_range_in);
    float3 out = lrgb_to_yuv(process_pixel_lrgb(make_f3(Yn, U, V), &p), &p);
    store_y(dst_y, dst_y_pitch, x, y, out.x, p.dst_bit_depth, p.full_range_out);

    if ((x & 1) == 0 && (y & 1) == 0) {
        /* Average in linear RGB (OpenCL tonemap style) before YUV — avoids
         * magenta chroma bias when 2x2 scales differ strongly (clip/mobius). */
        float3 acc = make_f3(0, 0, 0);
        int count = 0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= width || yy >= height)
                    continue;
                float y2 = sample_y_norm(src_y, src_y_pitch, xx, yy,
                                         p.src_bit_depth, p.full_range_in);
                float3 rgb = process_pixel_lrgb(make_f3(y2, U, V), &p);
                acc.x += rgb.x;
                acc.y += rgb.y;
                acc.z += rgb.z;
                count++;
            }
        }
        if (count > 0) {
            float inv = 1.0f / (float)count;
            acc.x *= inv;
            acc.y *= inv;
            acc.z *= inv;
        }
        float3 chroma = lrgb_to_yuv(acc, &p);
        store_uv(dst_uv, dst_uv_pitch, ux, uy, chroma.y, chroma.z,
                 p.dst_bit_depth, p.full_range_out);
    }
}

/*
 * Subsampled linear-luma peak/avg for dynamic tonemap.
 * clang CUDA here has no __shared__/gridDim/atomicMax — one thread per block
 * walks a fixed 32x16 subsampled tile; host reduces. grid_w passed in.
 */
__global__ void tonemap_cuda_analyze(const void *src_y, int pitch,
                                     int width, int height,
                                     int bit_depth, int full_range,
                                     int trc, float hlg_peak_nits,
                                     TonemapCUDAAnalyzePartial *partial,
                                     int npartial, int grid_w)
{
    const int stride = 4;
    const int tile_x = 32;
    const int tile_y = 16;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;
    const int bid = by * grid_w + bx;
    float peak = 0.0f, sum = 0.0f;
    unsigned int cnt = 0;
    int tx, ty, x0, y0;
    float y, lin;

    if (threadIdx.x != 0 || threadIdx.y != 0)
        return;
    if (bid >= npartial)
        return;

    for (ty = 0; ty < tile_y; ty++) {
        y0 = (by * tile_y + ty) * stride;
        if (y0 >= height)
            break;
        for (tx = 0; tx < tile_x; tx++) {
            x0 = (bx * tile_x + tx) * stride;
            if (x0 >= width)
                break;
            y = sample_y_norm(src_y, pitch, x0, y0, bit_depth, full_range);
            switch (trc) {
            case TONEMAP_CUDA_TRC_PQ:
                lin = eotf_st2084(y);
                break;
            case TONEMAP_CUDA_TRC_HLG: {
                float sc = inverse_oetf_hlg(y);
                float gamma = 1.2f + 0.42f *
                    log10f(fmaxf(hlg_peak_nits, 1000.0f) / 1000.0f);
                float gain = hlg_peak_nits / REFERENCE_WHITE;
                lin = sc * (sc > 1e-6f ? powf(sc, gamma - 1.0f) : 0.0f) * gain;
                break;
            }
            default:
                lin = eotf_bt1886(y);
                break;
            }
            if (!(lin > 0.0f) || !(lin < 1e6f))
                lin = 0.0f;
            peak = fmaxf(peak, lin);
            sum += lin;
            cnt++;
        }
    }

    partial[bid].peak = peak;
    partial[bid].sum = sum;
    partial[bid].count = cnt;
    partial[bid]._pad = 0;
}

/*
 * Luma unsharp (separate src/dst — no __shared__, works with clang/nvcc PTX).
 * out = c + amount*(c - box3x3); soft-limit halos to neighbor min/max.
 */
__global__ void tonemap_cuda_sharpen_y(const void *src_y, void *dst_y,
                                       int pitch, int width, int height,
                                       int bit_depth, int full_range,
                                       float amount)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    float c, blur, mn, mx, out, lo, hi, v;
    int i, j, xx, yy;

    if (x >= width || y >= height || amount <= 1e-4f)
        return;

    c = sample_y_norm(src_y, pitch, x, y, bit_depth, full_range);
    blur = 0.0f;
    mn = c;
    mx = c;
#pragma unroll
    for (j = -1; j <= 1; j++) {
#pragma unroll
        for (i = -1; i <= 1; i++) {
            xx = x + i;
            yy = y + j;
            if (xx < 0) xx = 0;
            else if (xx >= width) xx = width - 1;
            if (yy < 0) yy = 0;
            else if (yy >= height) yy = height - 1;
            v = sample_y_norm(src_y, pitch, xx, yy, bit_depth, full_range);
            blur += v;
            mn = fminf(mn, v);
            mx = fmaxf(mx, v);
        }
    }
    blur *= (1.0f / 9.0f);
    out = c + amount * (c - blur);
    lo = fmaxf(mn - 0.02f, 0.0f);
    hi = fminf(mx + 0.02f, 1.0f);
    out = clampf(out, lo, hi);
    store_y(dst_y, pitch, x, y, out, bit_depth, full_range);
}

}
