/*
 * Multi-input CUDA compositor kernels (fill + scale-blit into slot).
 * This file is part of FFmpeg.
 */

extern "C" {

__device__ inline unsigned char clamp_u8(float v)
{
    return (unsigned char)(v < 0.f ? 0.f : (v > 255.f ? 255.f : v));
}

/* Rounded-rect helpers (local coords inside a dw×dh slot). */
__device__ inline int mixing_clamp_radius(int radius, int w, int h)
{
    int r = radius;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    return r;
}

__device__ inline int mixing_inside_rounded_rect(int x, int y, int w, int h, int radius)
{
    int r, cx, cy, dx, dy;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    r = mixing_clamp_radius(radius, w, h);
    if (r <= 0)
        return 1;
    if (x >= r && x < w - r)
        return 1;
    if (y >= r && y < h - r)
        return 1;
    if (x < r && y < r) {
        cx = r; cy = r;
    } else if (x >= w - r && y < r) {
        cx = w - 1 - r; cy = r;
    } else if (x < r && y >= h - r) {
        cx = r; cy = h - 1 - r;
    } else {
        cx = w - 1 - r; cy = h - 1 - r;
    }
    dx = x - cx;
    dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

__device__ inline int mixing_in_rounded_border(int x, int y, int w, int h,
                                               int border_px, int radius)
{
    int iw, ih, ir, ix, iy;
    if (border_px <= 0)
        return 0;
    if (!mixing_inside_rounded_rect(x, y, w, h, radius))
        return 0;
    iw = w - 2 * border_px;
    ih = h - 2 * border_px;
    if (iw <= 0 || ih <= 0)
        return 1;
    ir = radius - border_px;
    if (ir < 0)
        ir = 0;
    ix = x - border_px;
    iy = y - border_px;
    if (ix < 0 || iy < 0 || ix >= iw || iy >= ih)
        return 1;
    return !mixing_inside_rounded_rect(ix, iy, iw, ih, ir);
}

__device__ inline float lanczos2_weight(float x)
{
    if (x == 0.f)
        return 1.f;
    if (x <= -2.f || x >= 2.f)
        return 0.f;
    return 2.f * __sinf(3.14159265f * x) * __sinf(3.14159265f * x / 2.f)
         / (3.14159265f * 3.14159265f * x * x);
}

__device__ float sample_plane_bilinear(
    const unsigned char *src, int src_linesize,
    int src_w, int src_h, float sx, float sy)
{
    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;

    x0 = x0 < 0 ? 0 : (x0 >= src_w ? src_w - 1 : x0);
    x1 = x1 < 0 ? 0 : (x1 >= src_w ? src_w - 1 : x1);
    y0 = y0 < 0 ? 0 : (y0 >= src_h ? src_h - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 >= src_h ? src_h - 1 : y1);

    float v00 = src[x0 + y0 * src_linesize];
    float v10 = src[x1 + y0 * src_linesize];
    float v01 = src[x0 + y1 * src_linesize];
    float v11 = src[x1 + y1 * src_linesize];
    return (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
         + (1.f - fx) * fy * v01 + fx * fy * v11;
}

__device__ float sample_plane_lanczos2(
    const unsigned char *src, int src_linesize,
    int src_w, int src_h, float sx, float sy)
{
    int ix = (int)floorf(sx);
    int iy = (int)floorf(sy);
    float sum = 0.f;
    float wsum = 0.f;

    for (int j = -1; j <= 2; j++) {
        for (int i = -1; i <= 2; i++) {
            int px = ix + i;
            int py = iy + j;
            float w = lanczos2_weight(sx - (float)px) * lanczos2_weight(sy - (float)py);
            if (w == 0.f)
                continue;
            px = px < 0 ? 0 : (px >= src_w ? src_w - 1 : px);
            py = py < 0 ? 0 : (py >= src_h ? src_h - 1 : py);
            sum += w * src[px + py * src_linesize];
            wsum += w;
        }
    }
    return wsum > 0.f ? sum / wsum : 0.f;
}

/* Fill a Y plane. */
__global__ void MixingFill_Y(
    unsigned char *dst, int dst_linesize,
    int width, int height,
    unsigned char y_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    dst[x + y * dst_linesize] = y_val;
}

/* Fill NV12 UV plane (width/height are full luma size; UV is half). */
__global__ void MixingFill_UV_NV12(
    unsigned char *dst, int dst_linesize,
    int width, int height,
    unsigned char u_val, unsigned char v_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = width >> 1;
    int uv_h = height >> 1;
    if (x >= uv_w || y >= uv_h)
        return;
    int off = (x << 1) + y * dst_linesize;
    dst[off]     = u_val;
    dst[off + 1] = v_val;
}

/* Copy full frame (nearest) for background plate. */
__global__ void MixingCopy_Y(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    dst[x + y * dst_linesize] = src[x + y * src_linesize];
}

__global__ void MixingCopy_UV_NV12(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = width >> 1;
    int uv_h = height >> 1;
    if (x >= uv_w || y >= uv_h)
        return;
    int off = (x << 1) + y * dst_linesize;
    int soff = (x << 1) + y * src_linesize;
    dst[off]     = src[soff];
    dst[off + 1] = src[soff + 1];
}

/*
 * Bilinear scale source plane into destination slot (fit inside dx,dy,dw,dh).
 */
__global__ void MixingScaleBlit_Y(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;
    if (!mixing_inside_rounded_rect(x, y, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float v = sample_plane_bilinear(src, src_linesize, src_w, src_h, sx, sy);
    dst[(dx + x) + (dy + y) * dst_linesize] = clamp_u8(v);
}

__global__ void MixingScaleBlit_Y_Lanczos(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;
    if (!mixing_inside_rounded_rect(x, y, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float v = sample_plane_lanczos2(src, src_linesize, src_w, src_h, sx, sy);
    dst[(dx + x) + (dy + y) * dst_linesize] = clamp_u8(v);
}

/* NV12 UV: interleaved U/V, half resolution. Slot coords are luma-space. */
__global__ void MixingScaleBlit_UV_NV12(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;
    lx = x << 1;
    ly = y << 1;
    if (!mixing_inside_rounded_rect(lx, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx, ly + 1, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly + 1, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;

    for (int c = 0; c < 2; c++) {
        int x0 = (int)floorf(sx);
        int y0 = (int)floorf(sy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float fx = sx - (float)x0;
        float fy = sy - (float)y0;

        x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
        x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
        y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);

        float v00 = src[(x0 << 1) + c + y0 * src_linesize];
        float v10 = src[(x1 << 1) + c + y0 * src_linesize];
        float v01 = src[(x0 << 1) + c + y1 * src_linesize];
        float v11 = src[(x1 << 1) + c + y1 * src_linesize];
        float v = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                + (1.f - fx) * fy * v01 + fx * fy * v11;
        int dox = ((dx >> 1) + x) << 1;
        int doy = (dy >> 1) + y;
        dst[dox + c + doy * dst_linesize] = clamp_u8(v);
    }
}

__global__ void MixingScaleBlit_UV_NV12_Lanczos(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;
    lx = x << 1;
    ly = y << 1;
    if (!mixing_inside_rounded_rect(lx, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx, ly + 1, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly + 1, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;

    for (int c = 0; c < 2; c++) {
        int ix = (int)floorf(sx);
        int iy = (int)floorf(sy);
        float sum = 0.f;
        float wsum = 0.f;

        for (int j = -1; j <= 2; j++) {
            for (int i = -1; i <= 2; i++) {
                int px = ix + i;
                int py = iy + j;
                float w = lanczos2_weight(sx - (float)px) * lanczos2_weight(sy - (float)py);
                if (w == 0.f)
                    continue;
                px = px < 0 ? 0 : (px >= src_uv_w ? src_uv_w - 1 : px);
                py = py < 0 ? 0 : (py >= src_uv_h ? src_uv_h - 1 : py);
                sum += w * src[(px << 1) + c + py * src_linesize];
                wsum += w;
            }
        }
        float v = wsum > 0.f ? sum / wsum : 0.f;
        int dox = ((dx >> 1) + x) << 1;
        int doy = (dy >> 1) + y;
        dst[dox + c + doy * dst_linesize] = clamp_u8(v);
    }
}

/*
 * Scale-blit with alpha over existing destination (tile fade / fly).
 * alpha_q8: 0..255. dst = dst*(1-a) + src*a
 */
__global__ void MixingScaleBlitAlpha_Y(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float sv = sample_plane_bilinear(src, src_linesize, src_w, src_h, sx, sy);
    int di = (dx + x) + (dy + y) * dst_linesize;
    int dv = dst[di];
    int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
    dst[di] = clamp_u8(((dv * (255 - a)) + (int)(sv * a) + 127) / 255);
}

__global__ void MixingScaleBlitAlpha_UV_NV12(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;
    int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);

    for (int c = 0; c < 2; c++) {
        int x0 = (int)floorf(sx);
        int y0 = (int)floorf(sy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float fx = sx - (float)x0;
        float fy = sy - (float)y0;

        x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
        x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
        y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);

        float v00 = src[(x0 << 1) + c + y0 * src_linesize];
        float v10 = src[(x1 << 1) + c + y0 * src_linesize];
        float v01 = src[(x0 << 1) + c + y1 * src_linesize];
        float v11 = src[(x1 << 1) + c + y1 * src_linesize];
        float sv = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                 + (1.f - fx) * fy * v01 + fx * fy * v11;
        int dox = ((dx >> 1) + x) << 1;
        int doy = (dy >> 1) + y;
        int di = dox + c + doy * dst_linesize;
        int dv = dst[di];
        dst[di] = clamp_u8(((dv * (255 - a)) + (int)(sv * a) + 127) / 255);
    }
}

/*
 * Scale full src into [dx,dy,dw,dh], but only write pixels inside the slot
 * clip rectangle (for slide/fly entry from outside the tile).
 */
__global__ void MixingScaleBlitOffsetAlpha_Y(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int clip_x, int clip_y, int clip_w, int clip_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int gx, gy;
    if (x >= clip_w || y >= clip_h || clip_w <= 0 || clip_h <= 0 || dw <= 0 || dh <= 0)
        return;
    gx = clip_x + x;
    gy = clip_y + y;
    if (gx < dx || gy < dy || gx >= dx + dw || gy >= dy + dh)
        return;

    {
        float sx = ((float)(gx - dx) + 0.5f) * (float)src_w / (float)dw - 0.5f;
        float sy = ((float)(gy - dy) + 0.5f) * (float)src_h / (float)dh - 0.5f;
        float sv = sample_plane_bilinear(src, src_linesize, src_w, src_h, sx, sy);
        int di = gx + gy * dst_linesize;
        int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
        if (a >= 255) {
            dst[di] = clamp_u8(sv);
        } else if (a > 0) {
            int dv = dst[di];
            dst[di] = clamp_u8(((dv * (255 - a)) + (int)(sv * a) + 127) / 255);
        }
    }
}

__global__ void MixingScaleBlitOffsetAlpha_UV_NV12(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int clip_x, int clip_y, int clip_w, int clip_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int clip_uv_w = clip_w >> 1;
    int clip_uv_h = clip_h >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int gx, gy, lx, ly;
    if (x >= clip_uv_w || y >= clip_uv_h || clip_uv_w <= 0 || clip_uv_h <= 0 ||
        dw < 2 || dh < 2)
        return;
    gx = (clip_x >> 1) + x;
    gy = (clip_y >> 1) + y;
    lx = gx - (dx >> 1);
    ly = gy - (dy >> 1);
    if (lx < 0 || ly < 0 || lx >= (dw >> 1) || ly >= (dh >> 1))
        return;

    {
        float sx = ((float)lx + 0.5f) * (float)src_uv_w / (float)(dw >> 1) - 0.5f;
        float sy = ((float)ly + 0.5f) * (float)src_uv_h / (float)(dh >> 1) - 0.5f;
        int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
        for (int c = 0; c < 2; c++) {
            int x0 = (int)floorf(sx);
            int y0 = (int)floorf(sy);
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float fx = sx - (float)x0;
            float fy = sy - (float)y0;
            x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
            x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
            y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
            y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);
            float v00 = src[(x0 << 1) + c + y0 * src_linesize];
            float v10 = src[(x1 << 1) + c + y0 * src_linesize];
            float v01 = src[(x0 << 1) + c + y1 * src_linesize];
            float v11 = src[(x1 << 1) + c + y1 * src_linesize];
            float sv = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                     + (1.f - fx) * fy * v01 + fx * fy * v11;
            int di = (gx << 1) + c + gy * dst_linesize;
            if (a >= 255) {
                dst[di] = clamp_u8(sv);
            } else if (a > 0) {
                int dv = dst[di];
                dst[di] = clamp_u8(((dv * (255 - a)) + (int)(sv * a) + 127) / 255);
            }
        }
    }
}

/*
 * Draw a highlight border around a slot rectangle (Y plane).
 * Overwrites border_px pixels at each edge of [dx,dy,dw,dh] with y_val.
 */
__global__ void MixingHighlightBorder_Y(
    unsigned char *dst, int dst_linesize,
    int canvas_w, int canvas_h,
    int dx, int dy, int dw, int dh,
    int border_px, int radius, unsigned char y_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh)
        return;

    if (!mixing_in_rounded_border(x, y, dw, dh, border_px, radius))
        return;

    int gx = dx + x;
    int gy = dy + y;
    if (gx < 0 || gy < 0 || gx >= canvas_w || gy >= canvas_h)
        return;
    dst[gx + gy * dst_linesize] = y_val;
}

/*
 * Draw a highlight border around a slot rectangle (NV12 UV plane).
 * border_px / radius are in luma space; UV is half resolution.
 */
__global__ void MixingHighlightBorder_UV_NV12(
    unsigned char *dst, int dst_linesize,
    int canvas_w, int canvas_h,
    int dx, int dy, int dw, int dh,
    int border_px, int radius, unsigned char u_val, unsigned char v_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = canvas_w >> 1;
    int uv_h = canvas_h >> 1;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h)
        return;

    lx = x << 1;
    ly = y << 1;
    if (!mixing_in_rounded_border(lx, ly, dw, dh, border_px, radius) &&
        !mixing_in_rounded_border(lx + 1, ly, dw, dh, border_px, radius))
        return;

    int gx = (dx >> 1) + x;
    int gy = (dy >> 1) + y;
    if (gx < 0 || gy < 0 || gx >= uv_w || gy >= uv_h)
        return;
    int off = (gx << 1) + gy * dst_linesize;
    dst[off]     = u_val;
    dst[off + 1] = v_val;
}

/* ---- P010 / 10-bit (uint16, 10 meaningful bits in high position) ---- */

__device__ inline unsigned short clamp_u16(float v)
{
    return (unsigned short)(v < 0.f ? 0.f : (v > 65535.f ? 65535.f : (v + 0.5f)));
}

__device__ inline unsigned short load_u16(const unsigned char *src, int linesize,
                                          int x, int y)
{
    const unsigned short *row = (const unsigned short *)(src + y * linesize);
    return row[x];
}

__device__ inline void store_u16(unsigned char *dst, int linesize,
                                  int x, int y, unsigned short v)
{
    unsigned short *row = (unsigned short *)(dst + y * linesize);
    row[x] = v;
}

__device__ float sample_plane_bilinear_u16(
    const unsigned char *src, int src_linesize,
    int src_w, int src_h, float sx, float sy)
{
    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;

    x0 = x0 < 0 ? 0 : (x0 >= src_w ? src_w - 1 : x0);
    x1 = x1 < 0 ? 0 : (x1 >= src_w ? src_w - 1 : x1);
    y0 = y0 < 0 ? 0 : (y0 >= src_h ? src_h - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 >= src_h ? src_h - 1 : y1);

    float v00 = load_u16(src, src_linesize, x0, y0);
    float v10 = load_u16(src, src_linesize, x1, y0);
    float v01 = load_u16(src, src_linesize, x0, y1);
    float v11 = load_u16(src, src_linesize, x1, y1);
    return (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
         + (1.f - fx) * fy * v01 + fx * fy * v11;
}

__device__ float sample_plane_lanczos2_u16(
    const unsigned char *src, int src_linesize,
    int src_w, int src_h, float sx, float sy)
{
    int ix = (int)floorf(sx);
    int iy = (int)floorf(sy);
    float sum = 0.f;
    float wsum = 0.f;

    for (int j = -1; j <= 2; j++) {
        for (int i = -1; i <= 2; i++) {
            int px = ix + i;
            int py = iy + j;
            float w = lanczos2_weight(sx - (float)px) * lanczos2_weight(sy - (float)py);
            if (w == 0.f)
                continue;
            px = px < 0 ? 0 : (px >= src_w ? src_w - 1 : px);
            py = py < 0 ? 0 : (py >= src_h ? src_h - 1 : py);
            sum += w * (float)load_u16(src, src_linesize, px, py);
            wsum += w;
        }
    }
    return wsum > 0.f ? sum / wsum : 0.f;
}

__global__ void MixingFill_Y_U16(
    unsigned char *dst, int dst_linesize,
    int width, int height,
    unsigned short y_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    store_u16(dst, dst_linesize, x, y, y_val);
}

__global__ void MixingFill_UV_P010(
    unsigned char *dst, int dst_linesize,
    int width, int height,
    unsigned short u_val, unsigned short v_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = width >> 1;
    int uv_h = height >> 1;
    if (x >= uv_w || y >= uv_h)
        return;
    store_u16(dst, dst_linesize, (x << 1), y, u_val);
    store_u16(dst, dst_linesize, (x << 1) + 1, y, v_val);
}

__global__ void MixingCopy_Y_U16(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    store_u16(dst, dst_linesize, x, y, load_u16(src, src_linesize, x, y));
}

__global__ void MixingCopy_UV_P010(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = width >> 1;
    int uv_h = height >> 1;
    if (x >= uv_w || y >= uv_h)
        return;
    store_u16(dst, dst_linesize, (x << 1), y,
              load_u16(src, src_linesize, (x << 1), y));
    store_u16(dst, dst_linesize, (x << 1) + 1, y,
              load_u16(src, src_linesize, (x << 1) + 1, y));
}

__global__ void MixingScaleBlit_Y_U16(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;
    if (!mixing_inside_rounded_rect(x, y, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float v = sample_plane_bilinear_u16(src, src_linesize, src_w, src_h, sx, sy);
    store_u16(dst, dst_linesize, dx + x, dy + y, clamp_u16(v));
}

__global__ void MixingScaleBlit_Y_U16_Lanczos(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;
    if (!mixing_inside_rounded_rect(x, y, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float v = sample_plane_lanczos2_u16(src, src_linesize, src_w, src_h, sx, sy);
    store_u16(dst, dst_linesize, dx + x, dy + y, clamp_u16(v));
}

__global__ void MixingScaleBlit_UV_P010(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;
    lx = x << 1;
    ly = y << 1;
    if (!mixing_inside_rounded_rect(lx, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx, ly + 1, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly + 1, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;

    for (int c = 0; c < 2; c++) {
        int x0 = (int)floorf(sx);
        int y0 = (int)floorf(sy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float fx = sx - (float)x0;
        float fy = sy - (float)y0;

        x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
        x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
        y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);

        float v00 = load_u16(src, src_linesize, (x0 << 1) + c, y0);
        float v10 = load_u16(src, src_linesize, (x1 << 1) + c, y0);
        float v01 = load_u16(src, src_linesize, (x0 << 1) + c, y1);
        float v11 = load_u16(src, src_linesize, (x1 << 1) + c, y1);
        float v = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                + (1.f - fx) * fy * v01 + fx * fy * v11;
        store_u16(dst, dst_linesize, (((dx >> 1) + x) << 1) + c, (dy >> 1) + y,
                  clamp_u16(v));
    }
}

__global__ void MixingScaleBlit_UV_P010_Lanczos(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;
    lx = x << 1;
    ly = y << 1;
    if (!mixing_inside_rounded_rect(lx, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx, ly + 1, dw, dh, radius) &&
        !mixing_inside_rounded_rect(lx + 1, ly + 1, dw, dh, radius))
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;

    for (int c = 0; c < 2; c++) {
        int ix = (int)floorf(sx);
        int iy = (int)floorf(sy);
        float sum = 0.f;
        float wsum = 0.f;

        for (int j = -1; j <= 2; j++) {
            for (int i = -1; i <= 2; i++) {
                int px = ix + i;
                int py = iy + j;
                float w = lanczos2_weight(sx - (float)px) * lanczos2_weight(sy - (float)py);
                if (w == 0.f)
                    continue;
                px = px < 0 ? 0 : (px >= src_uv_w ? src_uv_w - 1 : px);
                py = py < 0 ? 0 : (py >= src_uv_h ? src_uv_h - 1 : py);
                sum += w * (float)load_u16(src, src_linesize, (px << 1) + c, py);
                wsum += w;
            }
        }
        float v = wsum > 0.f ? sum / wsum : 0.f;
        store_u16(dst, dst_linesize, (((dx >> 1) + x) << 1) + c, (dy >> 1) + y,
                  clamp_u16(v));
    }
}

__global__ void MixingScaleBlitAlpha_Y_U16(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh || dw <= 0 || dh <= 0)
        return;

    float sx = ((float)x + 0.5f) * (float)src_w / (float)dw - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_h / (float)dh - 0.5f;
    float sv = sample_plane_bilinear_u16(src, src_linesize, src_w, src_h, sx, sy);
    int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
    float dv = load_u16(dst, dst_linesize, dx + x, dy + y);
    store_u16(dst, dst_linesize, dx + x, dy + y,
              clamp_u16((dv * (255 - a) + sv * a) / 255.f));
}

__global__ void MixingScaleBlitAlpha_UV_P010(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    if (x >= slot_uv_w || y >= slot_uv_h || slot_uv_w <= 0 || slot_uv_h <= 0)
        return;

    float sx = ((float)x + 0.5f) * (float)src_uv_w / (float)slot_uv_w - 0.5f;
    float sy = ((float)y + 0.5f) * (float)src_uv_h / (float)slot_uv_h - 0.5f;
    int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);

    for (int c = 0; c < 2; c++) {
        int x0 = (int)floorf(sx);
        int y0 = (int)floorf(sy);
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float fx = sx - (float)x0;
        float fy = sy - (float)y0;

        x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
        x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
        y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);

        float v00 = load_u16(src, src_linesize, (x0 << 1) + c, y0);
        float v10 = load_u16(src, src_linesize, (x1 << 1) + c, y0);
        float v01 = load_u16(src, src_linesize, (x0 << 1) + c, y1);
        float v11 = load_u16(src, src_linesize, (x1 << 1) + c, y1);
        float sv = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                 + (1.f - fx) * fy * v01 + fx * fy * v11;
        int dxu = (((dx >> 1) + x) << 1) + c;
        int dyu = (dy >> 1) + y;
        float dv = load_u16(dst, dst_linesize, dxu, dyu);
        store_u16(dst, dst_linesize, dxu, dyu,
                  clamp_u16((dv * (255 - a) + sv * a) / 255.f));
    }
}

__global__ void MixingScaleBlitOffsetAlpha_Y_U16(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int clip_x, int clip_y, int clip_w, int clip_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int gx, gy;
    if (x >= clip_w || y >= clip_h || clip_w <= 0 || clip_h <= 0 || dw <= 0 || dh <= 0)
        return;
    gx = clip_x + x;
    gy = clip_y + y;
    if (gx < dx || gy < dy || gx >= dx + dw || gy >= dy + dh)
        return;

    {
        float sx = ((float)(gx - dx) + 0.5f) * (float)src_w / (float)dw - 0.5f;
        float sy = ((float)(gy - dy) + 0.5f) * (float)src_h / (float)dh - 0.5f;
        float sv = sample_plane_bilinear_u16(src, src_linesize, src_w, src_h, sx, sy);
        int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
        if (a >= 255) {
            store_u16(dst, dst_linesize, gx, gy, clamp_u16(sv));
        } else if (a > 0) {
            float dv = load_u16(dst, dst_linesize, gx, gy);
            store_u16(dst, dst_linesize, gx, gy,
                      clamp_u16((dv * (255 - a) + sv * a) / 255.f));
        }
    }
}

__global__ void MixingScaleBlitOffsetAlpha_UV_P010(
    unsigned char *dst, int dst_linesize,
    const unsigned char *src, int src_linesize,
    int src_w, int src_h,
    int clip_x, int clip_y, int clip_w, int clip_h,
    int dx, int dy, int dw, int dh,
    int alpha_q8)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int clip_uv_w = clip_w >> 1;
    int clip_uv_h = clip_h >> 1;
    int src_uv_w = src_w >> 1;
    int src_uv_h = src_h >> 1;
    int gx, gy, lx, ly;
    if (x >= clip_uv_w || y >= clip_uv_h || clip_uv_w <= 0 || clip_uv_h <= 0 ||
        dw < 2 || dh < 2)
        return;
    gx = (clip_x >> 1) + x;
    gy = (clip_y >> 1) + y;
    lx = gx - (dx >> 1);
    ly = gy - (dy >> 1);
    if (lx < 0 || ly < 0 || lx >= (dw >> 1) || ly >= (dh >> 1))
        return;

    {
        float sx = ((float)lx + 0.5f) * (float)src_uv_w / (float)(dw >> 1) - 0.5f;
        float sy = ((float)ly + 0.5f) * (float)src_uv_h / (float)(dh >> 1) - 0.5f;
        int a = alpha_q8 < 0 ? 0 : (alpha_q8 > 255 ? 255 : alpha_q8);
        for (int c = 0; c < 2; c++) {
            int x0 = (int)floorf(sx);
            int y0 = (int)floorf(sy);
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            float fx = sx - (float)x0;
            float fy = sy - (float)y0;
            x0 = x0 < 0 ? 0 : (x0 >= src_uv_w ? src_uv_w - 1 : x0);
            x1 = x1 < 0 ? 0 : (x1 >= src_uv_w ? src_uv_w - 1 : x1);
            y0 = y0 < 0 ? 0 : (y0 >= src_uv_h ? src_uv_h - 1 : y0);
            y1 = y1 < 0 ? 0 : (y1 >= src_uv_h ? src_uv_h - 1 : y1);
            float v00 = load_u16(src, src_linesize, (x0 << 1) + c, y0);
            float v10 = load_u16(src, src_linesize, (x1 << 1) + c, y0);
            float v01 = load_u16(src, src_linesize, (x0 << 1) + c, y1);
            float v11 = load_u16(src, src_linesize, (x1 << 1) + c, y1);
            float sv = (1.f - fx) * (1.f - fy) * v00 + fx * (1.f - fy) * v10
                     + (1.f - fx) * fy * v01 + fx * fy * v11;
            int di_x = (gx << 1) + c;
            if (a >= 255) {
                store_u16(dst, dst_linesize, di_x, gy, clamp_u16(sv));
            } else if (a > 0) {
                float dv = load_u16(dst, dst_linesize, di_x, gy);
                store_u16(dst, dst_linesize, di_x, gy,
                          clamp_u16((dv * (255 - a) + sv * a) / 255.f));
            }
        }
    }
}

__global__ void MixingHighlightBorder_Y_U16(
    unsigned char *dst, int dst_linesize,
    int canvas_w, int canvas_h,
    int dx, int dy, int dw, int dh,
    int border_px, int radius, unsigned short y_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dw || y >= dh)
        return;

    if (!mixing_in_rounded_border(x, y, dw, dh, border_px, radius))
        return;

    int gx = dx + x;
    int gy = dy + y;
    if (gx < 0 || gy < 0 || gx >= canvas_w || gy >= canvas_h)
        return;
    store_u16(dst, dst_linesize, gx, gy, y_val);
}

__global__ void MixingHighlightBorder_UV_P010(
    unsigned char *dst, int dst_linesize,
    int canvas_w, int canvas_h,
    int dx, int dy, int dw, int dh,
    int border_px, int radius, unsigned short u_val, unsigned short v_val)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int uv_w = canvas_w >> 1;
    int uv_h = canvas_h >> 1;
    int slot_uv_w = dw >> 1;
    int slot_uv_h = dh >> 1;
    int lx, ly;
    if (x >= slot_uv_w || y >= slot_uv_h)
        return;

    lx = x << 1;
    ly = y << 1;
    if (!mixing_in_rounded_border(lx, ly, dw, dh, border_px, radius) &&
        !mixing_in_rounded_border(lx + 1, ly, dw, dh, border_px, radius))
        return;

    int gx = (dx >> 1) + x;
    int gy = (dy >> 1) + y;
    if (gx < 0 || gy < 0 || gx >= uv_w || gy >= uv_h)
        return;
    store_u16(dst, dst_linesize, (gx << 1), gy, u_val);
    store_u16(dst, dst_linesize, (gx << 1) + 1, gy, v_val);
}

}
