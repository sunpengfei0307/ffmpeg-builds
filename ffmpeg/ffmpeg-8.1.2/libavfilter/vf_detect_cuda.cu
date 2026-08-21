/*
 * CUDA kernels for TensorRT object detection filter.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define DETECT_FMT_NV12     0
#define DETECT_FMT_P010     1
#define DETECT_FMT_YUV420P  2

typedef struct DetectBox {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
    int class_id;
} DetectBox;

__device__ inline float clampf(float v, float lo, float hi)
{
    return fminf(fmaxf(v, lo), hi);
}

__device__ inline int clampi(int v, int lo, int hi)
{
    return min(max(v, lo), hi);
}

__device__ inline float read_luma(const unsigned char *y, int pitch,
                                  int x, int y_pos, int fmt, int bit_depth)
{
    if (fmt == DETECT_FMT_P010) {
        const unsigned short *row = (const unsigned short *)(y + y_pos * pitch);
        return (float)(row[x] >> (16 - bit_depth)) / (float)((1 << bit_depth) - 1);
    }
    return (float)y[y_pos * pitch + x] / 255.0f;
}

__device__ inline void read_chroma(const unsigned char *uv, const unsigned char *vplane,
                                   int pitch_uv, int pitch_v, int x, int y_pos,
                                   int fmt, int bit_depth, float *u, float *v)
{
    if (fmt == DETECT_FMT_P010) {
        const unsigned short *row = (const unsigned short *)(uv + y_pos * pitch_uv);
        unsigned short u16 = row[(x << 1) + 0];
        unsigned short v16 = row[(x << 1) + 1];
        *u = (float)(u16 >> (16 - bit_depth)) / (float)((1 << bit_depth) - 1);
        *v = (float)(v16 >> (16 - bit_depth)) / (float)((1 << bit_depth) - 1);
    } else if (fmt == DETECT_FMT_NV12) {
        const unsigned char *row = uv + y_pos * pitch_uv;
        *u = (float)row[(x << 1) + 0] / 255.0f;
        *v = (float)row[(x << 1) + 1] / 255.0f;
    } else {
        *u = (float)uv[y_pos * pitch_uv + x] / 255.0f;
        *v = (float)vplane[y_pos * pitch_v + x] / 255.0f;
    }
}

extern "C" __global__ void detect_preproc_420(const unsigned char *src_y,
                                               const unsigned char *src_u,
                                               const unsigned char *src_v,
                                               int src_pitch_y,
                                               int src_pitch_u,
                                               int src_pitch_v,
                                               int src_w, int src_h,
                                               int fmt, int bit_depth,
                                               float *dst,
                                               int input_w, int input_h,
                                               float ratio, float pad_x, float pad_y,
                                               const float *matrix,
                                               const float *bias)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int plane_size = input_w * input_h;
    float yy, uu, vv, r, g, b;
    float sx_f, sy_f;
    int sx, sy;

    if (x >= input_w || y >= input_h)
        return;

    sx_f = ((float)x - pad_x) / ratio;
    sy_f = ((float)y - pad_y) / ratio;

    if (sx_f < 0.0f || sy_f < 0.0f || sx_f > (float)(src_w - 1) ||
        sy_f > (float)(src_h - 1)) {
        r = g = b = 0.447f;
    } else {
        sx = clampi((int)(sx_f + 0.5f), 0, src_w - 1);
        sy = clampi((int)(sy_f + 0.5f), 0, src_h - 1);
        yy = read_luma(src_y, src_pitch_y, sx, sy, fmt, bit_depth);
        read_chroma(src_u, src_v, src_pitch_u, src_pitch_v, sx >> 1, sy >> 1,
                    fmt, bit_depth, &uu, &vv);

        r = matrix[0] * yy + matrix[1] * uu + matrix[2] * vv + bias[0];
        g = matrix[3] * yy + matrix[4] * uu + matrix[5] * vv + bias[1];
        b = matrix[6] * yy + matrix[7] * uu + matrix[8] * vv + bias[2];
        r = clampf(r, 0.0f, 1.0f);
        g = clampf(g, 0.0f, 1.0f);
        b = clampf(b, 0.0f, 1.0f);
    }

    dst[y * input_w + x] = r;
    dst[plane_size + y * input_w + x] = g;
    dst[2 * plane_size + y * input_w + x] = b;
}

extern "C" __global__ void detect_yolov8_decode(const float *output,
                                                 int num_classes,
                                                 int num_anchors,
                                                 int transposed,
                                                 DetectBox *out_boxes,
                                                 int *out_count,
                                                 int max_det,
                                                 float score_threshold,
                                                 int filter_class_id,
                                                 int output_class_id,
                                                 float ratio,
                                                 float pad_x,
                                                 float pad_y,
                                                 int src_w,
                                                 int src_h)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float cx, cy, w, h, score, x1, y1, x2, y2;
    int best_cls = 0, c, slot, feat;

    if (i >= num_anchors)
        return;

    feat = 4 + num_classes;
    if (transposed) {
        cx = output[i * feat + 0];
        cy = output[i * feat + 1];
        w  = output[i * feat + 2];
        h  = output[i * feat + 3];
        score = output[i * feat + 4];
        best_cls = 0;
        for (c = 1; c < num_classes; c++) {
            float s = output[i * feat + 4 + c];
            if (s > score) {
                score = s;
                best_cls = c;
            }
        }
    } else {
        cx = output[0 * num_anchors + i];
        cy = output[1 * num_anchors + i];
        w  = output[2 * num_anchors + i];
        h  = output[3 * num_anchors + i];
        score = output[4 * num_anchors + i];
        best_cls = 0;
        for (c = 1; c < num_classes; c++) {
            float s = output[(4 + c) * num_anchors + i];
            if (s > score) {
                score = s;
                best_cls = c;
            }
        }
    }

    if (score < score_threshold)
        return;
    if (filter_class_id >= 0 && best_cls != filter_class_id)
        return;

    x1 = (cx - w * 0.5f - pad_x) / ratio;
    y1 = (cy - h * 0.5f - pad_y) / ratio;
    x2 = (cx + w * 0.5f - pad_x) / ratio;
    y2 = (cy + h * 0.5f - pad_y) / ratio;

    slot = atomicAdd(out_count, 1);
    if (slot >= max_det) {
        atomicSub(out_count, 1);
        return;
    }

    out_boxes[slot].x1 = clampf(x1, 0.0f, (float)(src_w - 1));
    out_boxes[slot].y1 = clampf(y1, 0.0f, (float)(src_h - 1));
    out_boxes[slot].x2 = clampf(x2, 0.0f, (float)(src_w - 1));
    out_boxes[slot].y2 = clampf(y2, 0.0f, (float)(src_h - 1));
    out_boxes[slot].score = score;
    out_boxes[slot].class_id = output_class_id;
}

__device__ inline int on_box_border(int x, int y, const DetectBox *b, int thickness)
{
    int left = (int)b->x1;
    int right = (int)b->x2;
    int top = (int)b->y1;
    int bottom = (int)b->y2;

    if (x < left || x > right || y < top || y > bottom)
        return 0;

    return (x - left < thickness) || (right - x < thickness) ||
           (y - top < thickness) || (bottom - y < thickness);
}

__device__ inline int point_in_face_box(int x, int y, const DetectBox *boxes,
                                         int n, int face_class_id, float pad_ratio)
{
    int i;
    for (i = 0; i < n; i++) {
        float bw, bh, pad;
        int left, right, top, bottom;
        if (boxes[i].class_id != face_class_id)
            continue;
        bw = boxes[i].x2 - boxes[i].x1;
        bh = boxes[i].y2 - boxes[i].y1;
        pad = pad_ratio * fminf(bw, bh);
        left = (int)floorf(boxes[i].x1 - pad);
        top = (int)floorf(boxes[i].y1 - pad);
        right = (int)ceilf(boxes[i].x2 + pad);
        bottom = (int)ceilf(boxes[i].y2 + pad);
        if (x >= left && x <= right && y >= top && y <= bottom)
            return 1;
    }
    return 0;
}

__device__ inline void write_luma(unsigned char *y, int pitch, int x, int y_pos,
                                  int fmt, int bit_depth, float val)
{
    val = clampf(val, 0.0f, 1.0f);
    if (fmt == DETECT_FMT_P010) {
        unsigned short *row = (unsigned short *)(y + y_pos * pitch);
        unsigned int maxv = (1u << bit_depth) - 1u;
        row[x] = (unsigned short)((unsigned int)(val * maxv + 0.5f) << (16 - bit_depth));
    } else {
        y[y_pos * pitch + x] = (unsigned char)(val * 255.0f + 0.5f);
    }
}

__device__ inline void write_chroma(unsigned char *uv, unsigned char *vplane,
                                    int pitch_uv, int pitch_v, int x, int y_pos,
                                    int fmt, int bit_depth, float u, float v)
{
    u = clampf(u, 0.0f, 1.0f);
    v = clampf(v, 0.0f, 1.0f);
    if (fmt == DETECT_FMT_P010) {
        unsigned short *row = (unsigned short *)(uv + y_pos * pitch_uv);
        unsigned int maxv = (1u << bit_depth) - 1u;
        row[(x << 1) + 0] = (unsigned short)((unsigned int)(u * maxv + 0.5f) << (16 - bit_depth));
        row[(x << 1) + 1] = (unsigned short)((unsigned int)(v * maxv + 0.5f) << (16 - bit_depth));
    } else if (fmt == DETECT_FMT_NV12) {
        unsigned char *row = uv + y_pos * pitch_uv;
        row[(x << 1) + 0] = (unsigned char)(u * 255.0f + 0.5f);
        row[(x << 1) + 1] = (unsigned char)(v * 255.0f + 0.5f);
    } else {
        uv[y_pos * pitch_uv + x] = (unsigned char)(u * 255.0f + 0.5f);
        vplane[y_pos * pitch_v + x] = (unsigned char)(v * 255.0f + 0.5f);
    }
}

/*
 * Face-ROI bilateral (bilateral_cuda-style). Reads src, writes dst only inside
 * face boxes; sigmaS/sigmaR are in pixel / 0-255 color units.
 */
extern "C" __global__ void detect_bilateral_face_y(const unsigned char *src_y,
                                                    unsigned char *dst_y,
                                                    int pitch_src_y,
                                                    int pitch_dst_y,
                                                    int width, int height,
                                                    int fmt, int bit_depth,
                                                    const DetectBox *boxes,
                                                    const int *count,
                                                    int max_det,
                                                    int face_class_id,
                                                    int window_size,
                                                    float sigmaS,
                                                    float sigmaR)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int n = min(count ? *count : 0, max_det);
    int half, i, j, r, c;
    float cy, Wp, acc, w, ny;
    float inv_2s = 1.0f / (2.0f * sigmaS * sigmaS);
    float inv_2r = 1.0f / (2.0f * sigmaR * sigmaR);

    if (x >= width || y >= height || window_size < 1 || n <= 0)
        return;
    if (!point_in_face_box(x, y, boxes, n, face_class_id, 0.08f))
        return;

    half = window_size / 2;
    cy = read_luma(src_y, pitch_src_y, x, y, fmt, bit_depth) * 255.0f;
    Wp = 0.0f;
    acc = 0.0f;

    for (j = -half; j <= half; j++) {
        for (i = -half; i <= half; i++) {
            r = clampi(x + i, 0, width - 1);
            c = clampi(y + j, 0, height - 1);
            ny = read_luma(src_y, pitch_src_y, r, c, fmt, bit_depth) * 255.0f;
            w = __expf(-((float)(i * i + j * j)) * inv_2s -
                       ((ny - cy) * (ny - cy)) * inv_2r);
            Wp += w;
            acc += ny * w;
        }
    }
    if (Wp > 1e-6f)
        write_luma(dst_y, pitch_dst_y, x, y, fmt, bit_depth, (acc / Wp) / 255.0f);
}

extern "C" __global__ void detect_bilateral_face_uv(const unsigned char *src_y,
                                                     const unsigned char *src_u,
                                                     const unsigned char *src_v,
                                                     unsigned char *dst_u,
                                                     unsigned char *dst_v,
                                                     int pitch_src_y,
                                                     int pitch_src_u,
                                                     int pitch_src_v,
                                                     int pitch_dst_u,
                                                     int pitch_dst_v,
                                                     int width, int height,
                                                     int fmt, int bit_depth,
                                                     const DetectBox *boxes,
                                                     const int *count,
                                                     int max_det,
                                                     int face_class_id,
                                                     int window_size,
                                                     float sigmaS,
                                                     float sigmaR)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int px = x << 1;
    int py = y << 1;
    int n = min(count ? *count : 0, max_det);
    int half, i, j, r, c, rr, cc;
    float cy, cu, cv, Wp, acc_u, acc_v, w, ny, nu, nv, d2;
    float inv_2s = 1.0f / (2.0f * sigmaS * sigmaS);
    float inv_2r = 1.0f / (2.0f * sigmaR * sigmaR);

    if (px >= width || py >= height || window_size < 1 || n <= 0)
        return;
    if (!point_in_face_box(px, py, boxes, n, face_class_id, 0.08f) &&
        !point_in_face_box(px + 1, py, boxes, n, face_class_id, 0.08f) &&
        !point_in_face_box(px, py + 1, boxes, n, face_class_id, 0.08f) &&
        !point_in_face_box(px + 1, py + 1, boxes, n, face_class_id, 0.08f))
        return;

    half = window_size / 2;
    cy = read_luma(src_y, pitch_src_y, px, py, fmt, bit_depth) * 255.0f;
    read_chroma(src_u, src_v, pitch_src_u, pitch_src_v, x, y, fmt, bit_depth, &cu, &cv);
    cu *= 255.0f;
    cv *= 255.0f;
    Wp = 0.0f;
    acc_u = 0.0f;
    acc_v = 0.0f;

    for (j = -half; j <= half; j++) {
        for (i = -half; i <= half; i++) {
            rr = clampi(px + i, 0, width - 1);
            cc = clampi(py + j, 0, height - 1);
            r = rr >> 1;
            c = cc >> 1;
            ny = read_luma(src_y, pitch_src_y, rr, cc, fmt, bit_depth) * 255.0f;
            read_chroma(src_u, src_v, pitch_src_u, pitch_src_v, r, c, fmt, bit_depth, &nu, &nv);
            nu *= 255.0f;
            nv *= 255.0f;
            d2 = (ny - cy) * (ny - cy) + (nu - cu) * (nu - cu) + (nv - cv) * (nv - cv);
            w = __expf(-((float)(i * i + j * j)) * inv_2s - d2 * inv_2r);
            Wp += w;
            acc_u += nu * w;
            acc_v += nv * w;
        }
    }
    if (Wp > 1e-6f)
        write_chroma(dst_u, dst_v, pitch_dst_u, pitch_dst_v, x, y, fmt, bit_depth,
                     (acc_u / Wp) / 255.0f, (acc_v / Wp) / 255.0f);
}

extern "C" __global__ void detect_draw_y(unsigned char *dst_y,
                                          int pitch_y,
                                          int width, int height,
                                          int fmt, int bit_depth,
                                          const DetectBox *boxes,
                                          const int *count,
                                          int max_det,
                                          int thickness,
                                          int face_class_id)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int n = min(count ? *count : 0, max_det);
    int i;

    if (x >= width || y >= height)
        return;

    for (i = 0; i < n; i++) {
        if (on_box_border(x, y, &boxes[i], thickness)) {
            if (fmt == DETECT_FMT_P010) {
                unsigned short *row = (unsigned short *)(dst_y + y * pitch_y);
                row[x] = (unsigned short)(940 << (16 - bit_depth));
            } else {
                dst_y[y * pitch_y + x] = boxes[i].class_id == face_class_id ? 235 : 210;
            }
            return;
        }
    }
}

extern "C" __global__ void detect_draw_uv(unsigned char *dst_u,
                                           unsigned char *dst_v,
                                           int pitch_u,
                                           int pitch_v,
                                           int width, int height,
                                           int fmt, int bit_depth,
                                           const DetectBox *boxes,
                                           const int *count,
                                           int max_det,
                                           int thickness,
                                           int face_class_id)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int px = x << 1;
    int py = y << 1;
    int n = min(count ? *count : 0, max_det);
    int i;

    if (px >= width || py >= height)
        return;

    for (i = 0; i < n; i++) {
        if (on_box_border(px, py, &boxes[i], thickness) ||
            on_box_border(px + 1, py, &boxes[i], thickness) ||
            on_box_border(px, py + 1, &boxes[i], thickness) ||
            on_box_border(px + 1, py + 1, &boxes[i], thickness)) {
            int face = boxes[i].class_id == face_class_id;
            if (fmt == DETECT_FMT_P010) {
                unsigned short *row = (unsigned short *)(dst_u + y * pitch_u);
                unsigned short u = (unsigned short)((face ? 512 : 256) << (16 - bit_depth));
                unsigned short v = (unsigned short)((face ? 896 : 256) << (16 - bit_depth));
                row[(x << 1) + 0] = u;
                row[(x << 1) + 1] = v;
            } else if (fmt == DETECT_FMT_NV12) {
                unsigned char *row = dst_u + y * pitch_u;
                row[(x << 1) + 0] = face ? 128 : 64;
                row[(x << 1) + 1] = face ? 240 : 64;
            } else {
                dst_u[y * pitch_u + x] = face ? 128 : 64;
                dst_v[y * pitch_v + x] = face ? 240 : 64;
            }
            return;
        }
    }
}
