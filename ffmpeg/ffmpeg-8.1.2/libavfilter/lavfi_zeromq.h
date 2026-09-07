/*
 * Lightweight ZMQ helper for libavfilter, API-aligned with fftools/spfutils.h IZeromq.
 * Prefers ipc:///data/LCMS/sock/<prefix>_<id>.sock
 * (task_id / FFMPEG_TASK_ID / getpid() fallback).
 */
#ifndef AVFILTER_LAVFI_ZEROMQ_H
#define AVFILTER_LAVFI_ZEROMQ_H

#include "config.h"

#if CONFIG_LIBZMQ

#include <stddef.h>

typedef struct LavfiZmq {
    char *zurl;
    int   type;
    int   bind;
    void *zctx;
    void *sock;
} LavfiZmq;

/**
 * Create ZMQ socket (same roles as create_zeromq in spfutils.h).
 * @param type  ZMQ_REP / ZMQ_PUB / ZMQ_SUB / ...
 * @param bind  1=bind (server), 0=connect (client)
 * @param zurl  endpoint, e.g. ipc:///data/LCMS/sock/gain_123.sock
 * @param log   av_log context (filter) or NULL
 */
LavfiZmq *lavfi_zmq_create(int type, int bind, const char *zurl, void *log);

void lavfi_zmq_destroy(LavfiZmq **pz);

/** Send; timeout_ms: -1 block, 0 non-block poll, >0 wait. Returns bytes or <0. */
int lavfi_zmq_send(LavfiZmq *z, const void *buf, size_t size, int timeout_ms);

/** Recv into buf[capa]; returns bytes or <0 / AVERROR(EAGAIN). */
int lavfi_zmq_recv(LavfiZmq *z, void *buf, size_t capa, int timeout_ms);

/**
 * Resolve endpoint (spfutils / LCMS convention):
 *   1) task_id / FFMPEG_TASK_ID → ipc:///data/LCMS/sock/<prefix>_<id>.sock
 *   2) else explicit non-empty endpoint
 *   3) else getpid() → ipc:///data/LCMS/sock/<prefix>_<pid>.sock
 * @param prefix e.g. "gain" or "cmd"
 */
int lavfi_zmq_resolve_url(char *dst, size_t dst_sz,
                          const char *prefix, int task_id,
                          const char *explicit_endpoint);

/** task_id from filter option, else FFMPEG_TASK_ID, else getpid(). */
int lavfi_zmq_task_id(int option_task_id);

#endif /* CONFIG_LIBZMQ */
#endif /* AVFILTER_LAVFI_ZEROMQ_H */
