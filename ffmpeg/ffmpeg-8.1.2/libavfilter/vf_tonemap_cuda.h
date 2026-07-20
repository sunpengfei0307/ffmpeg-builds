/*
 * Shared host/device ABI for tonemap_cuda
 *
 * This file is part of FFmpeg.
 */

#ifndef AVFILTER_VF_TONEMAP_CUDA_H
#define AVFILTER_VF_TONEMAP_CUDA_H

enum TonemapCUDAAlgorithm {
    TONEMAP_CUDA_NONE = 0,
    TONEMAP_CUDA_LINEAR,
    TONEMAP_CUDA_GAMMA,
    TONEMAP_CUDA_CLIP,
    TONEMAP_CUDA_REINHARD,
    TONEMAP_CUDA_HABLE,
    TONEMAP_CUDA_MOBIUS,
};

enum TonemapCUDAMode {
    TONEMAP_CUDA_MODE_AUTO = 0,
    TONEMAP_CUDA_MODE_HDR2SDR,
    TONEMAP_CUDA_MODE_SDR2HDR,
};

enum TonemapCUDATrc {
    TONEMAP_CUDA_TRC_BT709 = 0,
    TONEMAP_CUDA_TRC_BT2020_10,
    TONEMAP_CUDA_TRC_PQ,
    TONEMAP_CUDA_TRC_HLG,
    TONEMAP_CUDA_TRC_SRGB,
};

typedef struct TonemapCUDAParams {
    float peak;
    float param;
    float desat;
    float target_peak;
    int   tonemap;
    int   mode;          /* hdr2sdr / sdr2hdr */
    int   trc_in;
    int   trc_out;
    int   full_range_in;
    int   full_range_out;
    int   rgb2rgb_passthrough;
    float yuv2rgb[9];
    float rgb2yuv[9];
    float rgb2rgb[9];
    float luma_dst[3];
    int   src_bit_depth; /* 8 or 10 */
    int   dst_bit_depth; /* 8 or 10 */
} TonemapCUDAParams;

#endif /* AVFILTER_VF_TONEMAP_CUDA_H */
