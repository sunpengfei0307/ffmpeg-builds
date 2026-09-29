/*
 * Timestamp rebasing and marker-driven HTTP mux output assembly.
 */

#include "ffmpeg_http_mux.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/mathematics.h"

#define HTTP_MUX_OUTPUT_STATE 0x48584d4fU
#define HTTP_MUX_INITIAL_CAPACITY 4096

static int http_mux_subtraction_overflows(int64_t value, int64_t off)
{
    return (off > 0 && value < INT64_MIN + off) ||
           (off < 0 && value > INT64_MAX + off);
}

int http_mux_packet_prepare(AVPacket *pkt, AVRational src_tb,
                            AVRational dst_tb, int64_t epoch_us)
{
    int64_t off;

    if (!pkt || src_tb.num <= 0 || src_tb.den <= 0 ||
        dst_tb.num <= 0 || dst_tb.den <= 0)
        return AVERROR(EINVAL);
    off = av_rescale_q(epoch_us, AV_TIME_BASE_Q, src_tb);
    if ((pkt->pts != AV_NOPTS_VALUE &&
         http_mux_subtraction_overflows(pkt->pts, off)) ||
        (pkt->dts != AV_NOPTS_VALUE &&
         http_mux_subtraction_overflows(pkt->dts, off)))
        return AVERROR(ERANGE);
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= off;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= off;
    av_packet_rescale_ts(pkt, src_tb, dst_tb);
    return 0;
}

void http_mux_output_init(HttpMuxOutput *out, HttpPtrRing *ring)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->ring = ring;
    out->state = HTTP_MUX_OUTPUT_STATE;
}

void http_mux_output_destroy(HttpMuxOutput *out)
{
    if (!out)
        return;
    if (out->state == HTTP_MUX_OUTPUT_STATE) {
        av_freep(&out->header_data);
        av_freep(&out->unit_data);
    }
    memset(out, 0, sizeof(*out));
}

static int http_mux_append(uint8_t **dst, int *dst_size, int *dst_capacity,
                           int max_size, const uint8_t *src, int src_size)
{
    uint8_t *tmp;
    int capacity;
    int needed;

    if (src_size < 0 || (src_size && !src))
        return AVERROR(EINVAL);
    if (src_size > max_size - *dst_size)
        return AVERROR(ENOSPC);
    if (!src_size)
        return 0;
    needed = *dst_size + src_size;
    if (needed > *dst_capacity) {
        capacity = *dst_capacity ? *dst_capacity :
                   (HTTP_MUX_INITIAL_CAPACITY < max_size ?
                    HTTP_MUX_INITIAL_CAPACITY : max_size);
        while (capacity < needed) {
            if (capacity > max_size / 2)
                capacity = max_size;
            else
                capacity *= 2;
        }
        tmp = av_realloc(*dst, capacity);
        if (!tmp)
            return AVERROR(ENOMEM);
        *dst = tmp;
        *dst_capacity = capacity;
    }
    memcpy(*dst + *dst_size, src, src_size);
    *dst_size += src_size;
    return 0;
}

static void http_mux_discard_unit(HttpMuxOutput *out)
{
    av_freep(&out->unit_data);
    out->unit_size = 0;
    out->unit_capacity = 0;
    out->have_unit = 0;
}

static int http_mux_finalize_unit(HttpMuxOutput *out)
{
    HttpLiveBuf *unit;
    int ret;

    if (!out->have_unit || !out->unit_size) {
        http_mux_discard_unit(out);
        return 0;
    }
    if (out->unit_type == AVIO_DATA_MARKER_HEADER) {
        ret = http_mux_append(&out->header_data, &out->header_size,
                              &out->header_capacity,
                              HTTP_MUX_MAX_HEADER_SIZE,
                              out->unit_data, out->unit_size);
        if (ret < 0)
            return ret;
    } else {
        if (!out->ring)
            return AVERROR(EINVAL);
        unit = http_livebuf_alloc(out->unit_data, out->unit_size,
                                  out->unit_type ==
                                  AVIO_DATA_MARKER_SYNC_POINT);
        if (!unit)
            return AVERROR(ENOMEM);
        http_ring_push(out->ring, unit);
        http_livebuf_unref(&unit);
    }
    http_mux_discard_unit(out);
    return 0;
}

int http_mux_write_data_type(void *opaque, const uint8_t *buf, int size,
                             enum AVIODataMarkerType type, int64_t time)
{
    HttpMuxOutput *out = opaque;
    int starts_unit;
    int ret;

    (void)time;
    if (!out || out->state != HTTP_MUX_OUTPUT_STATE ||
        size < 0 || (size && !buf))
        return AVERROR(EINVAL);
    if (type == AVIO_DATA_MARKER_TRAILER) {
        ret = http_mux_finalize_unit(out);
        return ret < 0 ? ret : size;
    }
    starts_unit = type == AVIO_DATA_MARKER_HEADER ||
                  type == AVIO_DATA_MARKER_SYNC_POINT ||
                  type == AVIO_DATA_MARKER_BOUNDARY_POINT;
    if (starts_unit) {
        ret = http_mux_finalize_unit(out);
        if (ret < 0)
            return ret;
        out->unit_type = type;
        out->have_unit = 1;
    } else if (!out->have_unit) {
        out->unit_type = type;
        out->have_unit = 1;
    }
    ret = http_mux_append(&out->unit_data, &out->unit_size,
                          &out->unit_capacity,
                          out->unit_type == AVIO_DATA_MARKER_HEADER ?
                          HTTP_MUX_MAX_HEADER_SIZE :
                          HTTP_MUX_MAX_MEDIA_UNIT_SIZE,
                          buf, size);
    return ret < 0 ? ret : size;
}

int http_mux_output_finish_header(HttpMuxOutput *out, HttpLiveBuf **header)
{
    int ret;

    if (header)
        *header = NULL;
    if (!out || out->state != HTTP_MUX_OUTPUT_STATE || !header)
        return AVERROR(EINVAL);
    if (out->have_unit && out->unit_type == AVIO_DATA_MARKER_HEADER) {
        ret = http_mux_finalize_unit(out);
        if (ret < 0)
            return ret;
    }
    if (!out->header_size)
        return 0;
    *header = http_livebuf_alloc(out->header_data, out->header_size, 0);
    if (!*header)
        return AVERROR(ENOMEM);
    av_freep(&out->header_data);
    out->header_size = 0;
    out->header_capacity = 0;
    return 0;
}

int http_mux_output_finish(HttpMuxOutput *out, int publish_media)
{
    if (!out || out->state != HTTP_MUX_OUTPUT_STATE ||
        (publish_media != 0 && publish_media != 1))
        return AVERROR(EINVAL);
    if (!out->have_unit)
        return 0;
    if (out->unit_type == AVIO_DATA_MARKER_HEADER || publish_media)
        return http_mux_finalize_unit(out);
    http_mux_discard_unit(out);
    return 0;
}
