/*
 * Process-level HTTP/HTTPS GET: HLS/DASH/VOD files + /{app}/{stream}.flv|.ts|.mp4
 * from the memory GOP publisher (see ffmpeg_http_live.c).
 *
 * Accept thread + bounded worker pool (nginx-style workers, ZMQ-style HWM).
 * Optional TLS via OpenSSL on the accepted TCP fd. Pull auth + IP whitelist.
 */

#include "config.h"

#include "ffmpeg.h"

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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#define http_mkdir(path) mkdir((path), 0755)
#endif

#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"
#include "libavformat/url.h"

#if CONFIG_OPENSSL
#include <openssl/err.h>
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
int   ffmpeg_http_workers = 32;

#define HTTP_JOB_HWM        128
#define HTTP_LISTEN_BACKLOG 512
#define HTTP_SNDBUF         (256 * 1024)

typedef struct HttpConn {
    URLContext *tcp;
    int fd;
    char peer[80];
    char peer_ip[64];
    int family;
    uint8_t addr[16];
    int64_t t0;
    int64_t bytes_out;
    char close_reason[80];
#if CONFIG_OPENSSL
    SSL *ssl;
#endif
} HttpConn;

typedef struct HttpNet {
    int family;
    uint8_t addr[16];
    uint8_t mask[16];
} HttpNet;

static atomic_int http_running;
static atomic_int http_clients;
static pthread_t http_accept_th;
static pthread_t *http_workers;
static int http_nb_workers;
static int http_thread_started;
static URLContext *http_listen;
static char *http_root_abs;
static int http_use_tls;
static HttpNet *http_nets;
static int http_nb_nets;

static HttpConn *http_jobq[HTTP_JOB_HWM];
static int http_q_head, http_q_tail, http_q_count;
static pthread_mutex_t http_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  http_q_cv = PTHREAD_COND_INITIALIZER;

#if CONFIG_OPENSSL
static SSL_CTX *http_ssl_ctx;
#endif

static void http_trace(const HttpConn *c, int level, const char *fmt, ...)
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
    HttpConn *c = conn;
    return (c && c->peer[0]) ? c->peer : "-";
}

void ffmpeg_http_conn_note(void *conn, const char *reason)
{
    HttpConn *c = conn;
    if (!c || !reason || !reason[0])
        return;
    av_strlcpy(c->close_reason, reason, sizeof(c->close_reason));
}

int64_t ffmpeg_http_conn_bytes(void *conn)
{
    HttpConn *c = conn;
    return c ? c->bytes_out : 0;
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

static int allow_match(const HttpConn *c)
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

static void http_fill_peer(HttpConn *c)
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

static void http_sock_tune(int fd, int nonblock_url, URLContext *uc)
{
    int one = 1;
    int snd = HTTP_SNDBUF;

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
    if (uc) {
        uc->rw_timeout = 2000000;
        if (nonblock_url)
            uc->flags |= AVIO_FLAG_NONBLOCK;
    }
}

static void http_conn_free(HttpConn **pc)
{
    HttpConn *c = pc ? *pc : NULL;
    if (!c)
        return;
    http_trace(c, AV_LOG_INFO, "disconnect reason=%s bytes=%"PRId64,
               c->close_reason[0] ? c->close_reason : "done",
               c->bytes_out);
#if CONFIG_OPENSSL
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    ffurl_closep(&c->tcp);
    av_free(c);
    if (pc)
        *pc = NULL;
}

int ffmpeg_http_conn_write(void *conn, const uint8_t *buf, int len)
{
    HttpConn *c = conn;
    int off = 0, ret;

    if (!c || !buf || len <= 0)
        return AVERROR(EINVAL);
    while (off < len) {
#if CONFIG_OPENSSL
        if (c->ssl) {
            ret = SSL_write(c->ssl, buf + off, len - off);
            if (ret <= 0) {
                int err = SSL_get_error(c->ssl, ret);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    av_usleep(1000);
                    continue;
                }
                return AVERROR(EIO);
            }
            off += ret;
            continue;
        }
#endif
        if (!c->tcp)
            return AVERROR(EIO);
        ret = ffurl_write(c->tcp, buf + off, len - off);
        if (ret == AVERROR(EAGAIN)) {
            av_usleep(1000);
            continue;
        }
        if (ret < 0)
            return ret;
        off += ret;
    }
    c->bytes_out += len;
    return 0;
}

static int http_conn_read(HttpConn *c, unsigned char *buf, int len)
{
    int ret;

    if (!c || !buf || len <= 0)
        return AVERROR(EINVAL);
#if CONFIG_OPENSSL
    if (c->ssl) {
        ret = SSL_read(c->ssl, buf, len);
        if (ret > 0)
            return ret;
        if (ret == 0)
            return AVERROR_EOF;
        ret = SSL_get_error(c->ssl, ret);
        if (ret == SSL_ERROR_WANT_READ || ret == SSL_ERROR_WANT_WRITE)
            return AVERROR(EAGAIN);
        return AVERROR(EIO);
    }
#endif
    if (!c->tcp)
        return AVERROR(EIO);
    return ffurl_read(c->tcp, buf, len);
}

static int write_all(HttpConn *c, const char *buf, int len)
{
    return ffmpeg_http_conn_write(c, (const uint8_t *)buf, len);
}

static int send_status(HttpConn *c, int code, const char *text)
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
                 "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
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

static int send_options(HttpConn *c)
{
    const char *hdr =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
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

static int send_file(HttpConn *c, const char *path, int head_only, const char *req)
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
                     "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
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
                     "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
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

static int read_request(HttpConn *c, char *req, int req_sz)
{
    int n = 0, ret;
    int64_t deadline = av_gettime_relative() + 2000000;

    if (c->tcp)
        c->tcp->flags |= AVIO_FLAG_NONBLOCK;
    while (n + 1 < req_sz) {
        int want = FFMIN(4096, req_sz - 1 - n);
        ret = http_conn_read(c, (unsigned char *)req + n, want);
        if (ret == AVERROR(EAGAIN)) {
            if (av_gettime_relative() > deadline)
                break;
            av_usleep(1000);
            continue;
        }
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

static int check_auth(HttpConn *c, const char *req, const char *path)
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
static int http_tls_handshake(HttpConn *c)
{
    int ret, err;
    int64_t deadline;

    if (!http_ssl_ctx)
        return AVERROR(EINVAL);
    c->ssl = SSL_new(http_ssl_ctx);
    if (!c->ssl)
        return AVERROR(ENOMEM);
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_accept_state(c->ssl);
    deadline = av_gettime_relative() + 5000000;
    while (atomic_load(&http_running) && av_gettime_relative() < deadline) {
        ret = SSL_accept(c->ssl);
        if (ret == 1) {
            http_trace(c, AV_LOG_INFO, "tls handshake ok");
            return 0;
        }
        err = SSL_get_error(c->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            av_usleep(2000);
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
static void http_reply_error(HttpConn *c, int code, const char *text)
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
    http_sock_tune(c->fd, 1, c->tcp);
    read_request(c, req, sizeof(req));
    send_status(c, code, text);
    http_trace(c, AV_LOG_WARNING, "http %d %s", code, text);
}

static void http_deny_whitelist(HttpConn *c)
{
    ffmpeg_http_conn_note(c, "deny-whitelist");
    http_trace(c, AV_LOG_WARNING, "deny whitelist");
    http_trace(c, AV_LOG_WARNING, "http 403 Forbidden");
    send_status(c, 403, "Forbidden");
}

static void http_handle_conn(HttpConn *c)
{
    char req[8192], method[16], path[1024], mapped[2048];
    char host[256], ua[256], path_log[1024];
    int n, head_only = 0;
    char *line, *sp1, *sp2;

    if (http_use_tls) {
#if CONFIG_OPENSSL
        if (http_tls_handshake(c) < 0) {
            ffmpeg_http_conn_note(c, "tls-fail");
            return;
        }
#else
        ffmpeg_http_conn_note(c, "no-openssl");
        http_trace(c, AV_LOG_ERROR, "https requested but OpenSSL disabled");
        return;
#endif
    }
    http_sock_tune(c->fd, 1, c->tcp);

    n = read_request(c, req, sizeof(req));
    if (n < 0) {
        if (!allow_match(c)) {
            http_deny_whitelist(c);
            return;
        }
        ffmpeg_http_conn_note(c, "read-fail");
        http_trace(c, AV_LOG_WARNING, "read request failed: %s", av_err2str(n));
        return;
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
        return;
    }
    if (http_nb_nets > 0)
        http_trace(c, AV_LOG_INFO, "allow whitelist");

    if (!av_strcasecmp(method, "OPTIONS")) {
        send_options(c);
        ffmpeg_http_conn_note(c, "options");
        http_trace(c, AV_LOG_INFO, "reply 204 OPTIONS");
        return;
    }
    if (!av_strcasecmp(method, "HEAD"))
        head_only = 1;
    else if (av_strcasecmp(method, "GET")) {
        ffmpeg_http_conn_note(c, "method");
        http_trace(c, AV_LOG_WARNING, "method not allowed");
        send_status(c, 405, "Method Not Allowed");
        return;
    }

    if (!check_auth(c, req, path)) {
        ffmpeg_http_conn_note(c, "auth-fail");
        http_trace(c, AV_LOG_WARNING, "http 401 Unauthorized");
        send_status(c, 401, "Unauthorized");
        return;
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
            return;
        }
    }
file_only:
    if (!http_root_abs) {
        ffmpeg_http_conn_note(c, "not-found");
        http_trace(c, AV_LOG_WARNING, "404 no http_root");
        send_status(c, 404, "Not Found");
        return;
    }
    if (map_url_path(path, mapped, sizeof(mapped)) < 0) {
        ffmpeg_http_conn_note(c, "forbidden");
        http_trace(c, AV_LOG_WARNING, "403 path");
        send_status(c, 403, "Forbidden");
        return;
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
    return;
bad:
    if (!allow_match(c)) {
        http_deny_whitelist(c);
        return;
    }
    ffmpeg_http_conn_note(c, "bad-request");
    http_trace(c, AV_LOG_WARNING, "400 bad request");
    send_status(c, 400, "Bad Request");
}

static int http_job_push(HttpConn *c)
{
    pthread_mutex_lock(&http_q_mu);
    if (http_q_count >= HTTP_JOB_HWM) {
        pthread_mutex_unlock(&http_q_mu);
        return AVERROR(EAGAIN);
    }
    http_jobq[http_q_tail] = c;
    http_q_tail = (http_q_tail + 1) % HTTP_JOB_HWM;
    http_q_count++;
    pthread_cond_signal(&http_q_cv);
    pthread_mutex_unlock(&http_q_mu);
    return 0;
}

static HttpConn *http_job_pop(void)
{
    HttpConn *c = NULL;

    pthread_mutex_lock(&http_q_mu);
    while (atomic_load(&http_running) && http_q_count == 0)
        pthread_cond_wait(&http_q_cv, &http_q_mu);
    if (http_q_count > 0) {
        c = http_jobq[http_q_head];
        http_jobq[http_q_head] = NULL;
        http_q_head = (http_q_head + 1) % HTTP_JOB_HWM;
        http_q_count--;
    }
    pthread_mutex_unlock(&http_q_mu);
    return c;
}

static void *http_client_worker_fn(void *arg)
{
    (void)arg;
    while (atomic_load(&http_running)) {
        HttpConn *c = http_job_pop();
        if (!c)
            break;
        http_handle_conn(c);
        http_conn_free(&c);
        atomic_fetch_sub(&http_clients, 1);
    }
    return NULL;
}

static void *http_accept_worker(void *arg)
{
    (void)arg;
    while (atomic_load(&http_running)) {
        URLContext *client = NULL;
        HttpConn *conn;
        int ret;

        ret = ffurl_accept(http_listen, &client);
        if (ret < 0) {
            if (!atomic_load(&http_running))
                break;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR(ETIMEDOUT))
                continue;
            av_usleep(20 * 1000);
            continue;
        }
        if (!atomic_load(&http_running)) {
            ffurl_closep(&client);
            break;
        }
        conn = av_mallocz(sizeof(*conn));
        if (!conn) {
            ffurl_closep(&client);
            continue;
        }
        conn->tcp = client;
        conn->t0 = av_gettime_relative();
        conn->fd = ffurl_get_file_handle(client);
        http_fill_peer(conn);
        http_sock_tune(conn->fd, 0, client);
        http_trace(conn, AV_LOG_INFO, "connect %s",
                   http_use_tls ? "https" : "http");

        atomic_fetch_add(&http_clients, 1);
        if (http_job_push(conn) < 0) {
            atomic_fetch_sub(&http_clients, 1);
            ffmpeg_http_conn_note(conn, "overload");
            http_trace(conn, AV_LOG_WARNING,
                       "overload (queue=%d), 503", HTTP_JOB_HWM);
            http_reply_error(conn, 503, "Service Unavailable");
            http_conn_free(&conn);
        }
    }
    return NULL;
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
    OPENSSL_init_ssl(0, NULL);
    http_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!http_ssl_ctx)
        return AVERROR(ENOMEM);
    SSL_CTX_set_min_proto_version(http_ssl_ctx, TLS1_2_VERSION);
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
    char host[256], tcp_url[512];
    int port = 8080, ret, i, nworkers;
    AVDictionary *opts = NULL;
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

    snprintf(tcp_url, sizeof(tcp_url), "tcp://%s:%d", host, port);
    av_dict_set(&opts, "listen", "2", 0);
    av_dict_set(&opts, "listen_timeout", "-1", 0);
    av_dict_set(&opts, "timeout", "2000000", 0);
    av_dict_set(&opts, "tcp_nodelay", "1", 0);
    av_dict_set(&opts, "tcp_keepalive", "1", 0);
    av_dict_set(&opts, "send_buffer_size", "262144", 0);
    ret = ffurl_open_whitelist(&http_listen, tcp_url, AVIO_FLAG_READ_WRITE,
                               &int_cb, &opts, "tcp", NULL, NULL);
    av_dict_free(&opts);
    if (ret < 0) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: bind %s failed: %s\n",
               tcp_url, av_err2str(ret));
        return ret;
    }
    {
        int fd = ffurl_get_file_handle(http_listen);
        if (fd >= 0) {
            if (listen(fd, HTTP_LISTEN_BACKLOG) < 0)
                ffmpeg_http_log( AV_LOG_WARNING,
                       "http-server: listen backlog %d failed: %s\n",
                       HTTP_LISTEN_BACKLOG, strerror(errno));
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
        }
    }

    nworkers = ffmpeg_http_workers;
    if (nworkers <= 0)
        nworkers = 32;
    if (nworkers > 256)
        nworkers = 256;
    http_workers = av_calloc(nworkers, sizeof(*http_workers));
    if (!http_workers)
        return AVERROR(ENOMEM);

    atomic_store(&http_running, 1);
    atomic_store(&http_clients, 0);
    http_q_head = http_q_tail = http_q_count = 0;
    for (i = 0; i < nworkers; i++) {
        ret = pthread_create(&http_workers[i], NULL, http_client_worker_fn, NULL);
        if (ret) {
            atomic_store(&http_running, 0);
            pthread_cond_broadcast(&http_q_cv);
            ffmpeg_http_log( AV_LOG_ERROR, "http-server: worker %d failed: %s\n",
                   i, strerror(ret));
            return AVERROR(ret);
        }
        http_nb_workers++;
    }
    ret = pthread_create(&http_accept_th, NULL, http_accept_worker, NULL);
    if (ret) {
        atomic_store(&http_running, 0);
        pthread_cond_broadcast(&http_q_cv);
        ffmpeg_http_log( AV_LOG_ERROR, "http-server: accept thread failed: %s\n",
               strerror(ret));
        return AVERROR(ret);
    }
    http_thread_started = 1;
    ffmpeg_http_log( AV_LOG_INFO,
           "http-server: %s %s workers=%d hwm=%d auth=%s allow=%s files=%s live=%s\n",
           http_use_tls ? "https" : "http", url, nworkers, HTTP_JOB_HWM,
           auth_configured() ? "on" : "off",
           http_nb_nets > 0 ? ffmpeg_http_allow : "any",
           http_root_abs ? http_root_abs : "(off)",
           ffmpeg_http_live ? "gop" : "off");
    return 0;
}

void ffmpeg_http_server_uninit(void)
{
    int64_t wait_us = 0;
    int i;

    if (!http_thread_started)
        return;
    atomic_store(&http_running, 0);
    ffmpeg_http_live_shutdown();
    ffurl_closep(&http_listen);
    pthread_join(http_accept_th, NULL);
    pthread_mutex_lock(&http_q_mu);
    pthread_cond_broadcast(&http_q_cv);
    pthread_mutex_unlock(&http_q_mu);
    for (i = 0; i < http_nb_workers; i++)
        pthread_join(http_workers[i], NULL);
    http_nb_workers = 0;
    av_freep(&http_workers);
    while (http_q_count > 0) {
        HttpConn *c = http_jobq[http_q_head];
        http_jobq[http_q_head] = NULL;
        http_q_head = (http_q_head + 1) % HTTP_JOB_HWM;
        http_q_count--;
        http_conn_free(&c);
        atomic_fetch_sub(&http_clients, 1);
    }
    http_thread_started = 0;
    while (atomic_load(&http_clients) > 0 && wait_us < 2000000) {
        av_usleep(20 * 1000);
        wait_us += 20 * 1000;
    }
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
