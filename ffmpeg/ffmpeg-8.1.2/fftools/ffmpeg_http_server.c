/*
 * Process-level HTTP/HTTPS GET: HLS/DASH/VOD files + /{app}/{stream}.flv|.ts|.mp4
 * from the memory GOP publisher (see ffmpeg_http_live.c).
 *
 * Per-core EventWorker + per-connection coroutine (ZLM-style).
 * Optional TLS via OpenSSL. Pull auth + IP whitelist.
 */

#include "config.h"

#include "ffmpeg.h"
#include "ffmpeg_http_ev.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define http_mkdir(path) _mkdir(path)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define http_mkdir(path) mkdir((path), 0755)
#endif

#include "libavutil/attributes.h"
#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"

#if CONFIG_OPENSSL
/* tgmath.h (via ffmpeg.h → header.h) defines I as _Complex_I;
 * OpenSSL rsa.h uses parameter name I. */
#ifdef I
#undef I
#endif
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#endif

/* Same clock as get_fmttime() / print_report: ([2026-09-02 15:34:11,367] */
void ffmpeg_http_log(int level, const char *fmt, ...)
{
    char ts[40], msg[1024];
    int64_t us = av_gettime();
    time_t sec = (time_t)(us / 1000000);
    int ms = (int)((us / 1000) % 1000);
    struct tm tmbuf, *tm;
    va_list ap;

#ifdef _WIN32
    tm = (localtime_s(&tmbuf, &sec) == 0) ? &tmbuf : NULL;
#else
    tm = localtime_r(&sec, &tmbuf);
#endif
    if (tm)
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d,%03d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec, ms);
    else
        snprintf(ts, sizeof(ts), "---- -- -- --:--:--,%03d", ms);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    av_log(NULL, level, "([%s] %s", ts, msg);
}

char *ffmpeg_http_server_url;
char *ffmpeg_http_root;
char *ffmpeg_http_cert;
char *ffmpeg_http_key;
char *ffmpeg_http_auth;
char *ffmpeg_http_token;
char *ffmpeg_http_allow;
int   ffmpeg_http_workers; /* 0 = nproc EventWorkers */

#define HTTP_SNDBUF         (256 * 1024)

typedef struct HttpNet {
    int family;
    uint8_t addr[16];
    uint8_t mask[16];
} HttpNet;

static atomic_int http_running;
static int http_thread_started;
static char *http_root_abs;
static int http_use_tls;
static HttpNet *http_nets;
static int http_nb_nets;

#if CONFIG_OPENSSL
static SSL_CTX *http_ssl_ctx;
#endif

static void http_trace(const HttpSession *c, int level, const char *fmt, ...)
{
    char msg[640];
    va_list ap;
    int ms = c ? (int)((av_gettime_relative() - c->t0) / 1000) : 0;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ffmpeg_http_log( level, "http: [%s +%dms] %s\n",
           (c && c->peer[0]) ? c->peer : "-", ms, msg);
}

const char *ffmpeg_http_conn_peer(void *conn)
{
    HttpSession *c = conn;
    return (c && c->peer[0]) ? c->peer : "-";
}

void ffmpeg_http_conn_note(void *conn, const char *reason)
{
    HttpSession *c = conn;
    if (!c || !reason || !reason[0])
        return;
    av_strlcpy(c->close_reason, reason, sizeof(c->close_reason));
}

int64_t ffmpeg_http_conn_bytes(void *conn)
{
    HttpSession *c = conn;
    return c ? c->bytes_out : 0;
}

int ffmpeg_http_conn_worker_id(void *conn)
{
    HttpSession *c = conn;
    return (c && c->w) ? http_ev_worker_id(c->w) : -1;
}

void ffmpeg_http_conn_wait_msg(void *conn)
{
    if (conn)
        http_ev_session_wait_msg(conn);
}

int ffmpeg_http_conn_got_msg(void *conn, int *type, int *pub_idx, int *fmt_idx)
{
    HttpEvMsg m;

    if (!http_ev_session_got_msg(conn, &m))
        return 0;
    if (type)
        *type = m.type;
    if (pub_idx)
        *pub_idx = m.pub_idx;
    if (fmt_idx)
        *fmt_idx = m.fmt_idx;
    return 1;
}

void ffmpeg_http_conn_mark_live(void *conn, int pub_idx)
{
    HttpSession *c = conn;

    if (c)
        c->live_pub = pub_idx;
}

int ffmpeg_http_conn_kicked(void *conn)
{
    HttpSession *c = conn;

    return c && c->kick;
}

static int http_mkdir_p(const char *path)
{
    char *tmp, *p;
    size_t n;

    if (!path || !path[0])
        return AVERROR(EINVAL);
    tmp = av_strdup(path);
    if (!tmp)
        return AVERROR(ENOMEM);
    n = strlen(tmp);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\'))
        tmp[--n] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = 0;
            if (http_mkdir(tmp) < 0 && errno != EEXIST) {
                av_free(tmp);
                return AVERROR(errno);
            }
            *p = save;
        }
    }
    if (http_mkdir(tmp) < 0 && errno != EEXIST) {
        av_free(tmp);
        return AVERROR(errno);
    }
    av_free(tmp);
    return 0;
}

static void http_strip_slash(char *s)
{
    size_t n;

    if (!s)
        return;
    n = strlen(s);
    while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\'))
        s[--n] = 0;
}

static char *infer_http_root(void)
{
    for (int i = 0; i < nb_output_files; i++) {
        const char *url = output_files[i] ? output_files[i]->url : NULL;
        const char *dir;
        char *copy, *out;

        if (!url || !url[0])
            continue;
        if (av_strstart(url, "http:", NULL) || av_strstart(url, "https:", NULL) ||
            av_strstart(url, "rtmp:", NULL) || av_strstart(url, "srt:", NULL) ||
            av_strstart(url, "tcp:", NULL) || av_strstart(url, "udp:", NULL))
            continue;
        if (av_strstart(url, "file:", NULL))
            url += 5;
        if (!av_match_ext(url, "m3u8") && !av_match_ext(url, "mpd") &&
            !av_match_ext(url, "ts") && !av_match_ext(url, "mp4"))
            continue;
        copy = av_strdup(url);
        if (!copy)
            return NULL;
        dir = av_dirname(copy);
        if (!dir || !dir[0] || !strcmp(dir, ".")) {
            av_free(copy);
            return av_strdup(".");
        }
        out = av_strdup(dir);
        av_free(copy);
        return out;
    }
    return NULL;
}

static int parse_listen_url(const char *url, char *host, int host_sz, int *port,
                            int *use_tls)
{
    char proto[16] = {0}, path[256] = {0};
    const char *p = url;

    *port = 8080;
    *use_tls = 0;
    av_strlcpy(host, "0.0.0.0", host_sz);
    if (!url || !url[0])
        return AVERROR(EINVAL);

    if (av_strstart(p, "https://", NULL) || av_strstart(p, "tls://", NULL))
        *use_tls = 1;

    if (!av_strstart(p, "http://", NULL) && !av_strstart(p, "https://", NULL) &&
        !av_strstart(p, "tcp://", NULL) && !av_strstart(p, "tls://", NULL)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "http://%s", p);
        av_url_split(proto, sizeof(proto), NULL, 0, host, host_sz, port,
                     path, sizeof(path), buf);
    } else {
        av_url_split(proto, sizeof(proto), NULL, 0, host, host_sz, port,
                     path, sizeof(path), p);
    }
    if (!host[0])
        av_strlcpy(host, "0.0.0.0", host_sz);
    if (*port <= 0)
        *port = *use_tls ? 443 : 8080;
    return 0;
}

static const char *mime_for_path(const char *path)
{
    if (av_match_ext(path, "m3u8"))
        return "application/vnd.apple.mpegurl";
    if (av_match_ext(path, "mpd"))
        return "application/dash+xml";
    if (av_match_ext(path, "m4s") || av_match_ext(path, "mp4") ||
        av_match_ext(path, "cmfv") || av_match_ext(path, "cmfa") ||
        av_match_ext(path, "m4v") || av_match_ext(path, "m4a"))
        return "video/mp4";
    if (av_match_ext(path, "ts") || av_match_ext(path, "m2t") ||
        av_match_ext(path, "m2ts"))
        return "video/mp2t";
    if (av_match_ext(path, "aac"))
        return "audio/aac";
    if (av_match_ext(path, "vtt") || av_match_ext(path, "webvtt"))
        return "text/vtt";
    if (av_match_ext(path, "m3u"))
        return "audio/x-mpegurl";
    return "application/octet-stream";
}

static int is_playlist(const char *path)
{
    return av_match_ext(path, "m3u8") || av_match_ext(path, "mpd") ||
           av_match_ext(path, "m3u");
}

static int url_decode(char *dst, int dst_sz, const char *src)
{
    int i, o = 0;

    for (i = 0; src[i] && o + 1 < dst_sz; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
    return o;
}

static int map_url_path(const char *req, char *out, int out_sz)
{
    char decoded[1024], safe[1024];
    char *in, *save = NULL, *tok;

    url_decode(decoded, sizeof(decoded), req);
    if (decoded[0] == '/' || decoded[0] == '\\')
        memmove(decoded, decoded + 1, strlen(decoded));
    if (strchr(decoded, '\\')) {
        for (char *q = decoded; *q; q++)
            if (*q == '\\')
                *q = '/';
    }
    if (!decoded[0] || !strcmp(decoded, "/")) {
        snprintf(out, out_sz, "%s/index.m3u8", http_root_abs);
        return 0;
    }
    safe[0] = 0;
    in = decoded;
    tok = av_strtok(in, "/", &save);
    while (tok) {
        if (!strcmp(tok, "..") || tok[0] == 0 || !strcmp(tok, ".")) {
            if (!strcmp(tok, ".."))
                return AVERROR(EPERM);
        } else {
            av_strlcat(safe, "/", sizeof(safe));
            av_strlcat(safe, tok, sizeof(safe));
        }
        tok = av_strtok(NULL, "/", &save);
    }
    if (!safe[0])
        snprintf(out, out_sz, "%s/index.m3u8", http_root_abs);
    else
        snprintf(out, out_sz, "%s%s", http_root_abs, safe);
    return 0;
}

static int token_eq(const char *got, const char *expect)
{
    size_t n, m, i;
    unsigned diff;

    if (!expect || !expect[0])
        return 1;
    n = strlen(expect);
    m = got ? strlen(got) : 0;
    diff = (unsigned)(n != m);
    for (i = 0; i < n; i++)
        diff |= (unsigned char)((i < m) ? got[i] : 0) ^ (unsigned char)expect[i];
    return diff == 0;
}

static void prefix_mask(int family, int bits, uint8_t *mask)
{
    int i, nbytes = family == AF_INET ? 4 : 16;

    memset(mask, 0, 16);
    if (bits < 0)
        bits = 0;
    if (bits > nbytes * 8)
        bits = nbytes * 8;
    for (i = 0; i < nbytes; i++) {
        int left = bits - i * 8;
        if (left >= 8)
            mask[i] = 0xff;
        else if (left > 0)
            mask[i] = (uint8_t)(0xff << (8 - left));
    }
}

static int parse_one_cidr(const char *spec, HttpNet *n)
{
    char buf[128], *slash;
    int bits, ok;

    av_strlcpy(buf, spec, sizeof(buf));
    slash = strchr(buf, '/');
    if (slash)
        *slash = 0;
    memset(n, 0, sizeof(*n));
#ifdef _WIN32
    ok = InetPtonA(AF_INET, buf, n->addr) == 1;
    if (ok)
        n->family = AF_INET;
    else if (InetPtonA(AF_INET6, buf, n->addr) == 1) {
        ok = 1;
        n->family = AF_INET6;
    }
#else
    ok = inet_pton(AF_INET, buf, n->addr) == 1;
    if (ok)
        n->family = AF_INET;
    else if (inet_pton(AF_INET6, buf, n->addr) == 1) {
        ok = 1;
        n->family = AF_INET6;
    }
#endif
    if (!ok)
        return AVERROR(EINVAL);
    bits = slash ? atoi(slash + 1) : (n->family == AF_INET ? 32 : 128);
    prefix_mask(n->family, bits, n->mask);
    return 0;
}

static int parse_allow_list(const char *s)
{
    char *copy, *save = NULL, *tok;
    HttpNet *p;

    av_freep(&http_nets);
    http_nb_nets = 0;
    if (!s || !s[0])
        return 0;
    copy = av_strdup(s);
    if (!copy)
        return AVERROR(ENOMEM);
    tok = av_strtok(copy, ", \t\r\n", &save);
    while (tok) {
        p = av_realloc_array(http_nets, http_nb_nets + 1, sizeof(*http_nets));
        if (!p) {
            av_free(copy);
            return AVERROR(ENOMEM);
        }
        http_nets = p;
        if (parse_one_cidr(tok, &http_nets[http_nb_nets]) < 0) {
            ffmpeg_http_log( AV_LOG_ERROR, "http-server: bad -http_allow '%s'\n", tok);
            av_free(copy);
            return AVERROR(EINVAL);
        }
        http_nb_nets++;
        tok = av_strtok(NULL, ", \t\r\n", &save);
    }
    av_free(copy);
    return 0;
}

static int allow_match(const HttpSession *c)
{
    int i, nbytes;

    if (http_nb_nets <= 0)
        return 1;
    for (i = 0; i < http_nb_nets; i++) {
        HttpNet *n = &http_nets[i];
        int ok = 1;
        if (n->family != c->family)
            continue;
        nbytes = c->family == AF_INET ? 4 : 16;
        for (int b = 0; b < nbytes; b++) {
            if ((c->addr[b] & n->mask[b]) != (n->addr[b] & n->mask[b])) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return 1;
    }
    return 0;
}

static void http_fill_peer(HttpSession *c)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    char host[64] = {0};
    int port = 0;
#ifdef _WIN32
    const char *ok = NULL;
#endif

    c->peer[0] = 0;
    c->peer_ip[0] = 0;
    c->family = AF_UNSPEC;
    memset(c->addr, 0, sizeof(c->addr));
    if (c->fd < 0)
        return;
    if (getpeername(c->fd, (struct sockaddr *)&ss, &slen) != 0)
        return;
    c->family = ss.ss_family;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        memcpy(c->addr, &in->sin_addr, 4);
        port = ntohs(in->sin_port);
#ifdef _WIN32
        ok = InetNtopA(AF_INET, &in->sin_addr, host, sizeof(host));
        if (!ok)
            return;
#else
        if (!inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)))
            return;
#endif
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        memcpy(c->addr, &in6->sin6_addr, 16);
        port = ntohs(in6->sin6_port);
#ifdef _WIN32
        ok = InetNtopA(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        if (!ok)
            return;
#else
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)))
            return;
#endif
    } else {
        return;
    }
    av_strlcpy(c->peer_ip, host, sizeof(c->peer_ip));
    snprintf(c->peer, sizeof(c->peer), "%s:%d", host, port);
}

#ifdef _WIN32
#define http_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#define http_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

#if CONFIG_OPENSSL
static SSL *sess_ssl(HttpSession *c)
{
    return c ? (SSL *)c->ssl : NULL;
}
#endif

static void http_session_cleanup(HttpSession *c)
{
    if (!c)
        return;
    http_trace(c, AV_LOG_INFO, "disconnect reason=%s bytes=%"PRId64,
               c->close_reason[0] ? c->close_reason : "done",
               c->bytes_out);
#if CONFIG_OPENSSL
    if (c->ssl) {
        SSL_shutdown((SSL *)c->ssl);
        SSL_free((SSL *)c->ssl);
        c->ssl = NULL;
    }
#endif
}

int ffmpeg_http_conn_write(void *conn, const uint8_t *buf, int len)
{
    HttpSession *c = conn;
    int off = 0, ret;

    if (!c || !buf || len <= 0)
        return AVERROR(EINVAL);
    while (off < len) {
        if (c->kick)
            return AVERROR(EPIPE);
#if CONFIG_OPENSSL
        if (c->ssl) {
            ret = SSL_write(sess_ssl(c), buf + off, len - off);
            if (ret <= 0) {
                int err = SSL_get_error(sess_ssl(c), ret);
                if (err == SSL_ERROR_WANT_WRITE) {
                    http_ev_session_wait_fd(c, HTTP_EV_OUT);
                    continue;
                }
                if (err == SSL_ERROR_WANT_READ) {
                    http_ev_session_wait_fd(c, HTTP_EV_IN);
                    continue;
                }
                return AVERROR(EIO);
            }
            off += ret;
            continue;
        }
#endif
        if (c->fd < 0)
            return AVERROR(EIO);
#ifdef _WIN32
        ret = send(c->fd, (const char *)buf + off, len - off, 0);
#else
        ret = (int)send(c->fd, buf + off, (size_t)(len - off), MSG_NOSIGNAL);
#endif
        if (ret > 0) {
            off += ret;
            continue;
        }
        if (ret < 0 && http_wouldblock()) {
            http_ev_session_wait_fd(c, HTTP_EV_OUT);
            continue;
        }
        return AVERROR(EIO);
    }
    c->bytes_out += len;
    return 0;
}

static int http_conn_read(HttpSession *c, unsigned char *buf, int len)
{
    int ret;

    if (!c || !buf || len <= 0)
        return AVERROR(EINVAL);
#if CONFIG_OPENSSL
    if (c->ssl) {
        ret = SSL_read(sess_ssl(c), buf, len);
        if (ret > 0)
            return ret;
        if (ret == 0)
            return AVERROR_EOF;
        ret = SSL_get_error(sess_ssl(c), ret);
        if (ret == SSL_ERROR_WANT_READ) {
            http_ev_session_wait_fd(c, HTTP_EV_IN);
            return AVERROR(EAGAIN);
        }
        if (ret == SSL_ERROR_WANT_WRITE) {
            http_ev_session_wait_fd(c, HTTP_EV_OUT);
            return AVERROR(EAGAIN);
        }
        return AVERROR(EIO);
    }
#endif
    if (c->fd < 0)
        return AVERROR(EIO);
#ifdef _WIN32
    ret = recv(c->fd, (char *)buf, len, 0);
#else
    ret = (int)recv(c->fd, buf, (size_t)len, 0);
#endif
    if (ret > 0)
        return ret;
    if (ret == 0)
        return AVERROR_EOF;
    if (http_wouldblock()) {
        http_ev_session_wait_fd(c, HTTP_EV_IN);
        return AVERROR(EAGAIN);
    }
    return AVERROR(EIO);
}

static int write_all(HttpSession *c, const char *buf, int len)
{
    return ffmpeg_http_conn_write(c, (const uint8_t *)buf, len);
}

static int send_status(HttpSession *c, int code, const char *text)
{
    char hdr[768];
    const char *reason = text;
    const char *www = "";
    int n;

    if (code == 401)
        reason = "Unauthorized";
    else if (code == 403)
        reason = "Forbidden";
    if (code == 401)
        www = (ffmpeg_http_auth && ffmpeg_http_auth[0])
              ? "WWW-Authenticate: Basic realm=\"ffmpeg\"\r\n"
              : "WWW-Authenticate: Bearer realm=\"ffmpeg\"\r\n";
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 %d %s\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Headers: *\r\n"
                 "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n"
                 "%s",
                 code, reason, www, (int)strlen(text), text);
    return write_all(c, hdr, n);
}

static int query_get(const char *path, const char *key, char *out, int out_sz);

static int send_json(HttpSession *c, int code, const char *json, int head_only)
{
    char hdr[512];
    const char *body = json ? json : "{}";
    int n, blen = (int)strlen(body);

    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 %d %s\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Headers: *\r\n"
                 "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n"
                 "Cache-Control: no-cache, no-store\r\n"
                 "Connection: close\r\n"
                 "Content-Type: application/json; charset=utf-8\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n",
                 code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"),
                 blen);
    if (write_all(c, hdr, n) < 0)
        return AVERROR(EIO);
    if (head_only)
        return 0;
    return write_all(c, body, blen);
}

static int http_api_handle(HttpSession *c, const char *method, const char *path,
                           int head_only)
{
    char path_noq[1024], spec[512], app[128], stream[256], allq[16];
    char *json = NULL, *q;
    int all = 0, ret, code = 200;

    (void)method;

    av_strlcpy(path_noq, path, sizeof(path_noq));
    q = strchr(path_noq, '?');
    if (q)
        *q = 0;
    if (!strcmp(path_noq, "/api/streams") || !strcmp(path_noq, "/api/stat") ||
        !strcmp(path_noq, "/index/api/getMediaList")) {
        ret = ffmpeg_http_live_stat_json(&json);
        if (ret < 0 || !json)
            return send_status(c, 500, "stat failed");
        ret = send_json(c, 200, json, head_only);
        av_freep(&json);
        return ret;
    }
    if (!strcmp(path_noq, "/api/kick") ||
        !strcmp(path_noq, "/index/api/close_streams")) {
        spec[0] = 0;
        if (query_get(path, "all", allq, sizeof(allq)))
            all = allq[0] == '1' || !av_strcasecmp(allq, "true") ||
                  !av_strcasecmp(allq, "yes");
        if (query_get(path, "streams", spec, sizeof(spec)))
            ;
        else if (query_get(path, "spec", spec, sizeof(spec)))
            ;
        else if (query_get(path, "stream", stream, sizeof(stream))) {
            if (query_get(path, "app", app, sizeof(app)) && app[0])
                snprintf(spec, sizeof(spec), "%s/%s", app, stream);
            else
                av_strlcpy(spec, stream, sizeof(spec));
        }
        {
            char dec[512];
            url_decode(dec, sizeof(dec), spec);
            av_strlcpy(spec, dec, sizeof(spec));
        }
        if (!all && !spec[0]) {
            ffmpeg_http_conn_note(c, "kick-bad");
            return send_json(c, 400,
                             "{\"code\":-3,\"kicked\":0,\"error\":\"missing stream\"}",
                             head_only);
        }
        ret = ffmpeg_http_live_kick(spec, all, &json);
        if (ret < 0 || !json)
            return send_status(c, 500, "kick failed");
        if (strstr(json, "\"code\":-2"))
            code = 404;
        ffmpeg_http_conn_note(c, "kick");
        http_trace(c, AV_LOG_INFO, "api kick spec=%s all=%d", spec[0] ? spec : "-", all);
        ret = send_json(c, code, json, head_only);
        av_freep(&json);
        return ret;
    }
    return send_status(c, 404, "Not Found");
}

static int send_options(HttpSession *c)
{
    const char *hdr =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Connection: close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    return write_all(c, hdr, (int)strlen(hdr));
}

static int parse_range(const char *req, int64_t size, int64_t *start, int64_t *end)
{
    const char *p = av_stristr(req, "\nRange:");
    int64_t a = 0, b = size - 1;

    *start = 0;
    *end = size - 1;
    if (!p || size <= 0)
        return 0;
    p += 7;
    while (*p == ' ' || *p == '\t')
        p++;
    if (av_strncasecmp(p, "bytes=", 6))
        return 0;
    p += 6;
    if (*p == '-')
        return 0;
    a = strtoll(p, (char **)&p, 10);
    if (*p == '-') {
        p++;
        if (isdigit((unsigned char)*p))
            b = strtoll(p, NULL, 10);
    }
    if (a < 0 || a >= size)
        return AVERROR(EINVAL);
    if (b >= size)
        b = size - 1;
    if (b < a)
        return AVERROR(EINVAL);
    *start = a;
    *end = b;
    return 1;
}

static int send_file(HttpSession *c, const char *path, int head_only, const char *req)
{
    FILE *fp;
    char hdr[768], buf[16 * 1024];
    int64_t size, start = 0, end, send_len;
    int ranged, n;
    const char *mime = mime_for_path(path);
    const char *cache = is_playlist(path) ? "no-cache, no-store" : "max-age=2";

    fp = fopen(path, "rb");
    if (!fp)
        return send_status(c, 404, "Not Found");
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return send_status(c, 500, "seek failed");
    }
    size = ftell(fp);
    if (size < 0)
        size = 0;
    ranged = parse_range(req, size, &start, &end);
    if (ranged < 0) {
        fclose(fp);
        return send_status(c, 416, "Range Not Satisfiable");
    }
    if (!ranged) {
        start = 0;
        end = size > 0 ? size - 1 : 0;
    }
    send_len = size > 0 ? (end - start + 1) : 0;
    if (ranged)
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Range: bytes %"PRId64"-%"PRId64"/%"PRId64"\r\n"
                     "Content-Length: %"PRId64"\r\n"
                     "\r\n",
                     cache, mime, start, end, size, send_len);
    else
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %"PRId64"\r\n"
                     "\r\n",
                     cache, mime, size);
    if (write_all(c, hdr, n) < 0) {
        fclose(fp);
        return AVERROR(EIO);
    }
    if (head_only || size <= 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, (long)start, SEEK_SET) != 0) {
        fclose(fp);
        return AVERROR(EIO);
    }
#if CONFIG_OPENSSL
    if (c->ssl) {
        while (send_len > 0) {
            size_t chunk = FFMIN((int64_t)sizeof(buf), send_len);
            size_t got = fread(buf, 1, chunk, fp);
            if (!got)
                break;
            if (write_all(c, buf, (int)got) < 0)
                break;
            send_len -= (int64_t)got;
        }
        fclose(fp);
        return 0;
    }
#endif
#ifndef _WIN32
    {
        int in_fd = fileno(fp);
        off_t off = (off_t)start;
        while (send_len > 0 && in_fd >= 0) {
            ssize_t got = sendfile(c->fd, in_fd, &off, (size_t)send_len);
            if (got > 0) {
                send_len -= got;
                c->bytes_out += got;
                continue;
            }
            if (got < 0 && http_wouldblock()) {
                http_ev_session_wait_fd(c, HTTP_EV_OUT);
                continue;
            }
            break;
        }
    }
#else
    while (send_len > 0) {
        size_t chunk = FFMIN((int64_t)sizeof(buf), send_len);
        size_t got = fread(buf, 1, chunk, fp);
        if (!got)
            break;
        if (write_all(c, buf, (int)got) < 0)
            break;
        send_len -= (int64_t)got;
    }
#endif
    fclose(fp);
    return 0;
}

static int read_request(HttpSession *c, char *req, int req_sz)
{
    int n = 0, ret;

    while (n + 1 < req_sz) {
        int want = FFMIN(4096, req_sz - 1 - n);
        ret = http_conn_read(c, (unsigned char *)req + n, want);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret <= 0)
            break;
        n += ret;
        req[n] = 0;
        if (n >= 4 && strstr(req, "\r\n\r\n"))
            return n;
        if (n >= 2 && strstr(req, "\n\n"))
            return n;
    }
    return n > 0 ? n : AVERROR(EIO);
}

static const char *hdr_line(const char *req, const char *name)
{
    const char *p = req;
    size_t nlen = strlen(name);

    while (p && *p) {
        if (!av_strncasecmp(p, name, nlen) && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return NULL;
}

static void copy_hdr_val(char *dst, int dst_sz, const char *p)
{
    int i = 0;
    if (!p) {
        dst[0] = 0;
        return;
    }
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < dst_sz) {
        dst[i] = p[i];
        i++;
    }
    dst[i] = 0;
}

static void redact_query(char *s)
{
    char *p = av_stristr(s, "token=");
    if (!p)
        return;
    p += 6;
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n')
        *p++ = '*';
}

static int query_get(const char *path, const char *key, char *out, int out_sz)
{
    char pat[64];
    const char *q, *p, *e;
    int n;

    out[0] = 0;
    q = strchr(path, '?');
    if (!q)
        return 0;
    snprintf(pat, sizeof(pat), "%s=", key);
    p = av_stristr(q + 1, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    e = p;
    while (*e && *e != '&' && *e != ' ')
        e++;
    n = FFMIN((int)(e - p), out_sz - 1);
    memcpy(out, p, n);
    out[n] = 0;
    return n > 0;
}

static int auth_configured(void)
{
    return (ffmpeg_http_auth && ffmpeg_http_auth[0]) ||
           (ffmpeg_http_token && ffmpeg_http_token[0]);
}

static int check_auth(HttpSession *c, const char *req, const char *path)
{
    char qtok[256], userpass[256], decoded[256];
    const char *auth;
    int ok = 0;

    if (!auth_configured())
        return 1;

    if (ffmpeg_http_token && ffmpeg_http_token[0] &&
        query_get(path, "token", qtok, sizeof(qtok))) {
        char decoded_tok[256];
        url_decode(decoded_tok, sizeof(decoded_tok), qtok);
        if (token_eq(decoded_tok, ffmpeg_http_token)) {
            http_trace(c, AV_LOG_INFO, "auth ok token=query");
            return 1;
        }
    }

    auth = hdr_line(req, "Authorization");
    if (auth && !av_strncasecmp(auth, "Bearer ", 7)) {
        copy_hdr_val(qtok, sizeof(qtok), auth + 7);
        if (ffmpeg_http_token && ffmpeg_http_token[0] &&
            token_eq(qtok, ffmpeg_http_token)) {
            http_trace(c, AV_LOG_INFO, "auth ok token=bearer");
            return 1;
        }
    }
    if (auth && !av_strncasecmp(auth, "Basic ", 6) &&
        ffmpeg_http_auth && ffmpeg_http_auth[0]) {
        copy_hdr_val(userpass, sizeof(userpass), auth + 6);
        {
            int n64 = av_base64_decode((uint8_t *)decoded, userpass,
                                       sizeof(decoded) - 1);
            if (n64 >= 0) {
                decoded[n64] = 0;
                if (token_eq(decoded, ffmpeg_http_auth))
                    ok = 1;
            }
        }
        if (ok) {
            http_trace(c, AV_LOG_INFO, "auth ok basic");
            return 1;
        }
    }

    http_trace(c, AV_LOG_WARNING, "auth failed");
    return 0;
}

#if CONFIG_OPENSSL
static int http_tls_handshake(HttpSession *c)
{
    int ret, err;
    int64_t deadline;

    if (!http_ssl_ctx)
        return AVERROR(EINVAL);
    c->ssl = SSL_new(http_ssl_ctx);
    if (!c->ssl)
        return AVERROR(ENOMEM);
    SSL_set_fd(sess_ssl(c), c->fd);
    SSL_set_accept_state(sess_ssl(c));
    deadline = av_gettime_relative() + 5000000;
    while (atomic_load(&http_running) && av_gettime_relative() < deadline) {
        ret = SSL_accept(sess_ssl(c));
        if (ret == 1) {
            http_trace(c, AV_LOG_INFO, "tls handshake ok");
            return 0;
        }
        err = SSL_get_error(sess_ssl(c), ret);
        if (err == SSL_ERROR_WANT_READ) {
            http_ev_session_wait_fd(c, HTTP_EV_IN);
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            http_ev_session_wait_fd(c, HTTP_EV_OUT);
            continue;
        }
        http_trace(c, AV_LOG_ERROR, "tls handshake failed err=%d", err);
        return AVERROR(EIO);
    }
    http_trace(c, AV_LOG_ERROR, "tls handshake timeout");
    return AVERROR(ETIMEDOUT);
}
#endif

/* Drain the client request then send a complete HTTP error. A silent TCP
 * close (no status) leaves ffplay blocked waiting for response headers. */
static void http_reply_error(HttpSession *c, int code, const char *text)
{
    char req[8192];

    if (http_use_tls) {
#if CONFIG_OPENSSL
        if (!c->ssl && http_tls_handshake(c) < 0)
            return;
#else
        (void)req;
        return;
#endif
    }
    read_request(c, req, sizeof(req));
    send_status(c, code, text);
    http_trace(c, AV_LOG_WARNING, "http %d %s", code, text);
}

static void http_deny_whitelist(HttpSession *c)
{
    ffmpeg_http_conn_note(c, "deny-whitelist");
    http_trace(c, AV_LOG_WARNING, "deny whitelist");
    http_trace(c, AV_LOG_WARNING, "http 403 Forbidden");
    send_status(c, 403, "Forbidden");
}

static void http_session_co(void *arg)
{
    HttpSession *c = arg;
    char req[8192], method[16], path[1024], mapped[2048];
    char host[256], ua[256], path_log[1024];
    int n, head_only = 0, api = 0;
    char *line, *sp1, *sp2;

    if (!c)
        return;
    http_trace(c, AV_LOG_INFO, "connect %s", http_use_tls ? "https" : "http");

    if (http_use_tls) {
#if CONFIG_OPENSSL
        if (http_tls_handshake(c) < 0) {
            ffmpeg_http_conn_note(c, "tls-fail");
            goto done;
        }
#else
        ffmpeg_http_conn_note(c, "no-openssl");
        http_trace(c, AV_LOG_ERROR, "https requested but OpenSSL disabled");
        goto done;
#endif
    }
    n = read_request(c, req, sizeof(req));
    if (n < 0) {
        if (!allow_match(c)) {
            http_deny_whitelist(c);
            goto done;
        }
        ffmpeg_http_conn_note(c, "read-fail");
        http_trace(c, AV_LOG_WARNING, "read request failed: %s", av_err2str(n));
        goto done;
    }

    line = req;
    sp1 = strchr(line, ' ');
    if (!sp1)
        goto bad;
    *sp1 = 0;
    av_strlcpy(method, line, sizeof(method));
    sp2 = strchr(sp1 + 1, ' ');
    if (!sp2)
        goto bad;
    *sp2 = 0;
    av_strlcpy(path, sp1 + 1, sizeof(path));
    *sp2 = ' ';

    av_strlcpy(path_log, path, sizeof(path_log));
    redact_query(path_log);
    copy_hdr_val(host, sizeof(host), hdr_line(req, "Host"));
    copy_hdr_val(ua, sizeof(ua), hdr_line(req, "User-Agent"));
    http_trace(c, AV_LOG_INFO, "%s %s host=%s ua=%s",
               method, path_log, host[0] ? host : "-", ua[0] ? ua : "-");

    if (!allow_match(c)) {
        http_deny_whitelist(c);
        goto done;
    }
    if (http_nb_nets > 0)
        http_trace(c, AV_LOG_INFO, "allow whitelist");

    if (!av_strcasecmp(method, "OPTIONS")) {
        send_options(c);
        ffmpeg_http_conn_note(c, "options");
        http_trace(c, AV_LOG_INFO, "reply 204 OPTIONS");
        goto done;
    }
    {
        char pchk[1024], *qq;
        av_strlcpy(pchk, path, sizeof(pchk));
        qq = strchr(pchk, '?');
        if (qq)
            *qq = 0;
        if (!strncmp(pchk, "/api/", 5) || !strncmp(pchk, "/index/api/", 11))
            api = 1;
    }
    if (!av_strcasecmp(method, "HEAD"))
        head_only = 1;
    if (av_strcasecmp(method, "GET") && av_strcasecmp(method, "HEAD") &&
        !(api && !av_strcasecmp(method, "POST"))) {
        ffmpeg_http_conn_note(c, "method");
        http_trace(c, AV_LOG_WARNING, "method not allowed");
        send_status(c, 405, "Method Not Allowed");
        goto done;
    }

    if (!check_auth(c, req, path)) {
        ffmpeg_http_conn_note(c, "auth-fail");
        http_trace(c, AV_LOG_WARNING, "http 401 Unauthorized");
        send_status(c, 401, "Unauthorized");
        goto done;
    }

    if (api) {
        http_trace(c, AV_LOG_INFO, "route api %s", path_log);
        http_api_handle(c, method, path, head_only);
        goto done;
    }

    {
        char path_noq[1024], *q;
        av_strlcpy(path_noq, path, sizeof(path_noq));
        q = strchr(path_noq, '?');
        if (q)
            *q = 0;
        if (av_match_ext(path_noq, "m3u8") || av_match_ext(path_noq, "mpd") ||
            av_match_ext(path_noq, "m3u") || av_match_ext(path_noq, "m4s") ||
            av_match_ext(path_noq, "cmfv") || av_match_ext(path_noq, "cmfa"))
            goto file_only;
    }
    if (ffmpeg_http_live_enabled()) {
        const char *lfmt = NULL, *lmime = NULL;
        int lowdelay = 0, pub_idx = -1;
        if (ffmpeg_http_live_match(path, &lfmt, &lmime, &lowdelay, &pub_idx)) {
            http_trace(c, AV_LOG_INFO, "route live %s%s",
                       path_log, lowdelay ? " lowdelay" : "");
            ffmpeg_http_live_serve(c, lfmt, lmime, head_only, pub_idx,
                                   lowdelay, c->t0);
            goto done;
        }
    }
file_only:
    if (!http_root_abs) {
        ffmpeg_http_conn_note(c, "not-found");
        http_trace(c, AV_LOG_WARNING, "404 no http_root");
        send_status(c, 404, "Not Found");
        goto done;
    }
    if (map_url_path(path, mapped, sizeof(mapped)) < 0) {
        ffmpeg_http_conn_note(c, "forbidden");
        http_trace(c, AV_LOG_WARNING, "403 path");
        send_status(c, 403, "Forbidden");
        goto done;
    }
    if (!strcmp(path, "/") || !path[0]) {
        FILE *fp = fopen(mapped, "rb");
        if (!fp)
            snprintf(mapped, sizeof(mapped), "%s/index.mpd", http_root_abs);
        else
            fclose(fp);
    }
    http_trace(c, AV_LOG_INFO, "route file %s", mapped);
    send_file(c, mapped, head_only, req);
    ffmpeg_http_conn_note(c, "file-done");
    http_trace(c, AV_LOG_INFO, "file done");
    goto done;
bad:
    if (!allow_match(c)) {
        http_deny_whitelist(c);
        goto done;
    }
    ffmpeg_http_conn_note(c, "bad-request");
    http_trace(c, AV_LOG_WARNING, "400 bad request");
    send_status(c, 400, "Bad Request");
done:
    http_session_cleanup(c);
}

static int http_tls_ctx_init(void)
{
#if CONFIG_OPENSSL
    if (!http_use_tls)
        return 0;
    if (!ffmpeg_http_cert || !ffmpeg_http_cert[0] ||
        !ffmpeg_http_key || !ffmpeg_http_key[0]) {
        ffmpeg_http_log( AV_LOG_ERROR,
               "http-server: https needs -http_cert and -http_key\n");
        return AVERROR(EINVAL);
    }
    /* This tree links OpenSSL 1.0.2 (SSL_library_init). 1.1-only APIs
     * such as OPENSSL_init_ssl / TLS_server_method would be implicit
     * declarations under -Werror=implicit-function-declaration. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    http_ssl_ctx = SSL_CTX_new(SSLv23_server_method());
#else
    OPENSSL_init_ssl(0, NULL);
    http_ssl_ctx = SSL_CTX_new(TLS_server_method());
#endif
    if (!http_ssl_ctx)
        return AVERROR(ENOMEM);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_CTX_set_options(http_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                        SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
#else
    SSL_CTX_set_min_proto_version(http_ssl_ctx, TLS1_2_VERSION);
#endif
    if (SSL_CTX_use_certificate_chain_file(http_ssl_ctx, ffmpeg_http_cert) != 1) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: load cert %s failed\n",
               ffmpeg_http_cert);
        SSL_CTX_free(http_ssl_ctx);
        http_ssl_ctx = NULL;
        return AVERROR(EINVAL);
    }
    if (SSL_CTX_use_PrivateKey_file(http_ssl_ctx, ffmpeg_http_key,
                                    SSL_FILETYPE_PEM) != 1) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: load key %s failed\n",
               ffmpeg_http_key);
        SSL_CTX_free(http_ssl_ctx);
        http_ssl_ctx = NULL;
        return AVERROR(EINVAL);
    }
    if (SSL_CTX_check_private_key(http_ssl_ctx) != 1) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: cert/key mismatch\n");
        SSL_CTX_free(http_ssl_ctx);
        http_ssl_ctx = NULL;
        return AVERROR(EINVAL);
    }
    return 0;
#else
    if (http_use_tls) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: https needs OpenSSL\n");
        return AVERROR(ENOSYS);
    }
    return 0;
#endif
}

int ffmpeg_http_server_init(const char *url, const char *root)
{
    char host[256];
    int port = 8080, ret, nworkers;
    const char *use_root = root;

    if (!url || !url[0])
        return 0;
    if (http_thread_started)
        return 0;

    if (parse_allow_list(ffmpeg_http_allow) < 0)
        return AVERROR(EINVAL);

    if (!use_root || !use_root[0]) {
        av_freep(&ffmpeg_http_root);
        ffmpeg_http_root = infer_http_root();
        use_root = ffmpeg_http_root;
    }
    av_freep(&http_root_abs);
    if (use_root && use_root[0]) {
        ret = http_mkdir_p(use_root);
        if (ret < 0) {
            ffmpeg_http_log( AV_LOG_ERROR, "http-server: mkdir %s failed: %s\n",
                   use_root, av_err2str(ret));
            return ret;
        }
        http_root_abs = av_strdup(use_root);
        if (!http_root_abs)
            return AVERROR(ENOMEM);
        http_strip_slash(http_root_abs);
    } else {
        ffmpeg_http_log( AV_LOG_INFO,
               "http-server: no -http_root; file GET off, live GOP only\n");
    }

    if (parse_listen_url(url, host, sizeof(host), &port, &http_use_tls) < 0)
        return AVERROR(EINVAL);
    ret = http_tls_ctx_init();
    if (ret < 0)
        return ret;

    nworkers = ffmpeg_http_workers;
    if (nworkers <= 0)
        nworkers = http_ev_cpu_count();
    if (nworkers > HTTP_EV_MAX)
        nworkers = HTTP_EV_MAX;

    atomic_store(&http_running, 1);
    ret = http_ev_start(host, port, nworkers, http_session_co);
    if (ret < 0) {
        atomic_store(&http_running, 0);
        return ret;
    }
    http_thread_started = 1;
    nworkers = http_ev_nb_workers();
    ffmpeg_http_log( AV_LOG_INFO,
           "http-server: %s %s workers=%d reuseport=1 stack=64k auth=%s allow=%s files=%s live=%s\n",
           http_use_tls ? "https" : "http", url, nworkers,
           auth_configured() ? "on" : "off",
           http_nb_nets > 0 ? ffmpeg_http_allow : "any",
           http_root_abs ? http_root_abs : "(off)",
           ffmpeg_http_live ? "gop" : "off");
    return 0;
}

void ffmpeg_http_server_uninit(void)
{
    if (!http_thread_started)
        return;
    atomic_store(&http_running, 0);
    ffmpeg_http_live_shutdown();
    http_ev_stop();
    http_thread_started = 0;
#if CONFIG_OPENSSL
    if (http_ssl_ctx) {
        SSL_CTX_free(http_ssl_ctx);
        http_ssl_ctx = NULL;
    }
#endif
    av_freep(&http_nets);
    http_nb_nets = 0;
    av_freep(&http_root_abs);
}
