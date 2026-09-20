/*
 * Single-writer refcounted pointer ring.
 */

#include "ffmpeg_http_ring.h"

#include <string.h>

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

void http_ring_init(HttpPtrRing *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    atomic_init(&r->wpos, 0);
    atomic_init(&r->gop_pos, 0);
}

void http_ring_push(HttpPtrRing *r, HttpLiveBuf *b)
{
    unsigned pos, slot;
    HttpLiveBuf *old;

    if (!r || !b)
        return;
    pos = atomic_load_explicit(&r->wpos, memory_order_relaxed);
    slot = pos % HTTP_RING_CAP;
    old = r->slots[slot];
    r->slots[slot] = http_livebuf_ref(b);
    atomic_store_explicit(&r->wpos, pos + 1, memory_order_release);
    if (old)
        http_livebuf_unref(&old);
}

HttpLiveBuf *http_ring_get(HttpPtrRing *r, unsigned idx)
{
    unsigned w, slot;
    HttpLiveBuf *b;

    if (!r)
        return NULL;
    w = atomic_load_explicit(&r->wpos, memory_order_acquire);
    if (idx == w || (unsigned)(w - idx) > HTTP_RING_CAP)
        return NULL;
    slot = idx % HTTP_RING_CAP;
    b = r->slots[slot];
    if (!b)
        return NULL;
    http_livebuf_ref(b);
    /* Writer may have overwritten this slot after we loaded the pointer.
     * Re-check so we never send a future fragment as the current one. */
    w = atomic_load_explicit(&r->wpos, memory_order_acquire);
    if (idx == w || (unsigned)(w - idx) > HTTP_RING_CAP) {
        http_livebuf_unref(&b);
        return NULL;
    }
    return b;
}

int http_ring_stale(const HttpPtrRing *r, unsigned idx)
{
    unsigned w;

    if (!r)
        return 1;
    w = atomic_load_explicit(&r->wpos, memory_order_acquire);
    /* Caught up (idx == w) is empty, not stale. Overwritten only. */
    return idx != w && (unsigned)(w - idx) > HTTP_RING_CAP;
}

unsigned http_ring_valid_gop(const HttpPtrRing *r)
{
    unsigned w, oldest, i;

    if (!r)
        return 0;
    w = atomic_load_explicit(&r->wpos, memory_order_acquire);
    oldest = w > HTTP_RING_CAP ? w - HTTP_RING_CAP : 0;
    if (w == oldest)
        return w;
    /* Prefer last keyframe fragment still in the window. Do not fall back
     * to oldest: a P-frame / mid-moof start is Invalid NAL 0 / no frame. */
    for (i = w; i > oldest; i--) {
        HttpLiveBuf *b = r->slots[(i - 1) % HTTP_RING_CAP];
        if (b && b->is_key)
            return i - 1;
    }
    return w;
}

unsigned http_ring_oldest(const HttpPtrRing *r)
{
    unsigned w = http_ring_wpos(r);
    return w > HTTP_RING_CAP ? w - HTTP_RING_CAP : 0;
}

unsigned http_ring_wpos(const HttpPtrRing *r)
{
    return r ? atomic_load_explicit(&r->wpos, memory_order_acquire) : 0;
}

unsigned http_ring_gop(const HttpPtrRing *r)
{
    return r ? atomic_load_explicit(&r->gop_pos, memory_order_acquire) : 0;
}

void http_ring_mark_gop(HttpPtrRing *r, unsigned pos)
{
    if (r)
        atomic_store_explicit(&r->gop_pos, pos, memory_order_release);
}

void http_ring_clear(HttpPtrRing *r)
{
    int i;

    if (!r)
        return;
    for (i = 0; i < HTTP_RING_CAP; i++) {
        if (r->slots[i])
            http_livebuf_unref(&r->slots[i]);
    }
    atomic_store_explicit(&r->wpos, 0, memory_order_relaxed);
    atomic_store_explicit(&r->gop_pos, 0, memory_order_relaxed);
}
