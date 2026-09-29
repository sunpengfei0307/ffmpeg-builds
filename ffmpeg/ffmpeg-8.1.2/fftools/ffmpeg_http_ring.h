/*
 * Single-writer pointer ring of refcounted media buffers.
 * Slots hold HttpLiveBuf* only — never copies of packet payload.
 */

#ifndef FFTOOLS_FFMPEG_HTTP_RING_H
#define FFTOOLS_FFMPEG_HTTP_RING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define HTTP_RING_DEFAULT_CAPACITY 4096
#define HTTP_RING_DEFAULT_MAX_BYTES (64ULL * 1024 * 1024)
#define HTTP_RING_NO_SYNC UINT64_MAX

typedef struct HttpLiveBuf {
    atomic_int ref;
    uint8_t *data;
    int size;
    int is_key;
} HttpLiveBuf;

typedef struct HttpRingSlot {
    HttpLiveBuf *buf;
    uint64_t seq;
} HttpRingSlot;

typedef struct HttpPtrRing {
    HttpRingSlot *slots;
    unsigned capacity;
    uint64_t max_bytes;
    uint64_t bytes;
    atomic_uint_fast64_t wpos;
    atomic_uint_fast64_t oldest;
    uint64_t latest_sync;
    pthread_mutex_t lock;
} HttpPtrRing;

HttpLiveBuf *http_livebuf_alloc(const uint8_t *data, int size, int is_key);
HttpLiveBuf *http_livebuf_ref(HttpLiveBuf *b);
void http_livebuf_unref(HttpLiveBuf **b);

int http_ring_init(HttpPtrRing *r, unsigned capacity, uint64_t max_bytes);
void http_ring_push(HttpPtrRing *r, HttpLiveBuf *b);
int http_ring_get(HttpPtrRing *r, uint64_t seq, HttpLiveBuf **out);
uint64_t http_ring_wpos(const HttpPtrRing *r);
uint64_t http_ring_oldest(const HttpPtrRing *r);
uint64_t http_ring_latest_sync(HttpPtrRing *r);
/* Reset retained data while keeping the lock and allocation usable. */
void http_ring_clear(HttpPtrRing *r);
/* Final release; callers must first stop all ring users. */
void http_ring_destroy(HttpPtrRing *r);

#endif /* FFTOOLS_FFMPEG_HTTP_RING_H */
