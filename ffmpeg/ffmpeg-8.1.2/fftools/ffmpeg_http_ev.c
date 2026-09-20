/*
 * EventWorker pool: one epoll + SO_REUSEPORT listen per core.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"

#include "ffmpeg.h"
#include "ffmpeg_http_co.h"
#include "ffmpeg_http_ev.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define HTTP_EV_MAIL     1024
#define HTTP_EV_BACKLOG  512
#define HTTP_EV_SNDBUF   (256 * 1024)
#define HTTP_EV_TAG_LISTEN  ((void *)(intptr_t)1)
#define HTTP_EV_TAG_WAKE    ((void *)(intptr_t)2)

struct HttpEventWorker {
    int id;
    pthread_t th;
    int epfd;
    int listen_fd;
    int wakeup_r;
    int wakeup_w;
    atomic_int running;
    pthread_mutex_t mail_mu;
    HttpEvMsg mail[HTTP_EV_MAIL];
    int mail_head, mail_tail, mail_count;
    void (*session_fn)(void *);
};

static HttpEventWorker workers[HTTP_EV_MAX];
static int nb_workers;
static atomic_int ev_started;
static char listen_host[256];
static int listen_port;

#ifdef _WIN32
#define ev_close closesocket
#else
#define ev_close close
#endif

int http_ev_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return FFMAX(1, (int)si.dwNumberOfProcessors);
#else
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return n;
    }
    return FFMAX(1, (int)sysconf(_SC_NPROCESSORS_ONLN));
#endif
}

int http_ev_nb_workers(void)
{
    return nb_workers;
}

int http_ev_worker_id(const HttpEventWorker *w)
{
    return w ? w->id : -1;
}

#ifndef _WIN32
static int ev_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
#endif

static void ev_sock_tune(int fd)
{
    int one = 1, snd = HTTP_EV_SNDBUF;

    if (fd < 0)
        return;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&snd, sizeof(snd));
#ifndef _WIN32
#ifdef TCP_QUICKACK
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
#endif
}

static int ev_listen_socket(const char *host, int port)
{
    int fd, family = AF_INET, one = 1;
    struct sockaddr_storage ss;
    socklen_t slen;

    memset(&ss, 0, sizeof(ss));
    if (host && strchr(host, ':') && strcmp(host, "0.0.0.0")) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        family = AF_INET6;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons((uint16_t)port);
        if (!host[0] || !strcmp(host, "::") || !strcmp(host, "[::]"))
            in6->sin6_addr = in6addr_any;
        else if (inet_pton(AF_INET6, host, &in6->sin6_addr) != 1)
            return -1;
        slen = sizeof(*in6);
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        if (!host || !host[0] || !strcmp(host, "0.0.0.0") || !strcmp(host, "*"))
            in->sin_addr.s_addr = INADDR_ANY;
        else if (inet_pton(AF_INET, host, &in->sin_addr) != 1)
            return -1;
        slen = sizeof(*in);
    }

#ifdef _WIN32
    fd = (int)socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    {
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
    }
#else
    fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0)
        return -1;
#endif
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof(one));
#endif
    if (bind(fd, (struct sockaddr *)&ss, slen) < 0 ||
        listen(fd, HTTP_EV_BACKLOG) < 0) {
        ev_close(fd);
        return -1;
    }
#ifndef _WIN32
#ifdef TCP_DEFER_ACCEPT
    {
        int da = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &da, sizeof(da));
    }
#endif
#ifdef TCP_FASTOPEN
    {
        int qlen = 16;
        setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
    }
#endif
#endif
    return fd;
}

static int ev_epoll_add(HttpEventWorker *w, int fd, int events, void *ptr)
{
#ifdef _WIN32
    (void)w; (void)fd; (void)events; (void)ptr;
    return 0;
#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)events;
    ev.data.ptr = ptr;
    return epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev);
#endif
}

static int ev_epoll_mod(HttpEventWorker *w, int fd, int events, void *ptr)
{
#ifdef _WIN32
    (void)w; (void)fd; (void)events; (void)ptr;
    return 0;
#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (uint32_t)events;
    ev.data.ptr = ptr;
    return epoll_ctl(w->epfd, EPOLL_CTL_MOD, fd, &ev);
#endif
}

static void ev_epoll_del(HttpEventWorker *w, int fd)
{
#ifdef _WIN32
    (void)w; (void)fd;
#else
    epoll_ctl(w->epfd, EPOLL_CTL_DEL, fd, NULL);
#endif
}

static void ev_fill_peer(HttpSession *s)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    char host[64] = {0};
    int port = 0;

    s->peer[0] = 0;
    s->peer_ip[0] = 0;
    s->family = AF_UNSPEC;
    memset(s->addr, 0, sizeof(s->addr));
    if (s->fd < 0 || getpeername(s->fd, (struct sockaddr *)&ss, &slen) != 0)
        return;
    s->family = ss.ss_family;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        memcpy(s->addr, &in->sin_addr, 4);
        port = ntohs(in->sin_port);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        memcpy(s->addr, &in6->sin6_addr, 16);
        port = ntohs(in6->sin6_port);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
    }
    av_strlcpy(s->peer_ip, host, sizeof(s->peer_ip));
    snprintf(s->peer, sizeof(s->peer), "%s:%d", host, port);
}

static void ev_waiter_del(HttpSession *s);
static void ev_session_link(HttpSession *s);
static void ev_session_unlink(HttpSession *s);

static void ev_session_free(HttpSession *s)
{
    if (!s)
        return;
    ev_session_unlink(s);
    ev_waiter_del(s);
    if (s->w && s->fd >= 0)
        ev_epoll_del(s->w, s->fd);
    if (s->fd >= 0) {
        ev_close(s->fd);
        s->fd = -1;
    }
    http_co_destroy(&s->co);
    av_free(s);
}

static void ev_resume_session(HttpSession *s)
{
    if (!s || !s->co)
        return;
    http_co_resume(s->co);
    if (http_co_done(s->co) || s->closing)
        ev_session_free(s);
}

static void ev_accept_one(HttpEventWorker *w)
{
    HttpSession *s;
    int fd;

#ifdef _WIN32
    fd = (int)accept(w->listen_fd, NULL, NULL);
    if (fd >= 0) {
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
    }
#else
    fd = accept4(w->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    if (fd < 0)
        return;
    ev_sock_tune(fd);
    s = av_mallocz(sizeof(*s));
    if (!s) {
        ev_close(fd);
        return;
    }
    s->w = w;
    s->fd = fd;
    s->t0 = av_gettime_relative();
    s->live_pub = -1;
    ev_fill_peer(s);
    if (ev_epoll_add(w, fd, HTTP_EV_IN | HTTP_EV_ERR, s) < 0) {
        ev_close(fd);
        av_free(s);
        return;
    }
    s->co = http_co_create(w->session_fn, s);
    if (!s->co) {
        ev_session_free(s);
        return;
    }
    ev_session_link(s);
    ev_resume_session(s);
}

static HttpEvMsg wpending[HTTP_EV_MAX][64];
static int wpending_n[HTTP_EV_MAX];

static void ev_drain_mail_real(HttpEventWorker *w)
{
    HttpEvMsg local[64];
    int n = 0, i;

    pthread_mutex_lock(&w->mail_mu);
    while (w->mail_count > 0 && n < 64) {
        local[n++] = w->mail[w->mail_head];
        w->mail_head = (w->mail_head + 1) % HTTP_EV_MAIL;
        w->mail_count--;
    }
    pthread_mutex_unlock(&w->mail_mu);

    for (i = 0; i < n; i++) {
        if (wpending_n[w->id] < 64)
            wpending[w->id][wpending_n[w->id]++] = local[i];
    }
}

static void ev_deliver_pending(HttpEventWorker *w, HttpSession *s)
{
    int i, j;

    if (!s || !s->wait_msg)
        return;
    for (i = 0; i < wpending_n[w->id] && s->inbox_n < HTTP_EV_INBOX; i++) {
        HttpEvMsg m = wpending[w->id][i];
        int dup = 0;
        for (j = 0; j < s->inbox_n; j++) {
            if (s->inbox[j].type == m.type &&
                s->inbox[j].pub_idx == m.pub_idx &&
                s->inbox[j].fmt_idx == m.fmt_idx)
                dup = 1;
        }
        if (!dup)
            s->inbox[s->inbox_n++] = m;
    }
}

typedef struct EvWaiter {
    HttpSession *s;
    struct EvWaiter *next;
} EvWaiter;

static EvWaiter *waiters[HTTP_EV_MAX];
static HttpSession *sessions[HTTP_EV_MAX];

static void ev_session_link(HttpSession *s)
{
    if (!s || !s->w)
        return;
    s->next = sessions[s->w->id];
    sessions[s->w->id] = s;
}

static void ev_session_unlink(HttpSession *s)
{
    HttpSession **pp;

    if (!s || !s->w)
        return;
    for (pp = &sessions[s->w->id]; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            s->next = NULL;
            return;
        }
    }
}

static void ev_waiter_add(HttpSession *s)
{
    EvWaiter *n;
    int id;

    if (!s || !s->w)
        return;
    id = s->w->id;
    for (n = waiters[id]; n; n = n->next)
        if (n->s == s)
            return;
    n = av_mallocz(sizeof(*n));
    if (!n)
        return;
    n->s = s;
    n->next = waiters[id];
    waiters[id] = n;
}

static void ev_waiter_del(HttpSession *s)
{
    EvWaiter **pp;
    int id;

    if (!s || !s->w)
        return;
    id = s->w->id;
    for (pp = &waiters[id]; *pp; ) {
        if ((*pp)->s == s) {
            EvWaiter *x = *pp;
            *pp = x->next;
            av_free(x);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void ev_wake_waiters(HttpEventWorker *w)
{
    HttpSession *s, *next;
    int i;

    ev_drain_mail_real(w);
    for (i = 0; i < wpending_n[w->id]; i++) {
        HttpEvMsg m = wpending[w->id][i];
        if (m.type != HTTP_EV_MSG_KICK && m.type != HTTP_EV_MSG_STOP)
            continue;
        for (s = sessions[w->id]; s; s = s->next) {
            if (s->live_pub < 0)
                continue;
            if (m.type == HTTP_EV_MSG_STOP || m.pub_idx < 0 ||
                s->live_pub == m.pub_idx)
                s->kick = 1;
        }
    }
    for (s = sessions[w->id]; s; s = next) {
        next = s->next;
        ev_deliver_pending(w, s);
        if (s->kick || (s->wait_msg && s->inbox_n > 0)) {
            s->wait_msg = 0;
            ev_resume_session(s);
        }
    }
    wpending_n[w->id] = 0;
}

static void ev_affinity(int id)
{
#ifndef _WIN32
    cpu_set_t set, avail;
    int i, nth = 0, target = -1;

    if (sched_getaffinity(0, sizeof(avail), &avail) != 0)
        return;
    CPU_ZERO(&set);
    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &avail)) {
            if (nth == id) {
                target = i;
                break;
            }
            nth++;
        }
    }
    if (target < 0)
        return;
    CPU_SET(target, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    DWORD_PTR mask = (DWORD_PTR)1 << (id % 64);
    SetThreadAffinityMask(GetCurrentThread(), mask);
    (void)mask;
#endif
}

static void ev_clear_wakeup(HttpEventWorker *w)
{
#ifdef _WIN32
    char buf[64];
    recv(w->wakeup_r, buf, sizeof(buf), 0);
#else
    uint64_t v;
    while (read(w->wakeup_r, &v, sizeof(v)) > 0)
        ;
#endif
}

static void *ev_thread(void *arg)
{
    HttpEventWorker *w = arg;
#ifndef _WIN32
    struct epoll_event evs[64];
#endif

    http_co_thread_init();
    ev_affinity(w->id);
    atomic_store(&w->running, 1);

    while (atomic_load(&w->running)) {
#ifndef _WIN32
        int n, i;
        ev_wake_waiters(w);
        n = epoll_wait(w->epfd, evs, 64, 200);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (i = 0; i < n; i++) {
            void *ptr = evs[i].data.ptr;
            if (ptr == HTTP_EV_TAG_LISTEN) {
                ev_accept_one(w);
            } else if (ptr == HTTP_EV_TAG_WAKE) {
                ev_clear_wakeup(w);
                ev_wake_waiters(w);
            } else {
                HttpSession *s = ptr;
                ev_resume_session(s);
            }
        }
#else
        /* Degraded: poll listen + wakeup + no full session epoll. */
        ev_wake_waiters(w);
        ev_accept_one(w);
        av_usleep(2000);
#endif
    }
    return NULL;
}

int http_ev_post(int worker_id, HttpEvMsg msg)
{
    HttpEventWorker *w;
#ifdef _WIN32
    char one = 1;
#else
    uint64_t one = 1;
#endif

    if (worker_id < 0 || worker_id >= nb_workers)
        return AVERROR(EINVAL);
    w = &workers[worker_id];
    pthread_mutex_lock(&w->mail_mu);
    if (w->mail_count >= HTTP_EV_MAIL) {
        pthread_mutex_unlock(&w->mail_mu);
        return AVERROR(EAGAIN);
    }
    w->mail[w->mail_tail] = msg;
    w->mail_tail = (w->mail_tail + 1) % HTTP_EV_MAIL;
    w->mail_count++;
    pthread_mutex_unlock(&w->mail_mu);
#ifdef _WIN32
    send(w->wakeup_w, &one, 1, 0);
#else
    write(w->wakeup_w, &one, sizeof(one));
#endif
    return 0;
}

void http_ev_session_wait_fd(HttpSession *s, int events)
{
    if (!s || !s->w)
        return;
    s->wait_events = events | HTTP_EV_ERR;
    ev_epoll_mod(s->w, s->fd, s->wait_events, s);
    ev_waiter_del(s);
    http_co_yield();
}

void http_ev_session_wait_msg(HttpSession *s)
{
    if (!s)
        return;
    s->wait_msg = 1;
    ev_waiter_add(s);
    if (s->inbox_n > 0) {
        s->wait_msg = 0;
        return;
    }
    http_co_yield();
}

int http_ev_session_got_msg(HttpSession *s, HttpEvMsg *out)
{
    if (!s || s->inbox_n <= 0)
        return 0;
    if (out)
        *out = s->inbox[0];
    memmove(s->inbox, s->inbox + 1, (s->inbox_n - 1) * sizeof(s->inbox[0]));
    s->inbox_n--;
    return 1;
}

int http_ev_start(const char *host, int port, int nworkers,
                  void (*session_fn)(void *))
{
    int i, n, ret;

    if (atomic_load(&ev_started))
        return 0;
    if (!session_fn)
        return AVERROR(EINVAL);
    n = nworkers;
    if (n <= 0)
        n = http_ev_cpu_count();
    if (n > HTTP_EV_MAX)
        n = HTTP_EV_MAX;
    av_strlcpy(listen_host, host ? host : "0.0.0.0", sizeof(listen_host));
    listen_port = port > 0 ? port : 8080;
    memset(workers, 0, sizeof(workers));
    memset(waiters, 0, sizeof(waiters));
    memset(sessions, 0, sizeof(sessions));
    memset(wpending_n, 0, sizeof(wpending_n));

    for (i = 0; i < n; i++) {
        HttpEventWorker *w = &workers[i];
        w->id = i;
        w->session_fn = session_fn;
        w->listen_fd = -1;
        w->epfd = -1;
        w->wakeup_r = w->wakeup_w = -1;
        pthread_mutex_init(&w->mail_mu, NULL);
#ifndef _WIN32
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (w->epfd < 0)
            return AVERROR(errno);
        w->wakeup_r = w->wakeup_w = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (w->wakeup_r < 0)
            return AVERROR(errno);
#else
        {
            SOCKET a = socket(AF_INET, SOCK_DGRAM, 0);
            w->wakeup_r = (int)a;
            w->wakeup_w = (int)a;
        }
#endif
        w->listen_fd = ev_listen_socket(listen_host, listen_port);
        if (w->listen_fd < 0) {
            ffmpeg_http_log(AV_LOG_ERROR,
                            "http-server: reuseport bind %s:%d worker %d failed: %s\n",
                            listen_host, listen_port, i, strerror(errno));
            return AVERROR(errno);
        }
#ifndef _WIN32
        ev_epoll_add(w, w->listen_fd, HTTP_EV_IN, HTTP_EV_TAG_LISTEN);
        ev_epoll_add(w, w->wakeup_r, HTTP_EV_IN, HTTP_EV_TAG_WAKE);
#endif
        atomic_store(&w->running, 1);
        ret = pthread_create(&w->th, NULL, ev_thread, w);
        if (ret)
            return AVERROR(ret);
        nb_workers++;
    }
    atomic_store(&ev_started, 1);
    return 0;
}

void http_ev_stop(void)
{
    int i;
    HttpEvMsg stop = { .type = HTTP_EV_MSG_STOP, .pub_idx = -1, .fmt_idx = -1 };

    if (!atomic_load(&ev_started))
        return;
    for (i = 0; i < nb_workers; i++)
        atomic_store(&workers[i].running, 0);
    for (i = 0; i < nb_workers; i++)
        http_ev_post(i, stop);
    for (i = 0; i < nb_workers; i++) {
        pthread_join(workers[i].th, NULL);
        if (workers[i].listen_fd >= 0)
            ev_close(workers[i].listen_fd);
#ifndef _WIN32
        if (workers[i].wakeup_r >= 0)
            ev_close(workers[i].wakeup_r);
        if (workers[i].epfd >= 0)
            ev_close(workers[i].epfd);
#endif
        pthread_mutex_destroy(&workers[i].mail_mu);
        while (waiters[i]) {
            EvWaiter *x = waiters[i];
            waiters[i] = x->next;
            av_free(x);
        }
    }
    nb_workers = 0;
    atomic_store(&ev_started, 0);
}
