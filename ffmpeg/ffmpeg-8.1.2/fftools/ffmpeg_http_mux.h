/*
 * Timestamp rebasing and marker-driven HTTP mux output assembly.
 */

#ifndef FFTOOLS_FFMPEG_HTTP_MUX_H
#define FFTOOLS_FFMPEG_HTTP_MUX_H

#include <stdint.h>

#include "libavcodec/packet.h"
#include "libavformat/avio.h"
#include "ffmpeg_http_ring.h"

#define HTTP_MUX_MAX_HEADER_SIZE (1 * 1024 * 1024)
#define HTTP_MUX_MAX_MEDIA_UNIT_SIZE (64 * 1024 * 1024)

typedef struct HttpMuxOutput {
    HttpPtrRing *ring;
    uint8_t *header_data;
    int header_size;
    int header_capacity;
    uint8_t *unit_data;
    int unit_size;
    int unit_capacity;
    enum AVIODataMarkerType unit_type;
    int have_unit;
    uint32_t state;
} HttpMuxOutput;

int http_mux_packet_prepare(AVPacket *pkt, AVRational src_tb,
                            AVRational dst_tb, int64_t epoch_us);
/*
 * init may be called on arbitrary storage and establishes a fresh state.
 * destroy is valid only after init, or on an all-zero object. Call destroy
 * before reinitializing a live object.
 */
void http_mux_output_init(HttpMuxOutput *out, HttpPtrRing *ring);
void http_mux_output_destroy(HttpMuxOutput *out);
int http_mux_write_data_type(void *opaque, const uint8_t *buf, int size,
                             enum AVIODataMarkerType type, int64_t time);
int http_mux_output_finish_header(HttpMuxOutput *out, HttpLiveBuf **header);
/* Publish (nonzero) or drop (zero) the final accumulated media unit. */
int http_mux_output_finish(HttpMuxOutput *out, int publish_media);

#endif /* FFTOOLS_FFMPEG_HTTP_MUX_H */
