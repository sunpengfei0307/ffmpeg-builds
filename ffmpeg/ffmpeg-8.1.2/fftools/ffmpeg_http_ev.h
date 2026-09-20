/*
 * Per-core EventWorker: epoll + SO_REUSEPORT listen + pointer mailbox.
 * Other threads must not touch a session fd ? only http_ev_post().
 */

#ifndef FFTOOLS_FFMPEG_HTTP_EV_H
#define FFTOOLS_FFMPEG_HTTP_EV_H

#include <stdint.h>

#ifndef _WIN32
#include <sys/epoll.h>
#define HTTP_EV_IN      EPOLLIN
#define HTTP_EV_OUT     EPOLLOUT
#define HTTP_EV_ERR     (EPOLLERR | EPOLLHUP | EPOLLRDHUP)
#else
#define HTTP_EV_IN      0x001
#define HTTP_EV_OUT     0x004
#define HTTP_EV_ERR     0x018
#endif

#define HTTP_EV_MAX     64
#define HTTP_EV_INBOX   16

enum HttpEvMsgType {
    HTTP_EV_MSG_FRAME = 1,
    HTTP_EV_MSG_GOP_READY,
    HTTP_EV_MSG_STOP,
    HTTP_EV_MSG_KICK,
};

typedef struct HttpEvMsg {
    int type;
    int pub_idx;
    int fmt_idx;
} HttpEvMsg;

typedef struct HttpEventWorker HttpEventWorker;
typedef struct HttpCo HttpCo;

typedef struct HttpSession {
    HttpEventWorker *w;
    HttpCo *co;
    int fd;
    char peer[80];
    char peer_ip[64];
    int family;
    uint8_t addr[16];
    int64_t t0;
    int64_t bytes_out;
    char close_reason[80];
    int wait_events;
    int wait_msg;
    HttpEvMsg inbox[HTTP_EV_INBOX];
    int inbox_n;
    void *ssl;
    int closing;
    int live_pub;
    int kick;
    struct HttpSession *next;
} HttpSession;

int  http_ev_cpu_count(void);
int  http_ev_nb_workers(void);
int  http_ev_worker_id(const HttpEventWorker *w);

int  http_ev_start(const char *host, int port, int nworkers,
                   void (*session_fn)(void *));
void http_ev_stop(void);

int  http_ev_post(int worker_id, HttpEvMsg msg);

void http_ev_session_wait_fd(HttpSession *s, int events);
void http_ev_session_wait_msg(HttpSession *s);
int  http_ev_session_got_msg(HttpSession *s, HttpEvMsg *out);

#endif /* FFTOOLS_FFMPEG_HTTP_EV_H */
