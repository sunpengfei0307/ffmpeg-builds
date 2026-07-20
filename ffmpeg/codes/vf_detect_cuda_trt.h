/*
 * TensorRT wrapper for CUDA object detection filter.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AVFILTER_VF_DETECT_CUDA_TRT_H
#define AVFILTER_VF_DETECT_CUDA_TRT_H

#include <stddef.h>
#include <stdint.h>

#include "compat/cuda/dynlink_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFDetectTRTContext FFDetectTRTContext;

#define FF_DETECT_FACE_HEADS 3

typedef enum DetectModelType {
    DETECT_MODEL_AUTO = 0,
    DETECT_MODEL_YOLOV8,
    DETECT_MODEL_YOLOV8_FACE,
} DetectModelType;

typedef struct FFDetectTRTFaceHeadInfo {
    int grid_size;
    int stride;
    int channels;
    size_t bytes;
} FFDetectTRTFaceHeadInfo;

typedef struct FFDetectTRTConfig {
    const char *engine_path;
    DetectModelType model_type;
    int input_w;
    int input_h;
    int max_det;
    int fp16;
    float score_threshold;
    float iou_threshold;
    int cuda_device;
} FFDetectTRTConfig;

typedef struct FFDetectTRTModelInfo {
    DetectModelType model_type;
    int num_classes;
    int num_anchors;
    int yolo_transposed;
    size_t raw_output_bytes;
    int num_face_heads;
    FFDetectTRTFaceHeadInfo face_heads[FF_DETECT_FACE_HEADS];
    size_t face_heads_total_bytes;
} FFDetectTRTModelInfo;

typedef struct FFDetectTRTExecuteParams {
    CUdeviceptr input;
    CUdeviceptr raw_output;
    int num_raw_outputs;
    CUdeviceptr raw_outputs[FF_DETECT_FACE_HEADS];
    CUstream stream;
} FFDetectTRTExecuteParams;

int ff_detect_trt_create(FFDetectTRTContext **trt,
                         const FFDetectTRTConfig *cfg,
                         char *errbuf, int errbuf_size);

/* Resolve ONNX→TRT cache path for current GPU SM + TensorRT version.
 * Format: {onnx}.{WxH}.{fp16|fp32}.sm{XX}.trt{major}{minor}.trt
 * Returns 0 on success. For non-.onnx engine_path, copies engine_path. */
int ff_detect_trt_resolve_cache_path(const FFDetectTRTConfig *cfg,
                                     char *out_path, int out_path_size,
                                     char *errbuf, int errbuf_size);

int ff_detect_trt_get_info(const FFDetectTRTContext *trt,
                           FFDetectTRTModelInfo *info);

int ff_detect_trt_execute(FFDetectTRTContext *trt,
                          const FFDetectTRTExecuteParams *params,
                          char *errbuf, int errbuf_size);

void ff_detect_trt_destroy(FFDetectTRTContext **trt);

#ifdef __cplusplus
}
#endif

#endif /* AVFILTER_VF_DETECT_CUDA_TRT_H */
