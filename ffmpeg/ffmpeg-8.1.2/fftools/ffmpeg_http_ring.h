/*
 * Single-writer pointer ring of refcounted media buffers.
 * Slots hold HttpLiveBuf* only — never copies of packet payload.
 */

#ifndef FFTOOLS_FFMPEG_HTTP_RING_H
#define FFTOOLS_FFMPEG_HTTP_RING_H

#include <stdatomic.h>
#include <stdint.h>

#define HTTP_RING_CAP 512

typedef struct HttpLiveBuf {
    atomic_int ref;
    uint8_t *data;
    int size;
    int is_key;
} HttpLiveBuf;

typedef struct HttpPtrRing {
    HttpLiveBuf *slots[HTTP_RING_CAP];
    atomic_uint wpos;
    atomic_uint gop_pos;
} HttpPtrRing;

HttpLiveBuf *http_livebuf_alloc(const uint8_t *data, int size, int is_key);
HttpLiveBuf *http_livebuf_ref(HttpLiveBuf *b);
void http_livebuf_unref(HttpLiveBuf **b);

void http_ring_init(HttpPtrRing *r);
void http_ring_push(HttpPtrRing *r, HttpLiveBuf *b);
HttpLiveBuf *http_ring_get(HttpPtrRing *r, unsigned idx);
int  http_ring_stale(const HttpPtrRing *r, unsigned idx);
unsigned http_ring_wpos(const HttpPtrRing *r);
unsigned http_ring_oldest(const HttpPtrRing *r);
unsigned http_ring_gop(const HttpPtrRing *r);
unsigned http_ring_valid_gop(const HttpPtrRing *r);
void http_ring_mark_gop(HttpPtrRing *r, unsigned pos);
void http_ring_clear(HttpPtrRing *r);

#endif /* FFTOOLS_FFMPEG_HTTP_RING_H */
