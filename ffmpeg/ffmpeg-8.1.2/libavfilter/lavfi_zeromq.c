/*
 * ZMQ helper for libavfilter (IZeromq-compatible, see fftools/spfutils.h).
 */

#include "config.h"

#if CONFIG_LIBZMQ

#include "lavfi_zeromq.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zmq.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

static int lavfi_zmq_mkdir_p(const char *dir)
{
    char tmp[1024];
    size_t len, i;

    if (!dir || !*dir)
        return 0;
    av_strlcpy(tmp, dir, sizeof(tmp));
    len = strlen(tmp);
    if (len == 0)
        return 0;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (i = 1; tmp[i]; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int lavfi_zmq_ensure_ipc_dir(const char *zurl)
{
    char path[1024], *slash;

    if (!zurl || av_strncasecmp(zurl, "ipc://", 6))
        return 0;
    av_strlcpy(path, zurl + 6, sizeof(path));
    slash = strrchr(path, '/');
    if (!slash)
        return 0;
    *slash = 0;
    return lavfi_zmq_mkdir_p(path);
}

LavfiZmq *lavfi_zmq_create(int type, int bind, const char *zurl, void *log)
{
    LavfiZmq *z;
    int ret, linger = 0, hwm = 8;

    if (!zurl || !*zurl)
        return NULL;

    if (lavfi_zmq_ensure_ipc_dir(zurl) < 0)
        av_log(log, AV_LOG_WARNING, "lavfi_zmq: mkdir for %s failed\n", zurl);

    z = av_mallocz(sizeof(*z));
    if (!z)
        return NULL;
    z->zurl = av_strdup(zurl);
    if (!z->zurl)
        goto fail;
    z->type = type;
    z->bind = bind;

    z->zctx = zmq_ctx_new();
    if (!z->zctx)
        goto fail;
    z->sock = zmq_socket(z->zctx, type);
    if (!z->sock)
        goto fail;

    zmq_setsockopt(z->sock, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(z->sock, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(z->sock, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    if (type == ZMQ_SUB)
        zmq_setsockopt(z->sock, ZMQ_SUBSCRIBE, "", 0);

    if (bind)
        ret = zmq_bind(z->sock, zurl);
    else
        ret = zmq_connect(z->sock, zurl);
    if (ret != 0) {
        av_log(log, AV_LOG_WARNING, "lavfi_zmq: %s %s failed: %s\n",
               bind ? "bind" : "connect", zurl, zmq_strerror(errno));
        goto fail;
    }

    av_log(log, AV_LOG_INFO,
           "lavfi_zmq: create ok (zurl=%s type=%d bind=%d sock=%p)\n",
           z->zurl, type, bind, z->sock);
    return z;

fail:
    lavfi_zmq_destroy(&z);
    return NULL;
}

void lavfi_zmq_destroy(LavfiZmq **pz)
{
    LavfiZmq *z;
    if (!pz || !*pz)
        return;
    z = *pz;
    if (z->sock) {
        zmq_close(z->sock);
        z->sock = NULL;
    }
    if (z->zctx) {
        zmq_ctx_destroy(z->zctx);
        z->zctx = NULL;
    }
    av_freep(&z->zurl);
    av_freep(pz);
}

int lavfi_zmq_send(LavfiZmq *z, const void *buf, size_t size, int timeout_ms)
{
    zmq_pollitem_t items;
    int n;

    if (!z || !z->sock || !buf)
        return AVERROR(EINVAL);
    items.socket = z->sock;
    items.fd = 0;
    items.events = ZMQ_POLLOUT;
    items.revents = 0;
    if (zmq_poll(&items, 1, timeout_ms) < 0)
        return AVERROR_EXTERNAL;
    if (!(items.revents & ZMQ_POLLOUT))
        return AVERROR(EAGAIN);
    n = zmq_send(z->sock, buf, size, ZMQ_DONTWAIT);
    if (n < 0)
        return (errno == EAGAIN) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
    return n;
}

int lavfi_zmq_recv(LavfiZmq *z, void *buf, size_t capa, int timeout_ms)
{
    zmq_pollitem_t items;
    int n;

    if (!z || !z->sock || !buf || capa == 0)
        return AVERROR(EINVAL);
    items.socket = z->sock;
    items.fd = 0;
    items.events = ZMQ_POLLIN;
    items.revents = 0;
    if (zmq_poll(&items, 1, timeout_ms) < 0)
        return AVERROR_EXTERNAL;
    if (!(items.revents & ZMQ_POLLIN))
        return AVERROR(EAGAIN);
    n = zmq_recv(z->sock, buf, capa, ZMQ_DONTWAIT);
    if (n < 0)
        return (errno == EAGAIN) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
    return n;
}

int lavfi_zmq_task_id(int option_task_id)
{
    const char *e;
    int tid;

    if (option_task_id > 0)
        return option_task_id;
    e = getenv("FFMPEG_TASK_ID");
    if (e && *e) {
        tid = atoi(e);
        if (tid > 0)
            return tid;
    }
    /* Same-process PUB/SUB (amixrank→mixing_cuda) without -task_id. */
    tid = (int)getpid();
    return tid > 0 ? tid : 0;
}

int lavfi_zmq_resolve_url(char *dst, size_t dst_sz,
                          const char *prefix, int task_id,
                          const char *explicit_endpoint)
{
    const char *e;
    int tid = 0;

    if (!dst || dst_sz == 0 || !prefix)
        return AVERROR(EINVAL);

    /*
     * Priority:
     *  1) filter task_id / -task_id / FFMPEG_TASK_ID → ipc …/<prefix>_<id>.sock
     *  2) explicit endpoint (rank_endpoint etc.) when no task id
     *  3) getpid() → ipc …/<prefix>_<pid>.sock (in-process amixrank↔mixing default)
     */
    if (task_id > 0)
        tid = task_id;
    else if ((e = getenv("FFMPEG_TASK_ID")) && *e)
        tid = atoi(e);

    if (tid > 0) {
        snprintf(dst, dst_sz, "ipc:///data/LCMS/sock/%s_%d.sock", prefix, tid);
        return 0;
    }
    if (explicit_endpoint && *explicit_endpoint) {
        av_strlcpy(dst, explicit_endpoint, dst_sz);
        return 0;
    }
    tid = (int)getpid();
    if (tid > 0) {
        snprintf(dst, dst_sz, "ipc:///data/LCMS/sock/%s_%d.sock", prefix, tid);
        return 0;
    }
    dst[0] = 0;
    return AVERROR(ENOENT);
}

#endif /* CONFIG_LIBZMQ */
