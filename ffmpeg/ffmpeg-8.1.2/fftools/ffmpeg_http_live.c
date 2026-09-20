/*
 * In-process HTTP live publisher: per-output GOP cache + per-client mux.
 * Serves /{app}/{stream}.flv|.ts|.mp4 from encoded packets, not growing files.
 */

#include "config.h"

#include "ffmpeg.h"
#include "ffmpeg_http_ev.h"
#include "ffmpeg_http_ring.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavutil/intreadwrite.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
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
#define LIVE_AVIO_BUF       (256 * 1024)
#define LIVE_FMT_FLV        0
#define LIVE_FMT_TS         1
#define LIVE_FMT_MP4        2
#define LIVE_FMT_NB         3

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
    AVRational vfr;
    int a_pad;
    int a_rate;
    LiveList ready;
    LiveList building;
    LiveSub *subs;
    atomic_int gop_ready;
    atomic_int viewers[HTTP_EV_MAX][LIVE_FMT_NB];
    atomic_int fmt_want[LIVE_FMT_NB];
    atomic_int mux_on[LIVE_FMT_NB];
    struct LiveFmtMux {
        AVFormatContext *oc;
        AVIOContext *pb;
        unsigned char *iobuf;
        uint8_t *acc;
        int acc_len, acc_sz, acc_pos;
        int64_t file_pos;
        HttpLiveBuf *header;
        HttpPtrRing ring;
        int v_out, a_out;
        int to_avcc, to_asc;
        int frag_key;
        int64_t ts_off;
        int have_off;
        int64_t last_dts[2];
        int64_t last_dur[2];
        int64_t v_priming;
        int64_t a_priming;
        AVBSFContext *vbsf;
        const char *name;
    } fmux[LIVE_FMT_NB];
    int64_t t_start;
    int64_t last_key_pts;
    int64_t gop_us;
    atomic_uint_fast64_t bytes_v;
    atomic_uint_fast64_t bytes_a;
    atomic_uint_fast64_t bytes_out;
    atomic_uint_fast64_t frames_v;
    int64_t rate_t;
    uint64_t prev_bytes_v, prev_bytes_a, prev_bytes_out, prev_frames_v;
    int64_t v_bps, a_bps, out_bps;
    double fps;
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
static pthread_t live_stat_th;
static atomic_int live_stat_on;
static int live_stat_started;

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
    if (is_video) {
        if (ost->st && ost->st->avg_frame_rate.num)
            pub->vfr = ost->st->avg_frame_rate;
        else if (ost->enc && ost->enc->enc_ctx && ost->enc->enc_ctx->framerate.num)
            pub->vfr = ost->enc->enc_ctx->framerate;
        else
            pub->vfr = (AVRational){ 25, 1 };
    } else {
        if (ost->enc && ost->enc->enc_ctx) {
            if (ost->enc->enc_ctx->initial_padding > 0)
                pub->a_pad = ost->enc->enc_ctx->initial_padding;
            if (ost->enc->enc_ctx->sample_rate > 0)
                pub->a_rate = ost->enc->enc_ctx->sample_rate;
        }
        if (!pub->a_rate && ost->st && ost->st->codecpar)
            pub->a_rate = ost->st->codecpar->sample_rate;
        if (!pub->a_pad && ost->st && ost->st->codecpar)
            pub->a_pad = ost->st->codecpar->initial_padding;
    }
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
    int nal;

    if (!p || n < 4)
        return 0;
    if (AV_RB32(p) == 0x00000001)
        return 1;
    /* 3-byte start code. Do not treat AVCC length 0x000001xx (256–511)
     * as Annex B: that false positive re-parses length-prefixed NALs
     * and yields Invalid NAL 0 / log2_max_frame_num=31. */
    if (AV_RB24(p) != 0x000001)
        return 0;
    nal = p[3] & 0x1f;
    if (nal >= 1 && nal <= 23)
        return 1;
    /* HEVC: nal_unit_type in bits 1–6 (VPS=32 … SEI=39/40). */
    nal = (p[3] >> 1) & 0x3f;
    return nal >= 1 && nal <= 40;
}

/* AVCC: 4-byte NAL length, not a start code. copy 出来的包通常是这种。 */
static int live_is_avcc_nal(const uint8_t *p, int n)
{
    uint32_t sz;

    if (!p || n < 5 || live_is_annexb(p, n))
        return 0;
    sz = AV_RB32(p);
    return sz > 0 && sz <= (uint32_t)(n - 4);
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
static int live_avcc_from_annexb(AVCodecParameters *par, const uint8_t *src, int len)
{
    AVIOContext *pb = NULL;
    uint8_t *buf = NULL;
    int size, ret;

    if (!par || !src || len <= 0)
        return 0;
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
        return 0;
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

static int live_par_to_avcc(AVCodecParameters *par, const AVPacket *key)
{
    if (!par)
        return 0;
    if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC)
        return 0;
    if (live_extra_is_avcc(par))
        return 1;
    if (live_is_annexb(par->extradata, par->extradata_size) &&
        live_avcc_from_annexb(par, par->extradata, par->extradata_size))
        return 1;
    if (key && live_is_annexb(key->data, key->size) &&
        live_avcc_from_annexb(par, key->data, key->size))
        return 1;
    return 0;
}

static const AVPacket *live_first_video_key(const HttpLivePub *pub);

static int live_fill_avcc(AVCodecParameters *vp, const HttpLivePub *pub)
{
    const LiveNode *n;
    int lists, i;

    if (live_par_to_avcc(vp, live_first_video_key(pub)))
        return 1;
    if (!pub)
        return 0;
    for (i = 0, lists = 2; i < lists; i++) {
        for (n = (i ? pub->ready.head : pub->building.head); n; n = n->next) {
            if (n->st != 0 || !n->pkt || !(n->pkt->flags & AV_PKT_FLAG_KEY))
                continue;
            if (live_par_to_avcc(vp, n->pkt))
                return 1;
        }
    }
    return 0;
}

static int live_mux_open_vbsf(struct LiveFmtMux *m)
{
    const char *name;
    const AVBitStreamFilter *f;
    AVStream *st;
    int ret;

    if (!m || !m->oc || m->v_out < 0)
        return 0;
    st = m->oc->streams[m->v_out];
    if (!st || !st->codecpar)
        return 0;
    if (st->codecpar->codec_id == AV_CODEC_ID_H264)
        name = "h264_mp4toannexb";
    else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC)
        name = "hevc_mp4toannexb";
    else
        return 0;
    f = av_bsf_get_by_name(name);
    if (!f)
        return 0;
    ret = av_bsf_alloc(f, &m->vbsf);
    if (ret < 0)
        return ret;
    ret = avcodec_parameters_copy(m->vbsf->par_in, st->codecpar);
    if (ret < 0)
        return ret;
    m->vbsf->time_base_in = st->time_base;
    return av_bsf_init(m->vbsf);
}

static int live_pkt_to_avcc(AVPacket *pkt, enum AVCodecID id)
{
    uint8_t *buf = NULL;
    int size, ret;

    if (!pkt || live_is_avcc_nal(pkt->data, pkt->size))
        return 0;
    if (!live_is_annexb(pkt->data, pkt->size))
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

static int live_write_pkt(struct LiveFmtMux *m, AVPacket *pkt,
                          AVRational src_tb, int out_st, int to_asc);

static int live_fmt_idx(const char *fmt)
{
    if (!fmt)
        return -1;
    if (!strcmp(fmt, "flv"))
        return LIVE_FMT_FLV;
    if (!strcmp(fmt, "mpegts"))
        return LIVE_FMT_TS;
    if (!strcmp(fmt, "mp4"))
        return LIVE_FMT_MP4;
    return -1;
}

static void live_notify_workers(HttpLivePub *pub, int type, int fmt_idx)
{
    HttpEvMsg msg;
    int i, n;

    if (!pub)
        return;
    msg.type = type;
    msg.pub_idx = (int)(pub - pubs);
    msg.fmt_idx = fmt_idx;
    n = http_ev_nb_workers();
    for (i = 0; i < n && i < HTTP_EV_MAX; i++) {
        if (atomic_load(&pub->viewers[i][fmt_idx > 0 ? fmt_idx : 0]) > 0 ||
            type == HTTP_EV_MSG_GOP_READY) {
            int f, hit = 0;
            if (type == HTTP_EV_MSG_GOP_READY) {
                for (f = 0; f < LIVE_FMT_NB; f++)
                    if (atomic_load(&pub->viewers[i][f]) > 0)
                        hit = 1;
                if (!hit)
                    continue;
            } else if (fmt_idx >= 0 &&
                       atomic_load(&pub->viewers[i][fmt_idx]) <= 0) {
                continue;
            }
            http_ev_post(i, msg);
        }
    }
}

static int live_pub_clients(const HttpLivePub *pub, int *per_fmt)
{
    int w, f, n = 0;

    if (per_fmt)
        memset(per_fmt, 0, LIVE_FMT_NB * sizeof(*per_fmt));
    if (!pub)
        return 0;
    for (f = 0; f < LIVE_FMT_NB; f++) {
        int c = 0;
        for (w = 0; w < HTTP_EV_MAX; w++)
            c += atomic_load(&pub->viewers[w][f]);
        if (c < 0)
            c = 0;
        if (per_fmt)
            per_fmt[f] = c;
        n += c;
    }
    return n;
}

static const char *live_codec_log(enum AVCodecID id)
{
    const char *n = avcodec_get_name(id);

    if (!n || !n[0] || !strcmp(n, "none"))
        return "-";
    if (!strcmp(n, "hevc"))
        return "h265";
    return n;
}

static void live_codec_json(enum AVCodecID id, char *out, int sz)
{
    const char *n = avcodec_get_name(id);

    if (!n || !n[0] || !strcmp(n, "none")) {
        av_strlcpy(out, "", sz);
        return;
    }
    if (!strcmp(n, "h264"))
        av_strlcpy(out, "H264", sz);
    else if (!strcmp(n, "hevc"))
        av_strlcpy(out, "H265", sz);
    else if (!strcmp(n, "aac"))
        av_strlcpy(out, "AAC", sz);
    else if (!strcmp(n, "mp3"))
        av_strlcpy(out, "MP3", sz);
    else if (!strcmp(n, "opus"))
        av_strlcpy(out, "OPUS", sz);
    else
        av_strlcpy(out, n, sz);
}

static void live_refresh_rates_l(HttpLivePub *pub)
{
    int64_t now = av_gettime_relative();
    int64_t dt;
    uint64_t bv, ba, bo, fv;

    if (!pub)
        return;
    dt = pub->rate_t ? now - pub->rate_t
         : (pub->t_start ? now - pub->t_start : 0);
    if (dt < 400000 && pub->rate_t)
        return;
    if (dt <= 0)
        dt = 1;
    bv = atomic_load(&pub->bytes_v);
    ba = atomic_load(&pub->bytes_a);
    bo = atomic_load(&pub->bytes_out);
    fv = atomic_load(&pub->frames_v);
    if (pub->rate_t) {
        pub->v_bps  = (int64_t)((bv - pub->prev_bytes_v) * 8000000.0 / dt);
        pub->a_bps  = (int64_t)((ba - pub->prev_bytes_a) * 8000000.0 / dt);
        pub->out_bps = (int64_t)((bo - pub->prev_bytes_out) * 8000000.0 / dt);
        pub->fps    = (fv - pub->prev_frames_v) * 1000000.0 / dt;
    } else {
        pub->v_bps  = (int64_t)(bv * 8000000.0 / dt);
        pub->a_bps  = (int64_t)(ba * 8000000.0 / dt);
        pub->out_bps = (int64_t)(bo * 8000000.0 / dt);
        pub->fps    = fv * 1000000.0 / dt;
    }
    pub->prev_bytes_v = bv;
    pub->prev_bytes_a = ba;
    pub->prev_bytes_out = bo;
    pub->prev_frames_v = fv;
    pub->rate_t = now;
}

static void live_note_media(HttpLivePub *pub, const AVPacket *pkt, int is_video, int is_key)
{
    if (!pub || !pkt)
        return;
    if (!pub->t_start)
        pub->t_start = av_gettime_relative();
    if (is_video) {
        atomic_fetch_add(&pub->bytes_v, (uint64_t)pkt->size);
        atomic_fetch_add(&pub->frames_v, 1);
        if (is_key) {
            if (pub->last_key_pts != AV_NOPTS_VALUE &&
                pkt->pts != AV_NOPTS_VALUE &&
                pub->vtb.num && pub->vtb.den) {
                int64_t us = av_rescale_q(pkt->pts - pub->last_key_pts,
                                          pub->vtb, AV_TIME_BASE_Q);
                if (us > 0 && us < 60LL * AV_TIME_BASE)
                    pub->gop_us = us;
            }
            if (pkt->pts != AV_NOPTS_VALUE)
                pub->last_key_pts = pkt->pts;
        }
    } else {
        atomic_fetch_add(&pub->bytes_a, (uint64_t)pkt->size);
    }
}

static void live_log_stats(void)
{
    AVBPrint bp;
    int i;

    if (!atomic_load(&live_inited))
        return;
    av_bprint_init(&bp, 512, AV_BPRINT_SIZE_UNLIMITED);
    pthread_mutex_lock(&live_mutex);
    av_bprintf(&bp, "http-live: streams %d", nb_pubs);
    for (i = 0; i < nb_pubs; i++) {
        HttpLivePub *pub = &pubs[i];
        int clients = live_pub_clients(pub, NULL);
        const char *vc = pub->vpar ? live_codec_log(pub->vpar->codec_id) : "-";
        const char *ac = pub->apar ? live_codec_log(pub->apar->codec_id) : "-";

        live_refresh_rates_l(pub);
        av_bprintf(&bp, ", stream %s/%s, %s, %s, gop %.1fs, fps %.0f, clients %d",
                   pub->app, pub->stream, vc, ac,
                   pub->gop_us > 0 ? pub->gop_us / 1000000.0 : 0.0,
                   pub->fps, clients);
    }
    pthread_mutex_unlock(&live_mutex);
    if (av_bprint_is_complete(&bp) && bp.str && bp.str[0]) {
        char ts[40];
        int64_t us = av_gettime();
        time_t sec = (time_t)(us / 1000000);
        int ms = (int)((us / 1000) % 1000);
        struct tm tmbuf, *tm;
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
        /* 先 \n 结束 print_report 的 \r 进度行，整段只打一条 */
        av_log(NULL, AV_LOG_INFO, "\n([%s] %s\n", ts, bp.str);
    }
    av_bprint_finalize(&bp, NULL);
}

static void *live_stat_thread(void *arg)
{
    int i;

    (void)arg;
    while (atomic_load(&live_stat_on) && atomic_load(&live_running)) {
        for (i = 0; i < 60; i++) {
            if (!atomic_load(&live_stat_on) || !atomic_load(&live_running))
                return NULL;
            av_usleep(1000000);
        }
        if (atomic_load(&live_stat_on) && atomic_load(&live_running))
            live_log_stats();
    }
    return NULL;
}

static void live_notify_kick(int pub_idx)
{
    HttpEvMsg msg = { .type = HTTP_EV_MSG_KICK, .pub_idx = pub_idx, .fmt_idx = -1 };
    int i, n = http_ev_nb_workers();

    for (i = 0; i < n && i < HTTP_EV_MAX; i++)
        http_ev_post(i, msg);
}

static void json_quote(AVBPrint *bp, const char *s)
{
    av_bprint_chars(bp, '"', 1);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            av_bprint_chars(bp, '\\', 1);
            av_bprint_chars(bp, (char)c, 1);
        } else if (c < 0x20) {
            av_bprintf(bp, "\\u%04x", c);
        } else {
            av_bprint_chars(bp, (char)c, 1);
        }
    }
    av_bprint_chars(bp, '"', 1);
}

static void live_fmt_dur(char *out, int sz, int64_t sec)
{
    int h, m, s;

    if (sec < 0)
        sec = 0;
    h = (int)(sec / 3600);
    m = (int)((sec % 3600) / 60);
    s = (int)(sec % 60);
    if (h > 0)
        snprintf(out, sz, "%dh %dm %ds", h, m, s);
    else if (m > 0)
        snprintf(out, sz, "%dm %ds", m, s);
    else
        snprintf(out, sz, "%ds", s);
}

static struct LiveFmtMux *live_mux_from_key(void *opaque)
{
    intptr_t key = (intptr_t)opaque;
    int pidx = (int)(key >> 8), fi = (int)(key & 0xff);

    if (pidx < 0 || pidx >= nb_pubs || fi < 0 || fi >= LIVE_FMT_NB)
        return NULL;
    return &pubs[pidx].fmux[fi];
}

/* Write/seek into the unflushed acc window so FLV tag / moof size patches
 * still work after the 256KB AVIO buffer has spilled. */
static int live_mux_avio(void *opaque, const uint8_t *buf, int buf_size)
{
    struct LiveFmtMux *m = live_mux_from_key(opaque);
    uint8_t *nbuf;
    int need;

    if (!m || buf_size < 0)
        return AVERROR(EINVAL);
    if (!buf_size)
        return 0;
    need = m->acc_pos + buf_size;
    if (need > m->acc_sz) {
        int nsz = FFMAX(need, m->acc_sz * 2 + 4096);
        nbuf = av_realloc(m->acc, nsz);
        if (!nbuf)
            return AVERROR(ENOMEM);
        m->acc = nbuf;
        m->acc_sz = nsz;
    }
    memcpy(m->acc + m->acc_pos, buf, buf_size);
    m->acc_pos += buf_size;
    if (m->acc_pos > m->acc_len)
        m->acc_len = m->acc_pos;
    return buf_size;
}

static int64_t live_mux_avio_seek(void *opaque, int64_t offset, int whence)
{
    struct LiveFmtMux *m = live_mux_from_key(opaque);
    int64_t target, local;

    if (!m)
        return AVERROR(EINVAL);
    if (whence == AVSEEK_SIZE)
        return m->file_pos + m->acc_len;
    if (whence == SEEK_CUR)
        offset += m->file_pos + m->acc_pos;
    else if (whence == SEEK_END)
        offset += m->file_pos + m->acc_len;
    else if (whence != SEEK_SET)
        return AVERROR(EINVAL);
    target = offset;
    local = target - m->file_pos;
    if (local < 0 || local > m->acc_len)
        return AVERROR(EINVAL);
    m->acc_pos = (int)local;
    return target;
}

static void live_mux_flush_acc(HttpLivePub *pub, int fi, int is_key)
{
    struct LiveFmtMux *m = &pub->fmux[fi];
    HttpLiveBuf *b;
    unsigned pos;

    if (!m->acc_len)
        return;
    b = http_livebuf_alloc(m->acc, m->acc_len, is_key);
    m->file_pos += m->acc_len;
    m->acc_len = 0;
    m->acc_pos = 0;
    if (!b)
        return;
    pos = http_ring_wpos(&m->ring);
    http_ring_push(&m->ring, b);
    if (is_key)
        http_ring_mark_gop(&m->ring, pos);
    http_livebuf_unref(&b);
    live_notify_workers(pub, HTTP_EV_MSG_FRAME, fi);
}

/* Pull ftyp(+moov) off acc before the first moof so every client gets init
 * even with delay_moov (moov is written on the first fragment, not write_header). */
static void live_mux_take_mp4_header(struct LiveFmtMux *m)
{
    int pos = 0, saw_moov = 0, end;

    if (!m || m->header || m->acc_len < 8)
        return;
    while (pos + 8 <= m->acc_len) {
        uint32_t sz = AV_RB32(m->acc + pos);
        if (sz < 8 || pos + (int)sz > m->acc_len)
            return;
        if (!memcmp(m->acc + pos + 4, "moof", 4) ||
            !memcmp(m->acc + pos + 4, "mdat", 4)) {
            if (!saw_moov)
                return;
            break;
        }
        if (!memcmp(m->acc + pos + 4, "moov", 4))
            saw_moov = 1;
        pos += sz;
    }
    end = pos;
    if (!saw_moov || end <= 0)
        return;
    m->header = http_livebuf_alloc(m->acc, end, 1);
    if (end < m->acc_len)
        memmove(m->acc, m->acc + end, m->acc_len - end);
    m->acc_len -= end;
    m->acc_pos = m->acc_len;
    m->file_pos += end;
}

/* fMP4 frag_every_frame writes the *previous* sample's moof+mdat during
 * av_write_frame(current). Mark the ring slot with that previous sample. */
static void live_mux_commit(HttpLivePub *pub, int fi, int is_key)
{
    struct LiveFmtMux *m = &pub->fmux[fi];
    int slot_key = is_key;

    if (fi == LIVE_FMT_MP4)
        live_mux_take_mp4_header(m);
    if (fi == LIVE_FMT_MP4) {
        if (!m->acc_len) {
            /* Sample still in movenc; keep the delayed key across audio
             * packets that produce no bytes. */
            m->frag_key = m->frag_key || is_key;
            return;
        }
        slot_key = m->frag_key;
        m->frag_key = is_key;
    }
    live_mux_flush_acc(pub, fi, slot_key);
}

static void live_mux_flush_mp4(HttpLivePub *pub, int fi)
{
    struct LiveFmtMux *m = &pub->fmux[fi];

    if (fi != LIVE_FMT_MP4 || !m->oc)
        return;
    av_write_frame(m->oc, NULL);
    if (m->oc->pb)
        avio_flush(m->oc->pb);
    live_mux_commit(pub, fi, 0);
}

/* fMP4 keeps the current sample until the next write. Flush after an IDR so
 * clients start on this GOP instead of replaying the previous one (fast-forward).
 * Wait until audio has been muxed so delay_moov does not emit a video-only moov. */
static void live_mux_emit(HttpLivePub *pub, int fi, int is_key)
{
    struct LiveFmtMux *m = &pub->fmux[fi];

    live_mux_commit(pub, fi, is_key);
    if (fi != LIVE_FMT_MP4 || !is_key || !m->oc)
        return;
    if (m->a_out >= 0 && m->last_dts[m->a_out] == AV_NOPTS_VALUE)
        return;
    live_mux_flush_mp4(pub, fi);
}

static void live_mux_free(struct LiveFmtMux *m)
{
    if (!m)
        return;
    http_ring_clear(&m->ring);
    http_livebuf_unref(&m->header);
    av_bsf_free(&m->vbsf);
    av_freep(&m->acc);
    m->acc_len = m->acc_sz = m->acc_pos = 0;
    m->file_pos = 0;
    m->frag_key = 0;
    m->to_avcc = m->to_asc = 0;
    m->have_off = 0;
    m->ts_off = AV_NOPTS_VALUE;
    m->last_dts[0] = m->last_dts[1] = AV_NOPTS_VALUE;
    m->last_dur[0] = m->last_dur[1] = 0;
    m->v_priming = 0;
    m->a_priming = 0;
    m->v_out = m->a_out = -1;
    if (m->oc) {
        if (m->oc->pb) {
            av_freep(&m->oc->pb->buffer);
            avio_context_free(&m->oc->pb);
        }
        avformat_free_context(m->oc);
        m->oc = NULL;
        m->pb = NULL;
        m->iobuf = NULL;
    }
}

static int live_mux_enable(HttpLivePub *pub, int fi)
{
    struct LiveFmtMux *m;
    const char *fmt;
    AVDictionary *opts = NULL;
    intptr_t key;
    int ret;
    LiveNode *n;

    if (atomic_load(&pub->mux_on[fi]))
        return 0;
    m = &pub->fmux[fi];
    live_mux_free(m);
    fmt = fi == LIVE_FMT_FLV ? "flv" : fi == LIVE_FMT_TS ? "mpegts" : "mp4";
    m->name = fmt;
    m->v_out = m->a_out = -1;
    m->ts_off = AV_NOPTS_VALUE;
    m->have_off = 0;
    m->frag_key = 0;
    m->file_pos = 0;
    m->last_dts[0] = m->last_dts[1] = AV_NOPTS_VALUE;
    m->last_dur[0] = m->last_dur[1] = 0;
    m->v_priming = 0;
    m->a_priming = 0;
    http_ring_init(&m->ring);
    ret = avformat_alloc_output_context2(&m->oc, NULL, fmt, NULL);
    if (ret < 0 || !m->oc) {
        live_mux_free(m);
        return ret < 0 ? ret : AVERROR(ENOMEM);
    }
    if (pub->vpar) {
        AVStream *st = avformat_new_stream(m->oc, NULL);
        AVCodecParameters *vp = avcodec_parameters_alloc();
        if (!st || !vp) {
            avcodec_parameters_free(&vp);
            live_mux_free(m);
            return AVERROR(ENOMEM);
        }
        avcodec_parameters_copy(vp, pub->vpar);
        if (live_mux_avcc(fmt)) {
            if (!live_fill_avcc(vp, pub)) {
                avcodec_parameters_free(&vp);
                live_mux_free(m);
                return AVERROR(EAGAIN);
            }
            m->to_avcc = 1;
        }
        avcodec_parameters_copy(st->codecpar, vp);
        st->time_base = pub->vtb.num ? pub->vtb : (AVRational){ 1, 90000 };
        if (pub->vfr.num && pub->vfr.den) {
            st->avg_frame_rate = pub->vfr;
            st->r_frame_rate = pub->vfr;
        }
        m->v_out = st->index;
        if (fi == LIVE_FMT_MP4 && pub->a_pad > 0 && pub->a_rate > 0)
            m->v_priming = av_rescale_q(pub->a_pad,
                                        (AVRational){ 1, pub->a_rate },
                                        st->time_base);
        avcodec_parameters_free(&vp);
    }
    if (pub->apar) {
        AVStream *st = avformat_new_stream(m->oc, NULL);
        AVCodecParameters *ap = avcodec_parameters_alloc();
        const AVPacket *apkt;
        if (!st || !ap) {
            avcodec_parameters_free(&ap);
            live_mux_free(m);
            return AVERROR(ENOMEM);
        }
        avcodec_parameters_copy(ap, pub->apar);
        apkt = live_first_audio(pub);
        live_aac_asc_from_adts(ap, apkt);
        live_aac_ensure_asc(ap);
        m->to_asc = live_mux_avcc(fmt) && ap->codec_id == AV_CODEC_ID_AAC;
        avcodec_parameters_copy(st->codecpar, ap);
        st->time_base = pub->atb.num ? pub->atb : (AVRational){ 1, 48000 };
        m->a_out = st->index;
        avcodec_parameters_free(&ap);
    }
    if (fi == LIVE_FMT_TS) {
        ret = live_mux_open_vbsf(m);
        if (ret < 0) {
            live_mux_free(m);
            return ret;
        }
    }
    m->iobuf = av_malloc(LIVE_AVIO_BUF);
    if (!m->iobuf) {
        live_mux_free(m);
        return AVERROR(ENOMEM);
    }
    key = ((intptr_t)(pub - pubs) << 8) | fi;
    m->pb = avio_alloc_context(m->iobuf, LIVE_AVIO_BUF, 1, (void *)key,
                               NULL, live_mux_avio, live_mux_avio_seek);
    if (!m->pb) {
        av_freep(&m->iobuf);
        live_mux_free(m);
        return AVERROR(ENOMEM);
    }
    /* Keep seekable=0 so muxers do not rewrite already-pushed headers.
     * live_mux_avio_seek still patches sizes inside the current acc window. */
    m->pb->seekable = 0;
    m->oc->pb = m->pb;
    if (fi != LIVE_FMT_MP4)
        m->oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    m->oc->max_delay = 0;
    if (fi == LIVE_FMT_FLV)
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
    if (fi == LIVE_FMT_TS) {
        /* copyts: keep the same A/V clock as FLV. Default mpegts delay
         * plus starting mid-GOP makes video look ~1s behind audio. */
        av_dict_set(&opts, "mpegts_flags", "resend_headers+pat_pmt_at_frames", 0);
        av_dict_set(&opts, "mpegts_copyts", "1", 0);
    }
    if (fi == LIVE_FMT_MP4)
        /* delay_moov: first sample dts need not be 0.
         * frag_every_frame: GOP-sized moof stalls players every -g.
         * separate_moof + negative_cts_offsets: HEVC+AAC+B-frames in one
         * file; cmaf (single-track chunks) leaves video on the first
         * sample while audio keeps moving. */
        av_dict_set(&opts, "movflags",
                    "frag_every_frame+delay_moov+empty_moov+default_base_moof+"
                    "separate_moof+negative_cts_offsets", 0);
    av_dict_set(&opts, "flush_packets", "1", 0);
    ret = avformat_write_header(m->oc, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        live_mux_free(m);
        return ret;
    }
    /* mpegts forces 90 kHz after write_header. ADTS players skip AAC
     * priming (FLV does too; fMP4 does not), so audio leads video by
     * ~40–80ms. Delay audio PTS only on TS. */
    if (fi == LIVE_FMT_TS && m->a_out >= 0 && pub->a_pad > 0 && pub->a_rate > 0) {
        AVStream *ast = m->oc->streams[m->a_out];
        m->a_priming = av_rescale_q(pub->a_pad,
                                    (AVRational){ 1, pub->a_rate },
                                    ast->time_base);
    }
    avio_flush(m->oc->pb);
    if (fi == LIVE_FMT_MP4)
        live_mux_take_mp4_header(m);
    else if (m->acc_len) {
        m->header = http_livebuf_alloc(m->acc, m->acc_len, 1);
        m->file_pos += m->acc_len;
        m->acc_len = 0;
        m->acc_pos = 0;
    }
    n = pub->building.has_key ? pub->building.head :
        (pub->ready.head ? pub->ready.head : NULL);
    for (; n; n = n->next) {
        AVRational tb = n->st == 0 ? pub->vtb : pub->atb;
        int out = n->st == 0 ? m->v_out : m->a_out;
        int pkt_key = n->st == 0 && n->pkt && (n->pkt->flags & AV_PKT_FLAG_KEY);
        if (!m->have_off && n->pkt && n->pkt->dts != AV_NOPTS_VALUE) {
            m->ts_off = av_rescale_q(n->pkt->dts, tb, AV_TIME_BASE_Q);
            m->have_off = 1;
        }
        if (live_write_pkt(m, n->pkt, tb, out, n->st == 1 ? m->to_asc : 0) < 0)
            ffmpeg_http_log(AV_LOG_WARNING,
                            "http-live: mux %s replay st=%d failed\n",
                            m->name, n->st);
        live_mux_emit(pub, fi, pkt_key);
    }
    if (fi == LIVE_FMT_MP4) {
        live_mux_flush_mp4(pub, fi);
        live_mux_take_mp4_header(m);
    }
    atomic_store(&pub->mux_on[fi], 1);
    live_notify_workers(pub, HTTP_EV_MSG_FRAME, fi);
    return 0;
}

static void live_mux_packet(HttpLivePub *pub, const AVPacket *pkt, int st)
{
    int fi;
    AVRational tb = st == 0 ? pub->vtb : pub->atb;
    int is_key = st == 0 && pkt && (pkt->flags & AV_PKT_FLAG_KEY);

    for (fi = 0; fi < LIVE_FMT_NB; fi++) {
        struct LiveFmtMux *m = &pub->fmux[fi];
        int out, was_on;
        was_on = atomic_load(&pub->mux_on[fi]);
        if (!was_on && atomic_load(&pub->fmt_want[fi]) > 0)
            live_mux_enable(pub, fi);
        if (!atomic_load(&pub->mux_on[fi]) || !m->oc)
            continue;
        /* enable() already muxed the current packet as part of GOP replay. */
        if (!was_on)
            continue;
        out = st == 0 ? m->v_out : m->a_out;
        if (!m->have_off && pkt && pkt->dts != AV_NOPTS_VALUE) {
            m->ts_off = av_rescale_q(pkt->dts, tb, AV_TIME_BASE_Q);
            m->have_off = 1;
        }
        if (live_write_pkt(m, (AVPacket *)pkt, tb, out, st == 1 ? m->to_asc : 0) < 0)
            ffmpeg_http_log(AV_LOG_WARNING,
                            "http-live: mux %s st=%d write failed\n",
                            m->name, st);
        live_mux_emit(pub, fi, is_key);
    }
}

static int64_t live_default_dur(const AVStream *st)
{
    const AVCodecParameters *par;
    AVRational fr;

    if (!st || !st->codecpar)
        return 0;
    par = st->codecpar;
    if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->sample_rate > 0) {
        int nb = par->frame_size > 0 ? par->frame_size : 1024;
        return av_rescale_q(nb, (AVRational){ 1, par->sample_rate }, st->time_base);
    }
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        if (fr.num && fr.den)
            return av_rescale_q(1, av_inv_q(fr), st->time_base);
    }
    return 0;
}

/* copy 音频常和上一包 dts+duration 重叠（AAC priming / 时间基取整），movenc
 * 会报 Packet duration 为负并把 pts 清掉。缺 pts 时用 dts。重叠则后移，保持 cts。 */
static void live_sanitize_ts(struct LiveFmtMux *m, AVPacket *out, AVStream *st,
                             int out_st)
{
    int64_t def, min_dts, shift;

    if (out->dts == AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE)
        out->dts = out->pts;
    if (out->pts == AV_NOPTS_VALUE && out->dts != AV_NOPTS_VALUE)
        out->pts = out->dts;

    def = live_default_dur(st);
    if (out->duration < 0)
        out->duration = 0;
    if (out->duration <= 0 && def > 0)
        out->duration = def;

    if (out_st < 0 || out_st > 1)
        return;

    if (out->dts == AV_NOPTS_VALUE) {
        if (m->last_dts[out_st] != AV_NOPTS_VALUE)
            out->dts = m->last_dts[out_st] +
                       (m->last_dur[out_st] > 0 ? m->last_dur[out_st] : 1);
        else
            out->dts = 0;
        out->pts = out->dts;
    }

    if (out->dts < 0) {
        shift = -out->dts;
        out->dts = 0;
        if (out->pts != AV_NOPTS_VALUE)
            out->pts += shift;
    }

    if (m->last_dts[out_st] != AV_NOPTS_VALUE) {
        min_dts = m->last_dts[out_st];
        if (m->last_dur[out_st] > 0)
            min_dts += m->last_dur[out_st];
        else
            min_dts += 1;
        if (out->dts < min_dts) {
            shift = min_dts - out->dts;
            out->dts = min_dts;
            if (out->pts != AV_NOPTS_VALUE)
                out->pts += shift;
            else
                out->pts = out->dts;
        }
    }
    if (out->pts == AV_NOPTS_VALUE)
        out->pts = out->dts;
    m->last_dts[out_st] = out->dts;
    m->last_dur[out_st] = out->duration > 0 ? out->duration : def;
}

static int live_write_pkt(struct LiveFmtMux *m, AVPacket *pkt,
                          AVRational src_tb, int out_st, int to_asc)
{
    AVFormatContext *oc;
    AVStream *st;
    AVPacket *out;
    int ret;

    if (!m || !m->oc)
        return 0;
    oc = m->oc;
    if (out_st < 0 || out_st >= oc->nb_streams)
        return 0;
    st = oc->streams[out_st];
    out = av_packet_clone(pkt);
    if (!out)
        return AVERROR(ENOMEM);
    out->stream_index = out_st;
    if (m->have_off && m->ts_off != AV_NOPTS_VALUE) {
        int64_t off = av_rescale_q(m->ts_off, AV_TIME_BASE_Q, src_tb);
        if (out->pts != AV_NOPTS_VALUE)
            out->pts -= off;
        if (out->dts != AV_NOPTS_VALUE)
            out->dts -= off;
    }
    av_packet_rescale_ts(out, src_tb, st->time_base);
    if (out_st == m->v_out && m->v_priming > 0) {
        /* AAC encoder delay is not in fMP4; FLV players compensate, MP4 does
         * not — video looks 40–80ms early. Shift video to match. */
        if (out->pts != AV_NOPTS_VALUE)
            out->pts += m->v_priming;
        if (out->dts != AV_NOPTS_VALUE)
            out->dts += m->v_priming;
    }
    live_sanitize_ts(m, out, st, out_st);
    /* After sanitize: TS/ADTS players skip AAC priming so audio leads
     * video by ~a_pad (40–80ms). Delay audio PTS; FLV/MP4 unchanged. */
    if (out_st == m->a_out && m->a_priming > 0) {
        if (out->pts != AV_NOPTS_VALUE)
            out->pts += m->a_priming;
        if (out->dts != AV_NOPTS_VALUE)
            out->dts += m->a_priming;
    }
    if (st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        out->side_data && out->side_data_elems)
        av_packet_side_data_remove(out->side_data, &out->side_data_elems,
                                   AV_PKT_DATA_NEW_EXTRADATA);
    if (m->vbsf && out_st == m->v_out) {
        AVPacket *flt = av_packet_alloc();
        if (!flt) {
            av_packet_free(&out);
            return AVERROR(ENOMEM);
        }
        ret = av_bsf_send_packet(m->vbsf, out);
        av_packet_free(&out);
        if (ret < 0) {
            av_packet_free(&flt);
            return ret;
        }
        ret = av_bsf_receive_packet(m->vbsf, flt);
        if (ret < 0) {
            av_packet_free(&flt);
            return ret == AVERROR(EAGAIN) ? 0 : ret;
        }
        out = flt;
        out->stream_index = out_st;
    } else if (m->to_avcc && st->codecpar &&
        st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = live_pkt_to_avcc(out, st->codecpar->codec_id);
        if (ret < 0) {
            av_packet_free(&out);
            return ret;
        }
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
        pubs[i].vfr = (AVRational){ 0, 1 };
        pubs[i].a_pad = 0;
        pubs[i].a_rate = 0;
        pubs[i].t_start = 0;
        pubs[i].last_key_pts = AV_NOPTS_VALUE;
        pubs[i].gop_us = 0;
        atomic_init(&pubs[i].bytes_v, 0);
        atomic_init(&pubs[i].bytes_a, 0);
        atomic_init(&pubs[i].bytes_out, 0);
        atomic_init(&pubs[i].frames_v, 0);
        atomic_init(&pubs[i].gop_ready, 0);
        {
            int w, f;
            for (f = 0; f < LIVE_FMT_NB; f++) {
                atomic_init(&pubs[i].fmt_want[f], 0);
                atomic_init(&pubs[i].mux_on[f], 0);
                http_ring_init(&pubs[i].fmux[f].ring);
                for (w = 0; w < HTTP_EV_MAX; w++)
                    atomic_init(&pubs[i].viewers[w][f], 0);
            }
        }
        ffmpeg_http_log( AV_LOG_INFO,
               "http-live: GOP /%s/%s.flv|.ts|.mp4 <- output file %d\n",
               pubs[i].app, pubs[i].stream, pubs[i].file_index);
    }

    atomic_store(&live_running, 1);
    atomic_store(&live_inited, 1);
    atomic_store(&live_stat_on, 1);
    if (!live_stat_started &&
        !pthread_create(&live_stat_th, NULL, live_stat_thread, NULL))
        live_stat_started = 1;
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

    atomic_store(&live_stat_on, 0);
    if (live_stat_started) {
        pthread_join(live_stat_th, NULL);
        live_stat_started = 0;
    }
    if (atomic_load(&live_inited)) {
        ffmpeg_http_live_shutdown();
        pthread_mutex_lock(&live_mutex);
        for (i = 0; i < nb_pubs; i++) {
            live_list_free(&pubs[i].ready);
            live_list_free(&pubs[i].building);
            avcodec_parameters_free(&pubs[i].vpar);
            avcodec_parameters_free(&pubs[i].apar);
            pubs[i].subs = NULL;
            {
                int f;
                for (f = 0; f < LIVE_FMT_NB; f++) {
                    live_mux_free(&pubs[i].fmux[f]);
                    atomic_store(&pubs[i].mux_on[f], 0);
                }
            }
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
        live_note_media(pub, owned, is_video, is_key);
        live_fanout(pub, owned, st);
        if (live_gop_cached(pub) && !atomic_exchange(&pub->gop_ready, 1))
            live_notify_workers(pub, HTTP_EV_MSG_GOP_READY, 0);
        live_mux_packet(pub, owned, st);
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
    struct LiveFmtMux *m;
    HttpLiveBuf *b = NULL;
    int fi, wid, ret = 0, wait_ms;
    int64_t t_wait, deadline;
    unsigned rpos;
    const char *why = "ok";
    int wait_key;

    if (!c || !fmt || !mime)
        return AVERROR(EINVAL);
    fi = live_fmt_idx(fmt);
    wid = ffmpeg_http_conn_worker_id(c);
    if (wid < 0 || wid >= HTTP_EV_MAX)
        wid = 0;
    if (!ffmpeg_http_live_enabled() || pub_idx < 0 || pub_idx >= nb_pubs || fi < 0) {
        ffmpeg_http_conn_note(c, "live-off");
        return live_send_status(c, 503, "Live publisher off");
    }
    pub = &pubs[pub_idx];
    m = &pub->fmux[fi];
    if (accept_us <= 0)
        accept_us = av_gettime_relative();
    ffmpeg_http_log(AV_LOG_INFO, "http-live: [%s] connect /%s/%s %s%s req=%dms\n",
           peer, pub->app, pub->stream, fmt, lowdelay ? " lowdelay" : "",
           (int)((av_gettime_relative() - accept_us) / 1000));

    ffmpeg_http_conn_mark_live(c, pub_idx);
    atomic_fetch_add(&pub->viewers[wid][fi], 1);
    atomic_fetch_add(&pub->fmt_want[fi], 1);

    t_wait = av_gettime_relative();
    deadline = t_wait + (pub->vpar ? 2000000LL : LIVE_WAIT_PAR_US);
    while (atomic_load(&live_running) && !atomic_load(&pub->gop_ready) &&
           !(head_only && pub->vpar) && !lowdelay) {
        if (ffmpeg_http_conn_kicked(c)) {
            why = "kicked";
            goto out;
        }
        if (av_gettime_relative() >= deadline)
            break;
        ffmpeg_http_conn_wait_msg(c);
        while (ffmpeg_http_conn_got_msg(c, NULL, NULL, NULL))
            ;
    }
    wait_ms = (int)((av_gettime_relative() - t_wait) / 1000);
    if (!head_only && !lowdelay && !atomic_load(&pub->gop_ready) && !pub->vpar) {
        why = "gop-not-ready";
        ffmpeg_http_log(AV_LOG_WARNING,
               "http-live: [%s] GOP not ready wait=%dms cache=%s\n",
               peer, wait_ms, live_cache_name(pub));
        live_send_status(c, 503, "Live GOP not ready");
        goto out;
    }
    ffmpeg_http_log(AV_LOG_INFO,
           "http-live: [%s] gop ready wait=%dms cache=%s extra=%d\n",
           peer, wait_ms, live_cache_name(pub),
           pub->vpar ? pub->vpar->extradata_size : 0);
    if (live_send_ok_hdr(c, mime) < 0) {
        why = "write-200";
        goto out;
    }
    ffmpeg_http_log(AV_LOG_INFO, "http-live: [%s] http 200 %s\n", peer, mime);
    if (head_only) {
        why = "head";
        goto out;
    }

    deadline = av_gettime_relative() + 2000000LL;
    while (atomic_load(&live_running) && !atomic_load(&pub->mux_on[fi])) {
        if (ffmpeg_http_conn_kicked(c)) {
            why = "kicked";
            goto out;
        }
        if (av_gettime_relative() >= deadline) {
            why = "mux-timeout";
            goto out;
        }
        ffmpeg_http_conn_wait_msg(c);
        while (ffmpeg_http_conn_got_msg(c, NULL, NULL, NULL))
            ;
    }
    if (!atomic_load(&pub->mux_on[fi])) {
        why = "mux-off";
        goto out;
    }
    ffmpeg_http_log(AV_LOG_INFO, "http-live: [%s] mux header ok\n", peer);
    if (m->header && ffmpeg_http_conn_write(c, m->header->data, m->header->size) < 0) {
        why = ffmpeg_http_conn_kicked(c) ? "kicked" : "write-fail";
        goto out;
    }
    if (m->header)
        atomic_fetch_add(&pub->bytes_out, (uint64_t)m->header->size);
    wait_key = 1;
    if (lowdelay) {
        rpos = http_ring_wpos(&m->ring);
        wait_key = 1;
    } else {
        rpos = http_ring_valid_gop(&m->ring);
        /* TS used to start at oldest when no GOP mark was seen. Audio played
         * immediately while video waited for the next IDR (~1s / half GOP). */
        if (rpos == http_ring_wpos(&m->ring))
            wait_key = 1;
    }
    ffmpeg_http_log(AV_LOG_INFO,
           "http-live: [%s] streaming /%s/%s %s%s setup=%dms wait=%dms\n",
           peer, pub->app, pub->stream, fmt, lowdelay ? " lowdelay" : "",
           (int)((av_gettime_relative() - accept_us) / 1000), wait_ms);

    while (atomic_load(&live_running)) {
        if (ffmpeg_http_conn_kicked(c)) {
            why = "kicked";
            goto out;
        }
        if (http_ring_stale(&m->ring, rpos)) {
            unsigned jump = http_ring_valid_gop(&m->ring);
            ffmpeg_http_log(AV_LOG_WARNING,
                   "http-live: [%s] client lagged, skip GOP rpos=%u -> %u w=%u\n",
                   peer, rpos, jump, http_ring_wpos(&m->ring));
            rpos = jump;
            wait_key = 1;
            why = "lagged";
        }
        while (!http_ring_stale(&m->ring, rpos) && rpos < http_ring_wpos(&m->ring)) {
            b = http_ring_get(&m->ring, rpos);
            if (!b) {
                if (http_ring_stale(&m->ring, rpos))
            break;
                rpos++;
                continue;
            }
            rpos++;
            if (wait_key && !b->is_key) {
                http_livebuf_unref(&b);
                continue;
            }
            wait_key = 0;
            ret = ffmpeg_http_conn_write(c, b->data, b->size);
            if (ret >= 0)
                atomic_fetch_add(&pub->bytes_out, (uint64_t)b->size);
            http_livebuf_unref(&b);
            if (ret < 0) {
                why = ffmpeg_http_conn_kicked(c) ? "kicked" : "write-fail";
                goto out;
            }
        }
        if (!atomic_load(&live_running))
            break;
        ffmpeg_http_conn_wait_msg(c);
        while (ffmpeg_http_conn_got_msg(c, NULL, NULL, NULL))
            ;
    }
    if (!strcmp(why, "ok") && !atomic_load(&live_running))
        why = "shutdown";
out:
    atomic_fetch_sub(&pub->viewers[wid][fi], 1);
    atomic_fetch_sub(&pub->fmt_want[fi], 1);
    ffmpeg_http_log(AV_LOG_INFO,
           "http-live: [%s] disconnect /%s/%s reason=%s dur=%dms bytes=%"PRId64"\n",
           peer, pub->app, pub->stream, why,
           (int)((av_gettime_relative() - accept_us) / 1000),
           ffmpeg_http_conn_bytes(c));
    ffmpeg_http_conn_note(c, why);
    return ret;
}

static int live_audio_channels(const AVCodecParameters *par)
{
    if (!par)
        return 0;
    return par->ch_layout.nb_channels;
}

static int live_collect_idxs(const char *spec, int all, int *idxs, int max)
{
    char buf[1024], *tok, *save = NULL;
    int n = 0, i, j;

    if (all || (spec && (!strcmp(spec, "all") || !strcmp(spec, "*")))) {
        for (i = 0; i < nb_pubs && n < max; i++)
            idxs[n++] = i;
        return n;
    }
    if (!spec || !spec[0] || !idxs || max <= 0)
        return 0;
    av_strlcpy(buf, spec, sizeof(buf));
    for (tok = av_strtok(buf, ",; \t", &save); tok && n < max;
         tok = av_strtok(NULL, ",; \t", &save)) {
        HttpLivePub *pub;
        char app[128], stream[128];
        char *slash;

        while (*tok == '/')
            tok++;
        if (!tok[0])
            continue;
        slash = strchr(tok, '/');
        if (slash && slash != tok && slash[1]) {
            av_strlcpy(app, tok, sizeof(app));
            app[FFMIN((int)(slash - tok), (int)sizeof(app) - 1)] = 0;
            av_strlcpy(stream, slash + 1, sizeof(stream));
            pub = live_find_pub(app, stream);
            if (pub) {
                int idx = (int)(pub - pubs);
                for (j = 0; j < n; j++)
                    if (idxs[j] == idx)
                break;
                if (j == n)
                    idxs[n++] = idx;
            }
        } else {
            for (i = 0; i < nb_pubs && n < max; i++) {
                if (strcmp(pubs[i].stream, tok))
                    continue;
                for (j = 0; j < n; j++)
                    if (idxs[j] == i)
            break;
                if (j == n)
                    idxs[n++] = i;
            }
        }
    }
    return n;
}

int ffmpeg_http_live_stat_json(char **out)
{
    AVBPrint bp;
    int i;

    if (!out)
        return AVERROR(EINVAL);
    *out = NULL;
    av_bprint_init(&bp, 2048, AV_BPRINT_SIZE_UNLIMITED);
    pthread_mutex_lock(&live_mutex);
    av_bprintf(&bp, "{\"code\":0,\"streams\":%d,\"data\":[", nb_pubs);
    for (i = 0; i < nb_pubs; i++) {
        HttpLivePub *pub = &pubs[i];
        char vc[32] = "", ac[32] = "", schema[280], dur[32];
        int pf[LIVE_FMT_NB] = {0};
        int clients, ch = 0, sr = 0, w = 0, h = 0, proto_n = 0;
        int64_t now = av_gettime_relative();
        int64_t dur_s = pub->t_start ? (now - pub->t_start) / 1000000 : 0;
        uint64_t bin;

        live_refresh_rates_l(pub);
        clients = live_pub_clients(pub, pf);
        if (pub->vpar) {
            live_codec_json(pub->vpar->codec_id, vc, sizeof(vc));
            w = pub->vpar->width;
            h = pub->vpar->height;
        }
        if (pub->apar) {
            live_codec_json(pub->apar->codec_id, ac, sizeof(ac));
            sr = pub->apar->sample_rate;
            ch = live_audio_channels(pub->apar);
        }
        snprintf(schema, sizeof(schema), "%s/%s", pub->app, pub->stream);
        live_fmt_dur(dur, sizeof(dur), dur_s);
        bin = atomic_load(&pub->bytes_v) + atomic_load(&pub->bytes_a);
        if (i)
            av_bprint_chars(&bp, ',', 1);
        av_bprintf(&bp, "{\"app\":");
        json_quote(&bp, pub->app);
        av_bprintf(&bp, ",\"stream\":");
        json_quote(&bp, pub->stream);
        av_bprintf(&bp, ",\"schema\":");
        json_quote(&bp, schema);
        av_bprintf(&bp,
                   ",\"video_codec\":\"%s\",\"width\":%d,\"height\":%d,"
                   "\"fps\":%.2f,\"gop\":%.2f,\"video_bitrate\":%"PRId64","
                   "\"audio_codec\":\"%s\",\"sample_rate\":%d,\"channels\":%d,"
                   "\"audio_bitrate\":%"PRId64","
                   "\"bitrate_in\":%"PRId64",\"bitrate_out\":%"PRId64","
                   "\"bytes_in\":%"PRIu64",\"bytes_out\":%"PRIu64","
                   "\"status\":\"%s\",\"duration\":%"PRId64",\"duration_str\":\"%s\","
                   "\"clients\":%d,\"clients_flv\":%d,\"clients_ts\":%d,\"clients_mp4\":%d,"
                   "\"protocols\":[",
                   vc, w, h, pub->fps,
                   pub->gop_us > 0 ? pub->gop_us / 1000000.0 : 0.0,
                   pub->v_bps, ac, sr, ch, pub->a_bps,
                   pub->v_bps + pub->a_bps, pub->out_bps,
                   bin, (uint64_t)atomic_load(&pub->bytes_out),
                   atomic_load(&pub->gop_ready) ? "active" :
                       (pub->t_start ? "wait" : "idle"),
                   dur_s, dur, clients, pf[LIVE_FMT_FLV], pf[LIVE_FMT_TS],
                   pf[LIVE_FMT_MP4]);
        if (atomic_load(&pub->mux_on[LIVE_FMT_FLV]) || pf[LIVE_FMT_FLV] > 0) {
            av_bprintf(&bp, "%s\"flv\"", proto_n ? "," : "");
            proto_n++;
        }
        if (atomic_load(&pub->mux_on[LIVE_FMT_TS]) || pf[LIVE_FMT_TS] > 0) {
            av_bprintf(&bp, "%s\"ts\"", proto_n ? "," : "");
            proto_n++;
        }
        if (atomic_load(&pub->mux_on[LIVE_FMT_MP4]) || pf[LIVE_FMT_MP4] > 0) {
            av_bprintf(&bp, "%s\"fmp4\"", proto_n ? "," : "");
            proto_n++;
        }
        av_bprintf(&bp, "]}");
    }
    av_bprintf(&bp, "]}");
    pthread_mutex_unlock(&live_mutex);
    if (!av_bprint_is_complete(&bp)) {
        av_bprint_finalize(&bp, NULL);
        return AVERROR(ENOMEM);
    }
    return av_bprint_finalize(&bp, out);
}

int ffmpeg_http_live_kick(const char *spec, int all, char **out)
{
    AVBPrint bp;
    int idxs[64], n, i, kicked = 0;

    if (!out)
        return AVERROR(EINVAL);
    *out = NULL;
    if (!atomic_load(&live_inited)) {
        av_bprint_init(&bp, 128, AV_BPRINT_SIZE_UNLIMITED);
        av_bprintf(&bp, "{\"code\":-1,\"kicked\":0,\"error\":\"live off\",\"streams\":[]}");
        return av_bprint_finalize(&bp, out);
    }
    pthread_mutex_lock(&live_mutex);
    n = live_collect_idxs(spec, all, idxs, FF_ARRAY_ELEMS(idxs));
    av_bprint_init(&bp, 256, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "{\"code\":%d,\"kicked\":", (n > 0 || all) ? 0 : -2);
    for (i = 0; i < n; i++)
        kicked += live_pub_clients(&pubs[idxs[i]], NULL);
    av_bprintf(&bp, "%d,\"streams\":[", kicked);
    for (i = 0; i < n; i++) {
        char schema[280];
        snprintf(schema, sizeof(schema), "%s/%s",
                 pubs[idxs[i]].app, pubs[idxs[i]].stream);
        if (i)
            av_bprint_chars(&bp, ',', 1);
        json_quote(&bp, schema);
        live_notify_kick(idxs[i]);
        ffmpeg_http_log(AV_LOG_WARNING,
               "http-live: kick %s/%s clients=%d\n",
               pubs[idxs[i]].app, pubs[idxs[i]].stream,
               live_pub_clients(&pubs[idxs[i]], NULL));
    }
    if (n <= 0 && !all)
        av_bprintf(&bp, "],\"error\":\"not found\"}");
    else
        av_bprintf(&bp, "]}");
    pthread_mutex_unlock(&live_mutex);
    if (!av_bprint_is_complete(&bp)) {
        av_bprint_finalize(&bp, NULL);
        return AVERROR(ENOMEM);
    }
    return av_bprint_finalize(&bp, out);
}
