/*
 * In-process HTTP live publisher: per-output GOP cache + per-client mux.
 * Serves /{app}/{stream}.flv|.ts|.mp4 from encoded packets, not growing files.
 */

#include "config.h"

#include "ffmpeg.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavutil/intreadwrite.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavformat/avc.h"
#include "libavformat/hevc.h"
#include "libavformat/nal.h"
#include "libavformat/url.h"

int ffmpeg_http_live = 1;

#define LIVE_SUB_MAX_PKTS   256
#define LIVE_GOP_MAX_PKTS   512
#define LIVE_WAIT_PAR_US    (10 * 1000000LL)
#define LIVE_AVIO_BUF       65536

typedef struct LiveNode {
    AVPacket *pkt;
    int st;
    struct LiveNode *next;
} LiveNode;

typedef struct LiveList {
    LiveNode *head;
    LiveNode *tail;
    int count;
    int has_key;
} LiveList;

typedef struct HttpLivePub HttpLivePub;

typedef struct LiveSub {
    LiveList q;
    int overflow;
    int wait_key;
    HttpLivePub *pub;
    struct LiveSub *next;
} LiveSub;

struct HttpLivePub {
    char app[128];
    char stream[128];
    int file_index;
    int v_src;
    int a_src;
    AVCodecParameters *vpar;
    AVCodecParameters *apar;
    AVRational vtb;
    AVRational atb;
    LiveList ready;
    LiveList building;
    LiveSub *subs;
};

typedef struct LiveBindSpec {
    char app[128];
    char stream[128];
    int file_index;
} LiveBindSpec;

typedef struct LiveAvio {
    void *conn;
    int started;
} LiveAvio;

static pthread_mutex_t live_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  live_cond  = PTHREAD_COND_INITIALIZER;
static atomic_int live_inited;
static atomic_int live_running;

static LiveBindSpec *bind_specs;
static int nb_bind_specs;
static HttpLivePub *pubs;
static int nb_pubs;

static void live_list_free(LiveList *l)
{
    LiveNode *n;

    if (!l)
        return;
    n = l->head;
    while (n) {
        LiveNode *next = n->next;
        av_packet_free(&n->pkt);
        av_free(n);
        n = next;
    }
    memset(l, 0, sizeof(*l));
}

static int live_list_append_owned(LiveList *l, AVPacket *pkt, int st)
{
    LiveNode *n = av_mallocz(sizeof(*n));

    if (!n)
        return AVERROR(ENOMEM);
    n->pkt = pkt;
    n->st = st;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
    l->count++;
    if (st == 0 && pkt && (pkt->flags & AV_PKT_FLAG_KEY))
        l->has_key = 1;
    return 0;
}

static int live_list_append_ref(LiveList *l, const AVPacket *src, int st)
{
    AVPacket *pkt = av_packet_alloc();
    int ret;

    if (!pkt)
        return AVERROR(ENOMEM);
    ret = av_packet_ref(pkt, src);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ret;
    }
    pkt->stream_index = st;
    ret = live_list_append_owned(l, pkt, st);
    if (ret < 0)
        av_packet_free(&pkt);
    return ret;
}

static int live_list_copy_ref(LiveList *dst, const LiveList *src)
{
    const LiveNode *n;

    for (n = src->head; n; n = n->next) {
        int ret = live_list_append_ref(dst, n->pkt, n->st);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static void live_rotate_gop(HttpLivePub *pub)
{
    live_list_free(&pub->ready);
    pub->ready = pub->building;
    memset(&pub->building, 0, sizeof(pub->building));
}

static int live_token_ok(const char *s)
{
    if (!s || !s[0] || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    if (strchr(s, '/') || strchr(s, '\\'))
        return 0;
    return 1;
}

static int live_ensure_par(HttpLivePub *pub, const OutputStream *ost,
                           const AVPacket *pkt, int is_video)
{
    AVCodecParameters **dst = is_video ? &pub->vpar : &pub->apar;
    AVRational *tb = is_video ? &pub->vtb : &pub->atb;
    int ret;

    if (*dst)
        return 0;
    *dst = avcodec_parameters_alloc();
    if (!*dst)
        return AVERROR(ENOMEM);
    if (ost->enc && ost->enc->enc_ctx)
        ret = avcodec_parameters_from_context(*dst, ost->enc->enc_ctx);
    else if (ost->st && ost->st->codecpar)
        ret = avcodec_parameters_copy(*dst, ost->st->codecpar);
    else
        ret = AVERROR(EINVAL);
    if (ret < 0) {
        avcodec_parameters_free(dst);
        return ret;
    }
    if (pkt->time_base.num)
        *tb = pkt->time_base;
    else if (ost->st)
        *tb = ost->st->time_base;
    else
        *tb = is_video ? (AVRational){ 1, 90000 } : (AVRational){ 1, 48000 };
    if (!is_video && ost->st && ost->st->codecpar &&
        !(*dst)->extradata_size && ost->st->codecpar->extradata_size > 0) {
        uint8_t *e = av_mallocz(ost->st->codecpar->extradata_size +
                                AV_INPUT_BUFFER_PADDING_SIZE);
        if (e) {
            memcpy(e, ost->st->codecpar->extradata,
                   ost->st->codecpar->extradata_size);
            av_freep(&(*dst)->extradata);
            (*dst)->extradata = e;
            (*dst)->extradata_size = ost->st->codecpar->extradata_size;
        }
    }
    return 0;
}

static int live_is_annexb(const uint8_t *p, int n)
{
    return p && n >= 4 &&
           (AV_RB32(p) == 0x00000001 || AV_RB24(p) == 0x000001);
}

static int live_mux_avcc(const char *fmt)
{
    return fmt && (!strcmp(fmt, "flv") || !strcmp(fmt, "mp4"));
}

static int live_extra_is_avcc(const AVCodecParameters *par)
{
    if (!par || !par->extradata || par->extradata_size < 7)
        return 0;
    if (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC)
        return par->extradata[0] == 1;
    return 0;
}

static int live_apply_extradata(AVCodecParameters *par, const uint8_t *data, int size)
{
    if (!par || !data || size <= 0 || par->extradata_size > 0)
        return 0;
    av_freep(&par->extradata);
    par->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!par->extradata)
        return AVERROR(ENOMEM);
    memcpy(par->extradata, data, size);
    par->extradata_size = size;
    return 1;
}

/* GOP packets stay encoder-native (NVENC Annex B). FLV/MP4 need AVCC extra
 * and length-prefixed NALs; a homemade AVCC extra with extra[0]==1 makes the
 * muxer skip conversion and write start codes as NAL sizes (0x0104xxxx). */
static int live_par_to_avcc(AVCodecParameters *par, const AVPacket *key)
{
    AVIOContext *pb = NULL;
    const uint8_t *src;
    uint8_t *buf = NULL;
    int len, size, ret;

    if (!par)
        return 0;
    if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC)
        return 0;
    if (live_extra_is_avcc(par))
        return 0;
    if (live_is_annexb(par->extradata, par->extradata_size)) {
        src = par->extradata;
        len = par->extradata_size;
    } else if (key && live_is_annexb(key->data, key->size)) {
        src = key->data;
        len = key->size;
    } else {
        return 0;
    }
    ret = avio_open_dyn_buf(&pb);
    if (ret < 0)
        return ret;
    if (par->codec_id == AV_CODEC_ID_H264)
        ret = ff_isom_write_avcc(pb, src, len);
    else
        ret = ff_isom_write_hvcc(pb, src, len, 0, NULL);
    size = avio_close_dyn_buf(pb, &buf);
    if (ret < 0) {
        av_free(buf);
        return ret;
    }
    if (size <= 0 || !buf) {
        av_free(buf);
        return 0;
    }
    av_freep(&par->extradata);
    par->extradata = buf;
    par->extradata_size = size;
    return 1;
}

static int live_pkt_to_avcc(AVPacket *pkt, enum AVCodecID id)
{
    uint8_t *buf = NULL;
    int size, ret;

    if (!pkt || !live_is_annexb(pkt->data, pkt->size))
        return 0;
    size = pkt->size;
    if (id == AV_CODEC_ID_H264)
        ret = ff_nal_parse_units_buf(pkt->data, &buf, &size);
    else if (id == AV_CODEC_ID_HEVC)
        ret = ff_hevc_annexb2mp4_buf(pkt->data, &buf, &size, 0, NULL);
    else
        return 0;
    if (ret < 0)
        return ret;
    av_buffer_unref(&pkt->buf);
    pkt->data = NULL;
    pkt->size = 0;
    ret = av_packet_from_data(pkt, buf, size);
    if (ret < 0)
        av_free(buf);
    return ret;
}

static const AVPacket *live_first_video_key(const HttpLivePub *pub)
{
    const LiveNode *n;

    if (!pub)
        return NULL;
    for (n = pub->building.head; n; n = n->next) {
        if (n->st == 0 && n->pkt && n->pkt->size > 0 &&
            (n->pkt->flags & AV_PKT_FLAG_KEY))
            return n->pkt;
    }
    for (n = pub->ready.head; n; n = n->next) {
        if (n->st == 0 && n->pkt && n->pkt->size > 0 &&
            (n->pkt->flags & AV_PKT_FLAG_KEY))
            return n->pkt;
    }
    return NULL;
}

static const AVPacket *live_first_audio(const HttpLivePub *pub)
{
    const LiveNode *n;

    if (!pub)
        return NULL;
    for (n = pub->building.head; n; n = n->next) {
        if (n->st == 1 && n->pkt && n->pkt->size > 0)
            return n->pkt;
    }
    for (n = pub->ready.head; n; n = n->next) {
        if (n->st == 1 && n->pkt && n->pkt->size > 0)
            return n->pkt;
    }
    return NULL;
}

static int live_try_fill_extradata(AVCodecParameters *par, const AVPacket *pkt)
{
    size_t sd_sz = 0;
    const uint8_t *sd;

    if (!par || par->extradata_size > 0 || !pkt)
        return 0;
    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &sd_sz);
    if (sd && sd_sz > 0)
        return live_apply_extradata(par, sd, (int)sd_sz);
    return 0;
}

/* mpegts wraps raw AAC with a nested ADTS muxer only if AudioSpecificConfig
 * is on codecpar. FLV/MP4 need ASC extra + raw frames; ADTS packets must be
 * stripped (aac_adtstoasc). GOP keeps encoder-native packets. */
static int live_aac_is_adts(const uint8_t *p, int n)
{
    return p && n >= 7 && (AV_RB16(p) & 0xfff0) == 0xfff0;
}

static int live_aac_asc_from_adts(AVCodecParameters *par, const AVPacket *pkt)
{
    int obj, sr_idx, ch;
    uint8_t *e;

    if (!par || par->codec_id != AV_CODEC_ID_AAC || par->extradata_size > 0)
        return 0;
    if (!pkt || !live_aac_is_adts(pkt->data, pkt->size))
        return 0;
    obj = ((pkt->data[2] >> 6) & 3) + 1;
    sr_idx = (pkt->data[2] >> 2) & 0xf;
    ch = ((pkt->data[2] & 3) << 2) | ((pkt->data[3] >> 6) & 3);
    if (sr_idx > 12 || ch < 1 || ch > 7)
        return 0;
    e = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!e)
        return AVERROR(ENOMEM);
    e[0] = (uint8_t)((obj << 3) | (sr_idx >> 1));
    e[1] = (uint8_t)(((sr_idx & 1) << 7) | (ch << 3));
    av_freep(&par->extradata);
    par->extradata = e;
    par->extradata_size = 2;
    return 1;
}

static int live_aac_strip_adts(AVPacket *pkt)
{
    int hdr, crc_absent, frame_len;

    if (!pkt || !live_aac_is_adts(pkt->data, pkt->size))
        return 0;
    crc_absent = pkt->data[1] & 1;
    hdr = crc_absent ? 7 : 9;
    frame_len = ((pkt->data[3] & 3) << 11) | (pkt->data[4] << 3) |
                (pkt->data[5] >> 5);
    if (frame_len < hdr || frame_len > pkt->size)
        frame_len = pkt->size;
    if (hdr >= frame_len)
        return AVERROR_INVALIDDATA;
    pkt->data += hdr;
    pkt->size = frame_len - hdr;
    return 1;
}

static int live_aac_ensure_asc(AVCodecParameters *par)
{
    static const int srate[] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    int sr_idx = -1, i, obj, ch;
    uint8_t *e;

    if (!par || par->codec_id != AV_CODEC_ID_AAC || par->extradata_size > 0)
        return 0;
    if (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0)
        return 0;
    for (i = 0; i < (int)FF_ARRAY_ELEMS(srate); i++) {
        if (srate[i] == par->sample_rate) {
            sr_idx = i;
            break;
        }
    }
    if (sr_idx < 0)
        return 0;
    if (par->profile >= AV_PROFILE_AAC_MAIN && par->profile <= AV_PROFILE_AAC_LTP)
        obj = par->profile + 1;
    else
        obj = 2;
    ch = par->ch_layout.nb_channels;
    if (ch == 8)
        ch = 7;
    if (ch < 1 || ch > 7)
        return 0;
    e = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!e)
        return AVERROR(ENOMEM);
    e[0] = (uint8_t)((obj << 3) | (sr_idx >> 1));
    e[1] = (uint8_t)(((sr_idx & 1) << 7) | (ch << 3));
    av_freep(&par->extradata);
    par->extradata = e;
    par->extradata_size = 2;
    return 1;
}

static void live_fill_extradata_from_gop(HttpLivePub *pub)
{
    LiveNode *n;

    if (!pub)
        return;
    if (pub->vpar && !pub->vpar->extradata_size) {
        for (n = pub->building.head; n; n = n->next) {
            if (n->st == 0 && n->pkt && (n->pkt->flags & AV_PKT_FLAG_KEY)) {
                live_try_fill_extradata(pub->vpar, n->pkt);
                if (pub->vpar->extradata_size)
                    break;
            }
        }
        if (pub->vpar && !pub->vpar->extradata_size) {
            for (n = pub->ready.head; n; n = n->next) {
                if (n->st == 0 && n->pkt && (n->pkt->flags & AV_PKT_FLAG_KEY)) {
                    live_try_fill_extradata(pub->vpar, n->pkt);
                    if (pub->vpar->extradata_size)
                        break;
                }
            }
        }
    }
    if (pub->apar && !pub->apar->extradata_size) {
        for (n = pub->building.head; n; n = n->next) {
            if (n->st == 1 && n->pkt) {
                live_try_fill_extradata(pub->apar, n->pkt);
                if (pub->apar->extradata_size)
                    break;
            }
        }
        if (pub->apar && !pub->apar->extradata_size) {
            for (n = pub->ready.head; n; n = n->next) {
                if (n->st == 1 && n->pkt) {
                    live_try_fill_extradata(pub->apar, n->pkt);
                    if (pub->apar->extradata_size)
                        break;
                }
            }
        }
        live_aac_ensure_asc(pub->apar);
    }
}

static const char *live_cache_name(const HttpLivePub *pub)
{
    if (pub->building.has_key)
        return "building";
    if (pub->ready.head)
        return "ready";
    return "none";
}

static void live_refresh_par(HttpLivePub *pub, const OutputStream *ost,
                             const AVPacket *pkt, int is_video)
{
    AVCodecParameters **dst = is_video ? &pub->vpar : &pub->apar;

    if (!*dst)
        return;
    if (!(*dst)->extradata_size && ost->enc && ost->enc->enc_ctx &&
        ost->enc->enc_ctx->extradata && ost->enc->enc_ctx->extradata_size > 0)
        avcodec_parameters_from_context(*dst, ost->enc->enc_ctx);
    if (!(*dst)->extradata_size && ost->st && ost->st->codecpar &&
        ost->st->codecpar->extradata_size > 0)
        live_apply_extradata(*dst, ost->st->codecpar->extradata,
                             ost->st->codecpar->extradata_size);
    if (pkt)
        live_try_fill_extradata(*dst, pkt);
    if (!is_video)
        live_aac_ensure_asc(*dst);
}

static void live_fanout(HttpLivePub *pub, const AVPacket *pkt, int st)
{
    LiveSub *s;

    for (s = pub->subs; s; s = s->next) {
        if (s->overflow)
            continue;
        if (s->wait_key) {
            if (st != 0 || !pkt || !(pkt->flags & AV_PKT_FLAG_KEY))
                continue;
            s->wait_key = 0;
        }
        if (s->q.count >= LIVE_SUB_MAX_PKTS) {
            s->overflow = 1;
            continue;
        }
        if (live_list_append_ref(&s->q, pkt, st) < 0)
            s->overflow = 1;
    }
}

static void live_sub_detach(LiveSub *sub)
{
    LiveSub **pp;
    HttpLivePub *pub;

    if (!sub || !sub->pub)
        return;
    pub = sub->pub;
    for (pp = &pub->subs; *pp; pp = &(*pp)->next) {
        if (*pp == sub) {
            *pp = sub->next;
            break;
        }
    }
    live_list_free(&sub->q);
    sub->next = NULL;
    sub->pub = NULL;
}

static int live_write_all(void *conn, const unsigned char *buf, int len)
{
    return ffmpeg_http_conn_write(conn, buf, len);
}

static int live_send_status(void *c, int code, const char *text)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "\r\n"
                     "%s",
                     code, text, (int)strlen(text), text);
    return live_write_all(c, (const unsigned char *)hdr, n);
}

static int live_send_ok_hdr(void *c, const char *mime)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                     "Cache-Control: no-cache, no-store\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "\r\n",
                     mime);
    return live_write_all(c, (const unsigned char *)hdr, n);
}

static int live_avio_write(void *opaque, const uint8_t *buf, int buf_size)
{
    LiveAvio *a = opaque;
    int ret;

    if (!a || !a->conn || buf_size <= 0)
        return AVERROR(EINVAL);
    ret = live_write_all(a->conn, buf, buf_size);
    if (ret < 0) {
        ffmpeg_http_conn_note(a->conn, "write-fail");
        return ret;
    }
    if (!a->started) {
        a->started = 1;
        ffmpeg_http_log( AV_LOG_INFO, "http-live: [%s] first media %d bytes\n",
               ffmpeg_http_conn_peer(a->conn), buf_size);
    }
    return buf_size;
}

static int live_cond_wait_until(int64_t deadline_rel)
{
    int64_t now = av_gettime_relative();
    int64_t left;
    int64_t abs_us;
    struct timespec ts;

    if (now >= deadline_rel)
        return ETIMEDOUT;
    left = deadline_rel - now;
    abs_us = av_gettime() + left;
    ts.tv_sec  = (time_t)(abs_us / 1000000);
    ts.tv_nsec = (long)((abs_us % 1000000) * 1000);
    return pthread_cond_timedwait(&live_cond, &live_mutex, &ts);
}

static int live_query_flag(const char *query, const char *key)
{
    char pat[64];
    const char *p;

    if (!query || !key)
        return 0;
    snprintf(pat, sizeof(pat), "%s=", key);
    p = av_stristr(query, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    return *p == '1' || !av_strncasecmp(p, "true", 4) ||
           !av_strncasecmp(p, "yes", 3);
}

static int live_ext_fmt(const char *dot, const char **fmt, const char **mime)
{
    if (!av_strcasecmp(dot, "flv")) {
        *fmt = "flv";
        *mime = "video/x-flv";
        return 1;
    }
    if (!av_strcasecmp(dot, "ts") || !av_strcasecmp(dot, "m2t") ||
        !av_strcasecmp(dot, "m2ts")) {
        *fmt = "mpegts";
        *mime = "video/mp2t";
        return 1;
    }
    if (!av_strcasecmp(dot, "mp4")) {
        *fmt = "mp4";
        *mime = "video/mp4";
        return 1;
    }
    return 0;
}

static HttpLivePub *live_find_pub(const char *app, const char *stream)
{
    int i;

    for (i = 0; i < nb_pubs; i++) {
        if (!strcmp(pubs[i].app, app) && !strcmp(pubs[i].stream, stream))
            return &pubs[i];
    }
    return NULL;
}

static int live_gop_cached(const HttpLivePub *pub)
{
    return pub->ready.head || pub->building.has_key;
}

static int live_wait_ready(HttpLivePub *pub, int head_only, int lowdelay)
{
    int64_t deadline;

    if (!atomic_load(&live_running))
        return AVERROR_EOF;
    live_fill_extradata_from_gop(pub);
    /* Have codecpar + (HEAD / lowdelay / cached GOP): start now.
     * Do not wait for extradata or the next closed GOP. NVENC often
     * never fills codecpar.extradata; SPS/PPS are in the IDR. FLV/MP4
     * convert Annex B → AVCC at serve time, not by forging extra[0]=1. */
    if (pub->vpar && (head_only || lowdelay || live_gop_cached(pub)))
        return 0;

    deadline = av_gettime_relative() + (pub->vpar ? 2000000LL : LIVE_WAIT_PAR_US);
    while (atomic_load(&live_running)) {
        live_fill_extradata_from_gop(pub);
        if (pub->vpar && (head_only || lowdelay || live_gop_cached(pub)))
            return 0;
        if (live_cond_wait_until(deadline) == ETIMEDOUT)
            break;
    }
    if (!atomic_load(&live_running))
        return AVERROR_EOF;
    if (!pub->vpar)
        return AVERROR(ETIMEDOUT);
    if (head_only || lowdelay)
        return 0;
    if (!live_gop_cached(pub))
        return AVERROR(ETIMEDOUT);
    return 0;
}

static int live_subscribe(HttpLivePub *pub, LiveSub *sub, int lowdelay)
{
    int ret;

    memset(sub, 0, sizeof(*sub));
    sub->pub = pub;
    if (lowdelay) {
        sub->wait_key = 1;
    } else if (pub->building.has_key) {
        ret = live_list_copy_ref(&sub->q, &pub->building);
        if (ret < 0)
            return ret;
    } else if (pub->ready.head) {
        ret = live_list_copy_ref(&sub->q, &pub->ready);
        if (ret < 0)
            return ret;
    } else {
        return AVERROR(EAGAIN);
    }
    sub->next = pub->subs;
    pub->subs = sub;
    return 0;
}

static int live_write_pkt(AVFormatContext *oc, AVPacket *pkt,
                          AVRational src_tb, int out_st,
                          int64_t *off_us, int have_off, int to_avcc,
                          int to_asc)
{
    AVStream *st;
    AVPacket *out;
    int ret;

    if (out_st < 0 || out_st >= oc->nb_streams)
        return 0;
    st = oc->streams[out_st];
    out = av_packet_clone(pkt);
    if (!out)
        return AVERROR(ENOMEM);
    out->stream_index = out_st;
    if (have_off && *off_us != AV_NOPTS_VALUE) {
        int64_t off = av_rescale_q(*off_us, AV_TIME_BASE_Q, src_tb);
        if (out->pts != AV_NOPTS_VALUE)
            out->pts -= off;
        if (out->dts != AV_NOPTS_VALUE)
            out->dts -= off;
    }
    av_packet_rescale_ts(out, src_tb, st->time_base);
    if (to_avcc && st->codecpar &&
        st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = live_pkt_to_avcc(out, st->codecpar->codec_id);
        if (ret < 0) {
            av_packet_free(&out);
            return ret;
        }
        /* Annex B NEW_EXTRADATA would make flvenc/movenc think the
         * already-converted AVCC packet is still start-code formatted. */
        if (out->side_data && out->side_data_elems)
            av_packet_side_data_remove(out->side_data, &out->side_data_elems,
                                       AV_PKT_DATA_NEW_EXTRADATA);
    }
    if (to_asc && st->codecpar &&
        st->codecpar->codec_id == AV_CODEC_ID_AAC) {
        ret = live_aac_strip_adts(out);
        if (ret < 0) {
            av_packet_free(&out);
            return ret;
        }
    }
    ret = av_write_frame(oc, out);
    av_packet_free(&out);
    if (ret >= 0 && oc->pb)
        avio_flush(oc->pb);
    return ret;
}

static int live_add_spec(const char *app, const char *stream, int file_index)
{
    LiveBindSpec *p;
    int i;

    if (!live_token_ok(app) || !live_token_ok(stream))
        return AVERROR(EINVAL);
    for (i = 0; i < nb_bind_specs; i++) {
        if (!strcmp(bind_specs[i].app, app) &&
            !strcmp(bind_specs[i].stream, stream)) {
            ffmpeg_http_log( AV_LOG_ERROR,
                   "http-live: duplicate bind /%s/%s\n", app, stream);
            return AVERROR(EEXIST);
        }
    }
    p = av_realloc_array(bind_specs, nb_bind_specs + 1, sizeof(*bind_specs));
    if (!p)
        return AVERROR(ENOMEM);
    bind_specs = p;
    memset(&bind_specs[nb_bind_specs], 0, sizeof(*bind_specs));
    av_strlcpy(bind_specs[nb_bind_specs].app, app,
               sizeof(bind_specs[nb_bind_specs].app));
    av_strlcpy(bind_specs[nb_bind_specs].stream, stream,
               sizeof(bind_specs[nb_bind_specs].stream));
    bind_specs[nb_bind_specs].file_index = file_index;
    nb_bind_specs++;
    return 0;
}

int ffmpeg_http_live_add_bind(const char *arg)
{
    char buf[256], app[128], stream[128];
    char *slash, *colon;
    int file_index = -1;

    if (!arg || !arg[0])
        return AVERROR(EINVAL);
    while (*arg == '/')
        arg++;
    av_strlcpy(buf, arg, sizeof(buf));
    colon = strrchr(buf, ':');
    if (colon && colon[1] &&
        strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
        file_index = atoi(colon + 1);
        *colon = 0;
    }
    slash = strchr(buf, '/');
    if (!slash || slash == buf || !slash[1]) {
        ffmpeg_http_log( AV_LOG_ERROR,
               "http-live: -http_live_bind expects app/stream[:index], got '%s'\n",
               arg);
        return AVERROR(EINVAL);
    }
    *slash = 0;
    av_strlcpy(app, buf, sizeof(app));
    av_strlcpy(stream, slash + 1, sizeof(stream));
    if (file_index < 0)
        file_index = nb_bind_specs;
    return live_add_spec(app, stream, file_index);
}

int ffmpeg_http_live_enabled(void)
{
    return atomic_load(&live_inited) && atomic_load(&live_running) &&
           ffmpeg_http_live;
}

int ffmpeg_http_live_match(const char *path, const char **fmt, const char **mime,
                           int *lowdelay, int *pub_idx)
{
    char buf[512], app[128], stream[128];
    char *slash, *dot, *cut;
    const char *p = path, *query = NULL;
    HttpLivePub *pub;

    if (lowdelay)
        *lowdelay = 0;
    if (pub_idx)
        *pub_idx = -1;
    if (!path || !fmt || !mime || !ffmpeg_http_live_enabled())
        return 0;
    while (*p == '/')
        p++;
    av_strlcpy(buf, p, sizeof(buf));
    cut = strchr(buf, '#');
    if (cut)
        *cut = 0;
    cut = strchr(buf, '?');
    if (cut) {
        *cut = 0;
        query = cut + 1;
    }
    slash = strchr(buf, '/');
    if (!slash || slash == buf)
        return 0;
    *slash = 0;
    av_strlcpy(app, buf, sizeof(app));
    av_strlcpy(stream, slash + 1, sizeof(stream));
    if (!app[0] || !stream[0] || strchr(stream, '/'))
        return 0;
    dot = strrchr(stream, '.');
    if (!dot || dot == stream || !dot[1])
        return 0;
    *dot++ = 0;
    if (!live_ext_fmt(dot, fmt, mime))
        return 0;
    pub = live_find_pub(app, stream);
    if (!pub)
        return 0;
    if (lowdelay)
        *lowdelay = live_query_flag(query, "lowdelay");
    if (pub_idx)
        *pub_idx = (int)(pub - pubs);
    return 1;
}

int ffmpeg_http_live_init(void)
{
    int i;

    if (!ffmpeg_http_live)
        return 0;
    if (atomic_load(&live_inited))
        return 0;

    if (nb_bind_specs == 0) {
        int ret = live_add_spec("live", "live", 0);
        if (ret < 0)
            return ret;
    }

    pubs = av_calloc(nb_bind_specs, sizeof(*pubs));
    if (!pubs)
        return AVERROR(ENOMEM);
    nb_pubs = nb_bind_specs;
    for (i = 0; i < nb_pubs; i++) {
        int idx = bind_specs[i].file_index;
        if (idx < 0)
            idx = i;
        if (nb_output_files > 0 && idx >= nb_output_files) {
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: /%s/%s file %d out of range, using 0\n",
                   bind_specs[i].app, bind_specs[i].stream, idx);
            idx = 0;
        }
        av_strlcpy(pubs[i].app, bind_specs[i].app, sizeof(pubs[i].app));
        av_strlcpy(pubs[i].stream, bind_specs[i].stream, sizeof(pubs[i].stream));
        pubs[i].file_index = idx;
        pubs[i].v_src = pubs[i].a_src = -1;
        pubs[i].vtb = (AVRational){ 0, 1 };
        pubs[i].atb = (AVRational){ 0, 1 };
        ffmpeg_http_log( AV_LOG_INFO,
               "http-live: GOP /%s/%s.flv|.ts|.mp4 <- output file %d\n",
               pubs[i].app, pubs[i].stream, pubs[i].file_index);
    }

    atomic_store(&live_running, 1);
    atomic_store(&live_inited, 1);
    return 0;
}

void ffmpeg_http_live_shutdown(void)
{
    if (!atomic_load(&live_inited))
        return;
    atomic_store(&live_running, 0);
    pthread_mutex_lock(&live_mutex);
    pthread_cond_broadcast(&live_cond);
    pthread_mutex_unlock(&live_mutex);
}

void ffmpeg_http_live_uninit(void)
{
    int i;

    if (atomic_load(&live_inited)) {
        ffmpeg_http_live_shutdown();
        pthread_mutex_lock(&live_mutex);
        for (i = 0; i < nb_pubs; i++) {
            live_list_free(&pubs[i].ready);
            live_list_free(&pubs[i].building);
            avcodec_parameters_free(&pubs[i].vpar);
            avcodec_parameters_free(&pubs[i].apar);
            pubs[i].subs = NULL;
        }
        pthread_mutex_unlock(&live_mutex);
        atomic_store(&live_inited, 0);
    }
    av_freep(&pubs);
    nb_pubs = 0;
    av_freep(&bind_specs);
    nb_bind_specs = 0;
}

void ffmpeg_http_live_push(const OutputFile *of, const OutputStream *ost,
                           const AVPacket *pkt)
{
    int is_video, is_audio, st, is_key, i, any = 0;

    if (!of || !ost || !pkt || !pkt->data || pkt->size <= 0)
        return;
    if (!atomic_load(&live_inited) || !atomic_load(&live_running))
        return;

    is_video = ost->type == AVMEDIA_TYPE_VIDEO;
    is_audio = ost->type == AVMEDIA_TYPE_AUDIO;
    if (!is_video && !is_audio)
        return;

    for (i = 0; i < nb_pubs; i++) {
        if (pubs[i].file_index == of->index) {
            any = 1;
            break;
        }
    }
    if (!any)
        return;

    pthread_mutex_lock(&live_mutex);
    if (!atomic_load(&live_running))
        goto unlock;

    for (i = 0; i < nb_pubs; i++) {
        HttpLivePub *pub = &pubs[i];
        AVPacket *owned;
        int ret;

        if (pub->file_index != of->index)
            continue;

        if (is_video) {
            if (pub->v_src < 0)
                pub->v_src = ost->index;
            if (ost->index != pub->v_src)
                continue;
            st = 0;
        } else {
            if (pub->a_src < 0)
                pub->a_src = ost->index;
            if (ost->index != pub->a_src)
                continue;
            st = 1;
        }

        ret = live_ensure_par(pub, ost, pkt, is_video);
        if (ret < 0)
            continue;
        live_refresh_par(pub, ost, pkt, is_video);

        is_key = is_video && (pkt->flags & AV_PKT_FLAG_KEY);
        if (is_key && pub->building.has_key)
            live_rotate_gop(pub);
        else if (is_key && !pub->building.has_key && pub->building.head)
            live_list_free(&pub->building);

        owned = av_packet_clone(pkt);
        if (!owned)
            continue;
        owned->stream_index = st;
        ret = live_list_append_owned(&pub->building, owned, st);
        if (ret < 0) {
            av_packet_free(&owned);
            continue;
        }
        if (pub->building.count > LIVE_GOP_MAX_PKTS && pub->building.has_key)
            live_rotate_gop(pub);
        live_fanout(pub, owned, st);
    }
    pthread_cond_broadcast(&live_cond);
unlock:
    pthread_mutex_unlock(&live_mutex);
}

int ffmpeg_http_live_serve(void *http_conn, const char *fmt,
                           const char *mime, int head_only,
                           int pub_idx, int lowdelay, int64_t accept_us)
{
    void *c = http_conn;
    const char *peer = ffmpeg_http_conn_peer(c);
    HttpLivePub *pub;
    LiveAvio avio_ctx = { .conn = c, .started = 0 };
    LiveSub sub;
    AVFormatContext *oc = NULL;
    AVIOContext *pb = NULL;
    unsigned char *iobuf = NULL;
    AVCodecParameters *vpar = NULL, *apar = NULL;
    AVPacket *key = NULL, *apkt = NULL;
    AVRational vtb = { 0, 1 }, atb = { 0, 1 };
    AVDictionary *opts = NULL;
    int64_t ts_off = AV_NOPTS_VALUE;
    int have_off = 0;
    int v_out = -1, a_out = -1;
    int ret, wait_ret, wait_ms, to_avcc = 0, to_asc = 0;
    int64_t t_wait;
    const char *why = "ok";

    memset(&sub, 0, sizeof(sub));
    if (!c || !fmt || !mime)
        return AVERROR(EINVAL);
    if (!ffmpeg_http_live_enabled() || pub_idx < 0 || pub_idx >= nb_pubs) {
        ffmpeg_http_conn_note(c, "live-off");
        return live_send_status(c, 503, "Live publisher off");
    }
    pub = &pubs[pub_idx];
    if (accept_us <= 0)
        accept_us = av_gettime_relative();
    ffmpeg_http_log( AV_LOG_INFO, "http-live: [%s] connect /%s/%s %s%s req=%dms\n",
           peer, pub->app, pub->stream, fmt, lowdelay ? " lowdelay" : "",
           (int)((av_gettime_relative() - accept_us) / 1000));

    t_wait = av_gettime_relative();
    pthread_mutex_lock(&live_mutex);
    wait_ret = live_wait_ready(pub, head_only, lowdelay);
    if (wait_ret >= 0 && pub->vpar) {
        const AVPacket *src;

        vpar = avcodec_parameters_alloc();
        if (vpar)
            avcodec_parameters_copy(vpar, pub->vpar);
        vtb = pub->vtb;
        if (pub->apar) {
            apar = avcodec_parameters_alloc();
            if (apar)
                avcodec_parameters_copy(apar, pub->apar);
            atb = pub->atb;
        }
        if (!head_only) {
            src = live_first_video_key(pub);
            if (live_mux_avcc(fmt) && src)
                key = av_packet_clone(src);
            src = live_first_audio(pub);
            if (src)
                apkt = av_packet_clone(src);
        }
    }
    pthread_mutex_unlock(&live_mutex);
    wait_ms = (int)((av_gettime_relative() - t_wait) / 1000);

    if (live_mux_avcc(fmt) && vpar) {
        ret = live_par_to_avcc(vpar, key);
        if (ret < 0)
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: [%s] AVCC extra from IDR failed: %s\n",
                   peer, av_err2str(ret));
        to_avcc = live_extra_is_avcc(vpar);
        if (!to_avcc)
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: [%s] no AVCC extra; FLV/MP4 may mis-parse Annex B\n",
                   peer);
    }
    av_packet_free(&key);
    if (apar) {
        ret = live_aac_asc_from_adts(apar, apkt);
        if (ret < 0)
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: [%s] AAC ASC from ADTS failed: %s\n",
                   peer, av_err2str(ret));
        ret = live_aac_ensure_asc(apar);
        if (ret < 0)
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: [%s] AAC ASC extra failed: %s\n",
                   peer, av_err2str(ret));
        else if (!strcmp(fmt, "mpegts") &&
                 apar->codec_id == AV_CODEC_ID_AAC && !apar->extradata_size)
            ffmpeg_http_log( AV_LOG_WARNING,
                   "http-live: [%s] AAC extra still empty; TS needs ADTS or ASC\n",
                   peer);
        to_asc = live_mux_avcc(fmt) && apar->codec_id == AV_CODEC_ID_AAC;
    }
    av_packet_free(&apkt);

    if (wait_ret < 0 || !vpar) {
        why = "gop-not-ready";
        ffmpeg_http_log( AV_LOG_WARNING,
               "http-live: [%s] GOP not ready wait=%dms cache=%s vpar=%d extra=%d\n",
               peer, wait_ms, live_cache_name(pub), pub->vpar ? 1 : 0,
               pub->vpar ? pub->vpar->extradata_size : 0);
        avcodec_parameters_free(&vpar);
        avcodec_parameters_free(&apar);
        ffmpeg_http_conn_note(c, why);
        ffmpeg_http_log( AV_LOG_INFO,
               "http-live: [%s] disconnect /%s/%s reason=%s dur=%dms bytes=%"PRId64"\n",
               peer, pub->app, pub->stream, why,
               (int)((av_gettime_relative() - accept_us) / 1000),
               ffmpeg_http_conn_bytes(c));
        return live_send_status(c, 503, "Live GOP not ready");
    }

    ffmpeg_http_log( AV_LOG_INFO,
           "http-live: [%s] gop ready wait=%dms cache=%s extra=%d (no wait extradata/next-IDR)\n",
           peer, wait_ms, live_cache_name(pub), vpar->extradata_size);
    if (live_send_ok_hdr(c, mime) < 0) {
        why = "write-200";
        ffmpeg_http_log( AV_LOG_ERROR, "http-live: [%s] write 200 failed\n", peer);
        avcodec_parameters_free(&vpar);
        avcodec_parameters_free(&apar);
        ffmpeg_http_conn_note(c, why);
        ffmpeg_http_log( AV_LOG_INFO,
               "http-live: [%s] disconnect /%s/%s reason=%s dur=%dms bytes=%"PRId64"\n",
               peer, pub->app, pub->stream, why,
               (int)((av_gettime_relative() - accept_us) / 1000),
               ffmpeg_http_conn_bytes(c));
        return AVERROR(EIO);
    }
    ffmpeg_http_log( AV_LOG_INFO, "http-live: [%s] http 200 %s\n", peer, mime);
    if (head_only) {
        why = "head";
        avcodec_parameters_free(&vpar);
        avcodec_parameters_free(&apar);
        ffmpeg_http_conn_note(c, why);
        ffmpeg_http_log( AV_LOG_INFO,
               "http-live: [%s] disconnect /%s/%s reason=%s dur=%dms bytes=%"PRId64"\n",
               peer, pub->app, pub->stream, why,
               (int)((av_gettime_relative() - accept_us) / 1000),
               ffmpeg_http_conn_bytes(c));
        return 0;
    }

    ret = avformat_alloc_output_context2(&oc, NULL, fmt, NULL);
    if (ret < 0 || !oc)
        goto fail;

    if (vpar) {
        AVStream *st = avformat_new_stream(oc, NULL);
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = avcodec_parameters_copy(st->codecpar, vpar);
        if (ret < 0)
            goto fail;
        st->time_base = vtb.num ? vtb : (AVRational){ 1, 90000 };
        v_out = st->index;
    }
    if (apar) {
        AVStream *st = avformat_new_stream(oc, NULL);
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = avcodec_parameters_copy(st->codecpar, apar);
        if (ret < 0)
            goto fail;
        st->time_base = atb.num ? atb : (AVRational){ 1, 48000 };
        a_out = st->index;
    }

    iobuf = av_malloc(LIVE_AVIO_BUF);
    if (!iobuf) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    pb = avio_alloc_context(iobuf, LIVE_AVIO_BUF, 1, &avio_ctx,
                            NULL, live_avio_write, NULL);
    if (!pb) {
        av_freep(&iobuf);
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    pb->seekable = 0;
    oc->pb = pb;
    oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    if (!strcmp(fmt, "mp4"))
        av_dict_set(&opts, "movflags",
                    "frag_keyframe+empty_moov+default_base_moof", 0);
    av_dict_set(&opts, "flush_packets", "1", 0);

    ret = avformat_write_header(oc, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        ffmpeg_http_log( AV_LOG_ERROR, "http-live: [%s] write_header %s failed: %s\n",
               peer, fmt, av_err2str(ret));
        goto fail;
    }
    avio_flush(oc->pb);
    ffmpeg_http_log( AV_LOG_INFO, "http-live: [%s] mux header ok\n", peer);

    pthread_mutex_lock(&live_mutex);
    ret = live_subscribe(pub, &sub, lowdelay);
    pthread_mutex_unlock(&live_mutex);
    if (ret < 0) {
        why = "subscribe-fail";
        ffmpeg_http_log( AV_LOG_WARNING, "http-live: [%s] subscribe failed: %s\n",
               peer, av_err2str(ret));
        av_write_trailer(oc);
        goto fail;
    }

    ffmpeg_http_log( AV_LOG_INFO,
           "http-live: [%s] streaming /%s/%s %s%s setup=%dms wait=%dms\n",
           peer, pub->app, pub->stream, fmt, lowdelay ? " lowdelay" : "",
           (int)((av_gettime_relative() - accept_us) / 1000), wait_ms);

    while (atomic_load(&live_running)) {
        LiveList batch;
        LiveNode *n;
        int overflow;

        memset(&batch, 0, sizeof(batch));
        pthread_mutex_lock(&live_mutex);
        while (atomic_load(&live_running) && !sub.q.head && !sub.overflow)
            pthread_cond_wait(&live_cond, &live_mutex);
        overflow = sub.overflow;
        batch = sub.q;
        memset(&sub.q, 0, sizeof(sub.q));
        pthread_mutex_unlock(&live_mutex);

        if (overflow) {
            live_list_free(&batch);
            why = "lagged";
            ffmpeg_http_log( AV_LOG_WARNING, "http-live: [%s] client lagged, drop\n",
                   peer);
            break;
        }
        for (n = batch.head; n; n = n->next) {
            AVRational tb = n->st == 0 ? vtb : atb;
            int out_st = n->st == 0 ? v_out : a_out;

            if (!have_off && n->pkt->dts != AV_NOPTS_VALUE) {
                ts_off = av_rescale_q(n->pkt->dts, tb, AV_TIME_BASE_Q);
                have_off = 1;
            }
            ret = live_write_pkt(oc, n->pkt, tb, out_st, &ts_off, have_off,
                                 n->st == 0 ? to_avcc : 0,
                                 n->st == 1 ? to_asc : 0);
            if (ret < 0)
                break;
        }
        live_list_free(&batch);
        if (ret < 0) {
            why = "write-fail";
            break;
        }
    }
    if (!strcmp(why, "ok") && !atomic_load(&live_running))
        why = "shutdown";

    av_write_trailer(oc);
    ret = 0;

fail:
    if (!strcmp(why, "ok") && ret < 0)
        why = "setup-fail";
    pthread_mutex_lock(&live_mutex);
    live_sub_detach(&sub);
    pthread_mutex_unlock(&live_mutex);

    ffmpeg_http_log( AV_LOG_INFO,
           "http-live: [%s] disconnect /%s/%s reason=%s dur=%dms bytes=%"PRId64"\n",
           peer, pub->app, pub->stream, why,
           (int)((av_gettime_relative() - accept_us) / 1000),
           ffmpeg_http_conn_bytes(c));
    ffmpeg_http_conn_note(c, why);

    if (oc) {
        if (oc->pb) {
            av_freep(&oc->pb->buffer);
            avio_context_free(&oc->pb);
        }
        avformat_free_context(oc);
    }
    avcodec_parameters_free(&vpar);
    avcodec_parameters_free(&apar);
    return ret;
}
