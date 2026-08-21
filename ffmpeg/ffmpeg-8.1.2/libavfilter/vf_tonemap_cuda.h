/*
 * Shared host/device ABI for tonemap_cuda (libplacebo-inspired).
 *
 * This file is part of FFmpeg.
 */

#ifndef AVFILTER_VF_TONEMAP_CUDA_H
#define AVFILTER_VF_TONEMAP_CUDA_H

/* Align naming with vf_libplacebo / libplacebo tone_mapping.h where practical. */
enum TonemapCUDAAlgorithm {
    TONEMAP_CUDA_AUTO = 0,     /* = spline */
    TONEMAP_CUDA_CLIP,
    TONEMAP_CUDA_BT2390,
    TONEMAP_CUDA_BT2446A,
    TONEMAP_CUDA_SPLINE,
    TONEMAP_CUDA_REINHARD,
    TONEMAP_CUDA_MOBIUS,
    TONEMAP_CUDA_HABLE,
    TONEMAP_CUDA_GAMMA,
    TONEMAP_CUDA_LINEAR,
    TONEMAP_CUDA_NONE,
    TONEMAP_CUDA_COUNT,
};

enum TonemapCUDAMode {
    TONEMAP_CUDA_MODE_AUTO = 0,
    TONEMAP_CUDA_MODE_HDR2SDR,
    TONEMAP_CUDA_MODE_SDR2HDR,
};

enum TonemapCUDATrc {
    TONEMAP_CUDA_TRC_BT709 = 0,
    TONEMAP_CUDA_TRC_BT2020_10,
    TONEMAP_CUDA_TRC_PQ,       /* smpte2084 */
    TONEMAP_CUDA_TRC_HLG,      /* arib-std-b67 */
    TONEMAP_CUDA_TRC_SRGB,
};

enum TonemapCUDAGamut {
    TONEMAP_CUDA_GAMUT_CLIP = 0,
    TONEMAP_CUDA_GAMUT_DESATURATE,
    TONEMAP_CUDA_GAMUT_PERCEPTUAL,
};

typedef struct TonemapCUDAParams {
    /* Internal peaks in relative luminance (nits / REFERENCE_WHITE=100).
     * Host exposes a single option `peak` (HDR-side); these are derived by mode. */
    float src_peak;
    float dst_peak;
    float param;               /* algorithm-specific */
    float desat;
    float exposure;            /* linear gain (libplacebo constants.exposure) */
    float knee;                /* linear_knee / mobius j / spline pivot */
    float reinhard_contrast;
    float spline_contrast;
    float knee_offset;         /* BT.2390 */
    float brightness;          /* additive in linear */
    float contrast;            /* multiplicative */
    float saturation;
    float hlg_peak_nits;       /* HLG system gamma reference, usually 1000 */

    int   tonemap;
    int   mode;                /* hdr2sdr / sdr2hdr */
    int   gamut_mode;
    int   trc_in;
    int   trc_out;
    int   full_range_in;
    int   full_range_out;
    int   rgb2rgb_passthrough;
    float yuv2rgb[9];
    float rgb2yuv[9];
    float rgb2rgb[9];
    float luma_src[3];         /* tonemap in source primaries (VLC order) */
    float luma_dst[3];         /* gamut map / encode in destination */
    int   src_bit_depth;       /* 8 or 10 */
    int   dst_bit_depth;       /* 8 or 10 */
} TonemapCUDAParams;

/* Per-block analyze result; host reduces across blocks (no atomicMax). */
typedef struct TonemapCUDAAnalyzePartial {
    float peak;
    float sum;
    unsigned int count;
    unsigned int _pad;
} TonemapCUDAAnalyzePartial;

#endif /* AVFILTER_VF_TONEMAP_CUDA_H */
