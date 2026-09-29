/*
 * Single-writer refcounted pointer ring.
 */

#include "ffmpeg_http_ring.h"

#include <errno.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"

HttpLiveBuf *http_livebuf_alloc(const uint8_t *data, int size, int is_key)
{
    HttpLiveBuf *b;

    if (!data || size <= 0)
        return NULL;
    b = av_mallocz(sizeof(*b));
    if (!b)
        return NULL;
    b->data = av_malloc(size);
    if (!b->data) {
        av_free(b);
        return NULL;
    }
    memcpy(b->data, data, size);
    b->size = size;
    b->is_key = is_key;
    atomic_init(&b->ref, 1);
    return b;
}

HttpLiveBuf *http_livebuf_ref(HttpLiveBuf *b)
{
    if (!b)
        return NULL;
    atomic_fetch_add_explicit(&b->ref, 1, memory_order_acq_rel);
    return b;
}

void http_livebuf_unref(HttpLiveBuf **pb)
{
    HttpLiveBuf *b = pb ? *pb : NULL;
    int left;

    if (!b)
        return;
    left = atomic_fetch_sub_explicit(&b->ref, 1, memory_order_acq_rel) - 1;
    if (left <= 0) {
        av_freep(&b->data);
        av_free(b);
    }
    if (pb)
        *pb = NULL;
}

int http_ring_init(HttpPtrRing *r, unsigned capacity, uint64_t max_bytes)
{
    int ret;

    if (!r || !capacity || !max_bytes)
        return AVERROR(EINVAL);
    memset(r, 0, sizeof(*r));
    atomic_init(&r->wpos, 0);
    atomic_init(&r->oldest, 0);
    r->slots = av_calloc(capacity, sizeof(*r->slots));
    if (!r->slots)
        return AVERROR(ENOMEM);
    ret = pthread_mutex_init(&r->lock, NULL);
    if (ret) {
        av_freep(&r->slots);
        return AVERROR(ret);
    }
    r->capacity = capacity;
    r->max_bytes = max_bytes;
    r->latest_sync = HTTP_RING_NO_SYNC;
    return 0;
}

static uint64_t http_ring_find_latest_sync(const HttpPtrRing *r)
{
    uint64_t seq;
    uint64_t oldest = atomic_load_explicit(&r->oldest, memory_order_relaxed);

    for (seq = atomic_load_explicit(&r->wpos, memory_order_relaxed);
         seq > oldest; seq--) {
        const HttpRingSlot *slot = &r->slots[(seq - 1) % r->capacity];

        if (slot->buf && slot->seq == seq - 1 && slot->buf->is_key)
            return seq - 1;
    }
    return HTTP_RING_NO_SYNC;
}

static void http_ring_evict_oldest(HttpPtrRing *r)
{
    HttpRingSlot *slot;
    uint64_t oldest = atomic_load_explicit(&r->oldest, memory_order_relaxed);
    int evicted_latest_sync = 0;

    if (oldest == atomic_load_explicit(&r->wpos, memory_order_relaxed))
        return;
    slot = &r->slots[oldest % r->capacity];
    if (slot->buf && slot->seq == oldest) {
        evicted_latest_sync = slot->seq == r->latest_sync;
        r->bytes -= slot->buf->size;
        http_livebuf_unref(&slot->buf);
    }
    atomic_store_explicit(&r->oldest, oldest + 1, memory_order_release);
    if (evicted_latest_sync)
        r->latest_sync = http_ring_find_latest_sync(r);
}

void http_ring_push(HttpPtrRing *r, HttpLiveBuf *b)
{
    HttpRingSlot *slot;
    uint64_t seq;

    if (!r || !r->slots || !b)
        return;
    pthread_mutex_lock(&r->lock);
    while (atomic_load_explicit(&r->wpos, memory_order_relaxed) -
           atomic_load_explicit(&r->oldest, memory_order_relaxed) >= r->capacity)
        http_ring_evict_oldest(r);
    seq = atomic_load_explicit(&r->wpos, memory_order_relaxed);
    slot = &r->slots[seq % r->capacity];
    slot->buf = http_livebuf_ref(b);
    slot->seq = seq;
    r->bytes += b->size;
    atomic_store_explicit(&r->wpos, seq + 1, memory_order_release);
    if (b->is_key)
        r->latest_sync = seq;
    while (r->bytes > r->max_bytes)
        http_ring_evict_oldest(r);
    pthread_mutex_unlock(&r->lock);
}

int http_ring_get(HttpPtrRing *r, uint64_t seq, HttpLiveBuf **out)
{
    HttpRingSlot *slot;
    int ret = 0;

    if (out)
        *out = NULL;
    if (!r || !r->slots || !out)
        return AVERROR(EINVAL);
    pthread_mutex_lock(&r->lock);
    if (seq < atomic_load_explicit(&r->oldest, memory_order_relaxed)) {
        ret = AVERROR(ESTALE);
    } else if (seq >= atomic_load_explicit(&r->wpos, memory_order_relaxed)) {
        ret = AVERROR(EAGAIN);
    } else {
        slot = &r->slots[seq % r->capacity];
        if (!slot->buf || slot->seq != seq)
            ret = AVERROR(ESTALE);
        else
            *out = http_livebuf_ref(slot->buf);
    }
    pthread_mutex_unlock(&r->lock);
    return ret;
}

uint64_t http_ring_wpos(const HttpPtrRing *r)
{
    if (!r)
        return 0;
    return atomic_load_explicit(&r->wpos, memory_order_acquire);
}

uint64_t http_ring_oldest(const HttpPtrRing *r)
{
    if (!r)
        return 0;
    return atomic_load_explicit(&r->oldest, memory_order_acquire);
}

uint64_t http_ring_latest_sync(HttpPtrRing *r)
{
    uint64_t latest_sync;

    if (!r)
        return HTTP_RING_NO_SYNC;
    pthread_mutex_lock(&r->lock);
    latest_sync = r->latest_sync;
    pthread_mutex_unlock(&r->lock);
    return latest_sync;
}

void http_ring_clear(HttpPtrRing *r)
{
    if (!r || !r->slots)
        return;
    pthread_mutex_lock(&r->lock);
    while (atomic_load_explicit(&r->oldest, memory_order_relaxed) !=
           atomic_load_explicit(&r->wpos, memory_order_relaxed))
        http_ring_evict_oldest(r);
    r->bytes = 0;
    r->latest_sync = HTTP_RING_NO_SYNC;
    pthread_mutex_unlock(&r->lock);
}

void http_ring_destroy(HttpPtrRing *r)
{
    if (!r || !r->slots)
        return;
    pthread_mutex_lock(&r->lock);
    while (atomic_load_explicit(&r->oldest, memory_order_relaxed) !=
           atomic_load_explicit(&r->wpos, memory_order_relaxed))
        http_ring_evict_oldest(r);
    av_freep(&r->slots);
    r->capacity = 0;
    r->max_bytes = 0;
    r->bytes = 0;
    atomic_store_explicit(&r->wpos, 0, memory_order_relaxed);
    atomic_store_explicit(&r->oldest, 0, memory_order_relaxed);
    r->latest_sync = HTTP_RING_NO_SYNC;
    pthread_mutex_unlock(&r->lock);
    pthread_mutex_destroy(&r->lock);
}
