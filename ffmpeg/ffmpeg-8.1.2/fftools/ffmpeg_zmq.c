/*
 * Process-level ZMQ command broker.
 * Does not replace graph-local zmq/azmq filters.
 */

#include "config.h"

#include "ffmpeg.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/url.h"

#if CONFIG_LIBZMQ
#include "spfutils.h"
#endif

char *ffmpeg_zmq_url;

enum ZmqKind {
    ZK_FILTER,
    ZK_ENC,
    ZK_DEC,
    ZK_OFMT,
    ZK_IFMT,
    ZK_PROTO,
    ZK_FFMPEG,
};

typedef struct ZmqTarget {
    enum ZmqKind kind;
    int file_idx;
    int stream_idx;
    int media;
    int type_idx;
    int graph_idx;
    int proto_in;
    char name[128];
} ZmqTarget;

#define SPACES " \f\t\n\r"

static int parse_media_tok(const char *s)
{
    if (!s || !s[0])
        return -1;
    if (!strcmp(s, "v") || !strcmp(s, "V") || !strcmp(s, "video"))
        return AVMEDIA_TYPE_VIDEO;
    if (!strcmp(s, "a") || !strcmp(s, "A") || !strcmp(s, "audio"))
        return AVMEDIA_TYPE_AUDIO;
    if (!strcmp(s, "s") || !strcmp(s, "S") || !strcmp(s, "subtitle"))
        return AVMEDIA_TYPE_SUBTITLE;
    if (!strcmp(s, "d") || !strcmp(s, "D") || !strcmp(s, "data"))
        return AVMEDIA_TYPE_DATA;
    return -2;
}

static int split_colon(char *s, char *out[], int max)
{
    int n = 0;
    char *save = NULL;
    char *tok;

    if (!s || !s[0])
        return 0;
    tok = av_strtok(s, ":", &save);
    while (tok && n < max) {
        out[n++] = tok;
        tok = av_strtok(NULL, ":", &save);
    }
    return n;
}

static int parse_io_spec(ZmqTarget *t, char *spec)
{
    char *toks[6] = {0};
    int n, m, i = 0;

    t->file_idx = t->stream_idx = t->media = t->type_idx = t->graph_idx = -1;
    t->name[0] = 0;
    if (!spec || !spec[0] || !strcmp(spec, "all"))
        return 0;

    n = split_colon(spec, toks, 6);
    if (n <= 0)
        return 0;

    m = parse_media_tok(toks[0]);
    if (m == -2 && !isdigit((unsigned char)toks[0][0])) {
        av_strlcpy(t->name, toks[0], sizeof(t->name));
        i = 1;
        if (i >= n)
            return 0;
        m = parse_media_tok(toks[i]);
    }

    if (m >= 0) {
        t->media = m;
        i++;
        if (i < n && isdigit((unsigned char)toks[i][0]))
            t->type_idx = atoi(toks[i]);
        return 0;
    }

    if (isdigit((unsigned char)toks[i][0])) {
        t->file_idx = atoi(toks[i]);
        i++;
    }
    if (i >= n)
        return 0;
    m = parse_media_tok(toks[i]);
    if (m >= 0) {
        t->media = m;
        i++;
        if (i < n && isdigit((unsigned char)toks[i][0]))
            t->type_idx = atoi(toks[i]);
        return 0;
    }
    if (isdigit((unsigned char)toks[i][0])) {
        t->stream_idx = atoi(toks[i]);
        i++;
        if (i < n) {
            m = parse_media_tok(toks[i]);
            if (m >= 0)
                t->media = m;
        }
    } else if (toks[i][0]) {
        av_strlcpy(t->name, toks[i], sizeof(t->name));
    }
    return 0;
}

static int parse_target(ZmqTarget *t, const char *raw)
{
    char buf[256];
    const char *p;

    memset(t, 0, sizeof(*t));
    t->file_idx = t->stream_idx = t->media = t->type_idx = t->graph_idx = -1;
    if (!raw || !raw[0])
        return AVERROR(EINVAL);
    av_strlcpy(buf, raw, sizeof(buf));

    if (!strncmp(buf, "filter:", 7) || !strncmp(buf, "fg:", 3)) {
        t->kind = ZK_FILTER;
        p = strchr(buf, ':') + 1;
        if (!strncmp(buf, "fg:", 3) && isdigit((unsigned char)*p)) {
            t->graph_idx = atoi(p);
            p = strchr(p, ':');
            p = p ? p + 1 : "";
        } else if (isdigit((unsigned char)*p) && strchr(p, ':')) {
            t->graph_idx = atoi(p);
            p = strchr(p, ':') + 1;
        }
        av_strlcpy(t->name, p, sizeof(t->name));
        return 0;
    }
    if (!strncmp(buf, "enc:", 4) || !strncmp(buf, "encoder:", 8) ||
        !strncmp(buf, "codec:", 6)) {
        t->kind = ZK_ENC;
        p = strchr(buf, ':') + 1;
        return parse_io_spec(t, (char *)p);
    }
    if (!strncmp(buf, "dec:", 4) || !strncmp(buf, "decoder:", 8)) {
        t->kind = ZK_DEC;
        p = strchr(buf, ':') + 1;
        return parse_io_spec(t, (char *)p);
    }
    if (!strncmp(buf, "of:", 3) || !strncmp(buf, "fmt:", 4) ||
        !strncmp(buf, "mux:", 4)) {
        t->kind = ZK_OFMT;
        p = strchr(buf, ':') + 1;
        if (isdigit((unsigned char)*p))
            t->file_idx = atoi(p);
        return 0;
    }
    if (!strncmp(buf, "if:", 3) || !strncmp(buf, "demux:", 6)) {
        t->kind = ZK_IFMT;
        p = strchr(buf, ':') + 1;
        if (isdigit((unsigned char)*p))
            t->file_idx = atoi(p);
        return 0;
    }
    if (!strncmp(buf, "proto:", 6) || !strncmp(buf, "protocol:", 9)) {
        t->kind = ZK_PROTO;
        p = strchr(buf, ':') + 1;
        if (!strncmp(p, "if:", 3) || !strncmp(p, "in:", 3)) {
            t->proto_in = 1;
            p = strchr(p, ':') + 1;
        } else if (!strncmp(p, "of:", 3) || !strncmp(p, "out:", 4)) {
            t->proto_in = 0;
            p = strchr(p, ':') + 1;
        }
        if (isdigit((unsigned char)*p))
            t->file_idx = atoi(p);
        return 0;
    }
    if (!strncmp(buf, "ffmpeg:", 7) || !strncmp(buf, "fftools:", 8)) {
        t->kind = ZK_FFMPEG;
        p = strchr(buf, ':') + 1;
        av_strlcpy(t->name, p, sizeof(t->name));
        return 0;
    }

    t->kind = ZK_FILTER;
    av_strlcpy(t->name, buf, sizeof(t->name));
    return 0;
}

static int encoder_name_in_use(const char *name)
{
    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        const AVCodec *c;

        if (!ost->enc || !ost->enc->enc_ctx || !ost->enc->enc_ctx->codec)
            continue;
        c = ost->enc->enc_ctx->codec;
        if (!strcmp(c->name, name))
            return 1;
        if (c->wrapper_name && !strcmp(c->wrapper_name, name))
            return 1;
    }
    return 0;
}

static int demuxer_name_in_use(const char *name)
{
    for (int i = 0; i < nb_input_files; i++) {
        const AVInputFormat *f;

        if (!input_files[i] || !input_files[i]->ctx || !input_files[i]->ctx->iformat)
            continue;
        f = input_files[i]->ctx->iformat;
        if (f->name && !strcmp(f->name, name))
            return 1;
    }
    return 0;
}

static int match_stream_pos(int want_type_idx, int *counter)
{
    int cur;
    if (want_type_idx < 0)
        return 1;
    cur = (*counter)++;
    return cur == want_type_idx;
}

static int ost_match(OutputStream *ost, const ZmqTarget *t, int *type_counter)
{
    const AVCodec *c;

    if (!ost->enc || !ost->enc->enc_ctx)
        return 0;
    if (t->file_idx >= 0 && ost->file->index != t->file_idx)
        return 0;
    if (t->stream_idx >= 0 && ost->index != t->stream_idx)
        return 0;
    if (t->media >= 0 && ost->type != t->media)
        return 0;
    c = ost->enc->enc_ctx->codec;
    if (t->name[0] && c) {
        int name_ok = !strcmp(c->name, t->name) ||
                      (c->wrapper_name && !strcmp(c->wrapper_name, t->name));
        if (!name_ok)
            return 0;
    }
    if (t->type_idx >= 0)
        return match_stream_pos(t->type_idx, type_counter);
    return 1;
}

static int ist_match(InputStream *ist, const ZmqTarget *t, int *type_counter)
{
    if (!ist->decoder)
        return 0;
    if (t->file_idx >= 0 && ist->file->index != t->file_idx)
        return 0;
    if (t->stream_idx >= 0 && ist->index != t->stream_idx)
        return 0;
    if (t->media >= 0 && ist->par && ist->par->codec_type != t->media)
        return 0;
    if (t->name[0] && ist->dec && strcmp(ist->dec->name, t->name))
        return 0;
    if (t->type_idx >= 0)
        return match_stream_pos(t->type_idx, type_counter);
    return 1;
}

static int dispatch_filters(const ZmqTarget *t, const char *cmd, const char *arg)
{
    const char *target = t->name[0] ? t->name : "all";
    int n = 0;

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        if (!ost->fg_simple)
            continue;
        if (t->graph_idx >= 0 && ost->fg_simple->index != t->graph_idx)
            continue;
        fg_send_command(ost->fg_simple, -1, target, cmd, arg, 0);
        n++;
    }
    for (int i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        if (t->graph_idx >= 0 && fg->index != t->graph_idx)
            continue;
        fg_send_command(fg, -1, target, cmd, arg, 0);
        n++;
    }
    return n > 0 ? 0 : AVERROR(ENODEV);
}

static int merge_cmd_ret(int acc, int ret, int *hits)
{
    if (ret == AVERROR(ENOSYS))
        return acc;
    (*hits)++;
    if (acc < 0 && acc != AVERROR(ENOSYS))
        return acc;
    return ret;
}

static int pb_proto_cmd(AVFormatContext *fc, const char *cmd, const char *arg,
                        char *res, int res_len, int flags)
{
    URLContext *h;

    if (!fc || !fc->pb || (fc->flags & AVFMT_FLAG_CUSTOM_IO))
        return AVERROR(ENOSYS);
    h = fc->pb->opaque;
    if (!h || !h->prot)
        return AVERROR(ENOSYS);
    return ffurl_process_command(h, cmd, arg, res, res_len, flags);
}

static int dispatch_one(const ZmqTarget *t, const char *cmd, const char *arg,
                        char *res, int res_len)
{
    int ret = AVERROR(ENOSYS), hits = 0, type_counter = 0;

    switch (t->kind) {
    case ZK_FILTER:
        return dispatch_filters(t, cmd, arg);
    case ZK_ENC:
        for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
            int r;
            if (!ost_match(ost, t, &type_counter))
                continue;
            r = avcodec_process_command(ost->enc->enc_ctx, cmd, arg,
                                        res, res_len, 0);
            ret = merge_cmd_ret(ret, r, &hits);
        }
        return hits ? ret : AVERROR(ENODEV);
    case ZK_DEC:
        for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
            int r;
            if (!ist_match(ist, t, &type_counter))
                continue;
            r = dec_process_command(ist, cmd, arg, res, res_len, 0);
            ret = merge_cmd_ret(ret, r, &hits);
        }
        return hits ? ret : AVERROR(ENODEV);
    case ZK_OFMT:
        for (int i = 0; i < nb_output_files; i++) {
            int r;
            if (t->file_idx >= 0 && i != t->file_idx)
                continue;
            r = of_process_command(output_files[i], cmd, arg, res, res_len, 0);
            ret = merge_cmd_ret(ret, r, &hits);
        }
        return hits ? ret : AVERROR(ENODEV);
    case ZK_IFMT:
        for (int i = 0; i < nb_input_files; i++) {
            int r;
            const AVInputFormat *f;

            if (t->file_idx >= 0 && i != t->file_idx)
                continue;
            if (!input_files[i] || !input_files[i]->ctx)
                continue;
            f = input_files[i]->ctx->iformat;
            if (t->name[0] && (!f || !f->name || strcmp(f->name, t->name)))
                continue;
            r = avformat_process_command(input_files[i]->ctx, cmd, arg,
                                         res, res_len, 0);
            ret = merge_cmd_ret(ret, r, &hits);
        }
        return hits ? ret : AVERROR(ENODEV);
    case ZK_PROTO:
        if (t->proto_in) {
            for (int i = 0; i < nb_input_files; i++) {
                int r;
                if (t->file_idx >= 0 && i != t->file_idx)
                    continue;
                r = pb_proto_cmd(input_files[i]->ctx, cmd, arg, res, res_len, 0);
                ret = merge_cmd_ret(ret, r, &hits);
            }
        } else {
            for (int i = 0; i < nb_output_files; i++) {
                int r;
                if (t->file_idx >= 0 && i != t->file_idx)
                    continue;
                r = of_proto_process_command(output_files[i], cmd, arg,
                                             res, res_len, 0);
                ret = merge_cmd_ret(ret, r, &hits);
            }
        }
        return hits ? ret : AVERROR(ENODEV);
    case ZK_FFMPEG:
        return ffmpeg_process_command(t->name[0] ? t->name : cmd,
                                      t->name[0] ? cmd : arg, res, res_len);
    }
    return AVERROR(EINVAL);
}

static int parse_line(const char *line, char **target, char **command, char **arg)
{
    const char **buf = &line;

    *target = av_get_token(buf, SPACES);
    if (!*target || !(*target)[0])
        return AVERROR(EINVAL);
    *command = av_get_token(buf, SPACES);
    if (!*command || !(*command)[0])
        return AVERROR(EINVAL);
    while (**buf && strchr(SPACES, **buf))
        (*buf)++;
    if (**buf == '\'' || **buf == '"')
        *arg = av_get_token(buf, SPACES);
    else if (**buf)
        *arg = av_strdup(*buf);
    else
        *arg = av_strdup("");
    return *arg ? 0 : AVERROR(ENOMEM);
}

int ffmpeg_zmq_dispatch(const char *line, char *res, int res_len)
{
    char *target = NULL, *command = NULL, *arg = NULL;
    ZmqTarget t;
    int ret;

    if (res && res_len > 0)
        res[0] = 0;
    ret = parse_line(line, &target, &command, &arg);
    if (ret < 0)
        goto end;

    ret = parse_target(&t, target);
    if (ret < 0)
        goto end;

    if (t.kind == ZK_FILTER && encoder_name_in_use(t.name)) {
        t.kind = ZK_ENC;
    }
    if (t.kind == ZK_FILTER && demuxer_name_in_use(t.name)) {
        t.kind = ZK_IFMT;
    }

    if (t.kind == ZK_FILTER &&
        (!strcmp(t.name, "ffmpeg") || !strcmp(t.name, "fftools"))) {
        t.kind = ZK_FFMPEG;
        t.name[0] = 0;
    }

    av_log(NULL, AV_LOG_WARNING, "zmq-proc: target=%s cmd=%s arg='%s'\n",
           target, command, arg ? arg : "");
    ret = dispatch_one(&t, command, arg, res, res_len);

end:
    av_freep(&target);
    av_freep(&command);
    av_freep(&arg);
    return ret;
}

#if CONFIG_LIBZMQ

static atomic_int zmq_running;
static pthread_t zmq_thread;
static IZeromq *zmq_obj;

static void *zmq_worker(void *arg)
{
    (void)arg;
    while (atomic_load(&zmq_running)) {
        char *recv = NULL;
        char reply[1536], res[1024];
        char *send_buf = NULL;
        int ret;

        {
            int n = zmqmsg_recv(zmq_obj, (void **)&recv, 0, 200);
            char *line;
            if (n < 0) {
                free(recv);
                continue;
            }
            line = av_malloc(n + 1);
            if (!line) {
                free(recv);
                continue;
            }
            memcpy(line, recv, n);
            line[n] = 0;
            free(recv);
            recv = line;
        }
        res[0] = 0;
        ret = ffmpeg_zmq_dispatch(recv, res, sizeof(res));
        send_buf = av_asprintf("%d %s%s%s",
                               -ret, av_err2str(ret), res[0] ? "\n" : "", res);
        if (!send_buf) {
            snprintf(reply, sizeof(reply), "%d %s", -AVERROR(ENOMEM),
                     av_err2str(AVERROR(ENOMEM)));
            zmqbuf_send(zmq_obj, reply, strlen(reply), 0);
        } else {
            zmqbuf_send(zmq_obj, send_buf, strlen(send_buf), 0);
            av_log(NULL, ret < 0 ? AV_LOG_ERROR : AV_LOG_WARNING,
                   "zmq-proc: reply %s (raw='%s')\n", send_buf, recv);
        }
        av_freep(&send_buf);
        av_freep(&recv);
    }
    return NULL;
}

int ffmpeg_zmq_init(const char *url)
{
    int err;

    if (!url || !url[0])
        return 0;
    if (zmq_obj)
        return 0;

    zmq_obj = create_zeromq(ZMQ_REP, 1, url);
    if (!zmq_obj) {
        av_log(NULL, AV_LOG_ERROR, "zmq-proc: bind %s failed\n", url);
        return AVERROR_EXTERNAL;
    }
    atomic_store(&zmq_running, 1);
    err = pthread_create(&zmq_thread, NULL, zmq_worker, NULL);
    if (err) {
        atomic_store(&zmq_running, 0);
        delete_zeromq(&zmq_obj);
        av_log(NULL, AV_LOG_ERROR, "zmq-proc: thread failed: %s\n", strerror(err));
        return AVERROR(err);
    }
    av_log(NULL, AV_LOG_INFO,
           "zmq-proc: listening %s (graph zmq/azmq unchanged)\n", url);
    return 0;
}

void ffmpeg_zmq_uninit(void)
{
    if (!zmq_obj)
        return;
    atomic_store(&zmq_running, 0);
    pthread_join(zmq_thread, NULL);
    delete_zeromq(&zmq_obj);
}

#else /* !CONFIG_LIBZMQ */

int ffmpeg_zmq_init(const char *url)
{
    if (url && url[0])
        av_log(NULL, AV_LOG_ERROR, "-zmq requires --enable-libzmq\n");
    return (url && url[0]) ? AVERROR(ENOSYS) : 0;
}

void ffmpeg_zmq_uninit(void)
{
}

#endif
