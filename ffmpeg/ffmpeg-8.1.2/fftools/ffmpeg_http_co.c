/*
 * Stackful coroutine: ucontext (Linux) / Fiber (Windows).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include "ffmpeg_http_co.h"

#include <stdlib.h>
#include <string.h>

#include "libavutil/mem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define HTTP_CO_STACK  (64 * 1024)
#define HTTP_CO_GUARD  4096

struct HttpCo {
#ifdef _WIN32
    void *fiber;
#else
    ucontext_t ctx;
    void *map;
    size_t map_sz;
#endif
    void (*fn)(void *);
    void *arg;
    int done;
};

#ifdef _WIN32
static __thread void *tls_host_fiber;
#else
static __thread ucontext_t *tls_host;
#endif
static __thread HttpCo *tls_cur;

void http_co_thread_init(void)
{
#ifdef _WIN32
    if (!IsThreadAFiber())
        ConvertThreadToFiber(NULL);
    tls_host_fiber = GetCurrentFiber();
#endif
}

#ifndef _WIN32
static void http_co_entry(void)
{
    HttpCo *co = tls_cur;
    if (co && co->fn)
        co->fn(co->arg);
    if (co)
        co->done = 1;
    if (tls_host)
        swapcontext(&co->ctx, tls_host);
}
#endif

#ifdef _WIN32
static VOID CALLBACK http_co_fiber_entry(PVOID arg)
{
    HttpCo *co = arg;
    tls_cur = co;
    if (co && co->fn)
        co->fn(co->arg);
    if (co)
        co->done = 1;
    SwitchToFiber(tls_host_fiber);
}
#endif

HttpCo *http_co_create(void (*fn)(void *), void *arg)
{
    HttpCo *co = av_mallocz(sizeof(*co));

    if (!co || !fn)
        return co ? (av_free(co), NULL) : NULL;
    co->fn = fn;
    co->arg = arg;
#ifdef _WIN32
    co->fiber = CreateFiber(HTTP_CO_STACK, http_co_fiber_entry, co);
    if (!co->fiber) {
        av_free(co);
        return NULL;
    }
#else
    {
        long page = sysconf(_SC_PAGESIZE);
        size_t guard = page > 0 ? (size_t)page : HTTP_CO_GUARD;
        size_t total = guard + HTTP_CO_STACK;

        co->map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (co->map == MAP_FAILED) {
            av_free(co);
            return NULL;
        }
        co->map_sz = total;
        if (mprotect(co->map, guard, PROT_NONE) < 0) {
            munmap(co->map, total);
            av_free(co);
            return NULL;
        }
        if (getcontext(&co->ctx) < 0) {
            munmap(co->map, total);
            av_free(co);
            return NULL;
        }
        co->ctx.uc_stack.ss_sp = (char *)co->map + guard;
        co->ctx.uc_stack.ss_size = HTTP_CO_STACK;
        co->ctx.uc_link = NULL;
        makecontext(&co->ctx, http_co_entry, 0);
    }
#endif
    return co;
}

void http_co_destroy(HttpCo **pco)
{
    HttpCo *co = pco ? *pco : NULL;

    if (!co)
        return;
#ifdef _WIN32
    if (co->fiber)
        DeleteFiber(co->fiber);
#else
    if (co->map && co->map_sz)
        munmap(co->map, co->map_sz);
#endif
    av_free(co);
    if (pco)
        *pco = NULL;
}

void http_co_resume(HttpCo *co)
{
    if (!co || co->done)
        return;
#ifdef _WIN32
    tls_host_fiber = GetCurrentFiber();
    tls_cur = co;
    SwitchToFiber(co->fiber);
    tls_cur = NULL;
#else
    {
        ucontext_t host;
        tls_host = &host;
        tls_cur = co;
        swapcontext(&host, &co->ctx);
        tls_cur = NULL;
        tls_host = NULL;
    }
#endif
}

void http_co_yield(void)
{
    HttpCo *co = tls_cur;

    if (!co)
        return;
#ifdef _WIN32
    SwitchToFiber(tls_host_fiber);
#else
    if (tls_host)
        swapcontext(&co->ctx, tls_host);
#endif
}

int http_co_done(const HttpCo *co)
{
    return co ? co->done : 1;
}

HttpCo *http_co_current(void)
{
    return tls_cur;
}
