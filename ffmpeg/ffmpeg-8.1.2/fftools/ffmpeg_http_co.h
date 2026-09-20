/*
 * Per-connection stackful coroutine. Resume only on the creating EventWorker.
 */

#ifndef FFTOOLS_FFMPEG_HTTP_CO_H
#define FFTOOLS_FFMPEG_HTTP_CO_H

typedef struct HttpCo HttpCo;

HttpCo *http_co_create(void (*fn)(void *), void *arg);
void http_co_destroy(HttpCo **co);
void http_co_resume(HttpCo *co);
void http_co_yield(void);
int  http_co_done(const HttpCo *co);
HttpCo *http_co_current(void);

/* Worker thread must call once before resume (Windows Fiber host). */
void http_co_thread_init(void);

#endif /* FFTOOLS_FFMPEG_HTTP_CO_H */
