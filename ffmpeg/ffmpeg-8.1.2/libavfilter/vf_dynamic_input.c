/*
 * Dynamic multi-source CUDA video (+ optional audio) source filter.
 *
 * Outputs N video pads (AV_PIX_FMT_CUDA NV12). With enable_audio=1, also
 * appends N audio pads (FLTP stereo @ sample_rate) demuxed from the same URL.
 * live=1 (default): wall-clock paced *video* at rate r; audio is pulled when
 * downstream wants samples so amixrank is not starved at 25fps×~1024 samples.
 * file sources also -re paced on demux; realtime URLs skip decode sleep.
 * playout_delay_ms (default 40): A and V share one media→wall due timeline
 * after prime (output lip-sync). Video may drop frames that lag last released
 * audio media PTS (catch-up) but must not hold while audio advances program PTS.
 * |A-V|<=sync_warn_av_ms (default 80) is silent; anomalies log dyn_sync.
 * Peer-late: wait av_wait_ms then trim to common media PTS.
 * PTS jumps: ffmpeg-style dts_delta_threshold (pts_jump_s, default 10s).
 * Output program PTS: video=media_frame_idx, audio=per-pad sample counter.
 * Runtime process_command via graph zmq/azmq/sendcmd:
 *   search / update / remove / purge (slots 1..N; 0=bg unused).
 * update opens on a side-thread pool; graph keeps playing until hot-swap.
 * Per-stream isolation.
 *
 * Patterns reused:
 *  - src_movie.c  : avformat_open_input / av_read_frame / decode / activate
 *  - vsrc_color_cuda.c : AVFILTER_FLAG_HWDEVICE + hw_frames_ctx setup
 *
 * This file is part of FFmpeg.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/cuda_check.h"
#include "libavutil/samplefmt.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"

#include "libswresample/swresample.h"

#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#define DYN_INPUT_MAX 32
/* Sized from playout_delay_ms + av_wait_ms; hold early audio up to ~av_wait. */
#define DYN_AUDIO_Q_MIN 8
#define DYN_AUDIO_Q_MAX 384
/* Short ordered video FIFO so A/V can share PTS due without latest-frame starve. */
#define DYN_VIDEO_Q_MIN 4
#define DYN_VIDEO_Q_MAX 16

typedef struct DynSource {
    char                *url;
    AVFormatContext     *fmt_ctx;
    AVCodecContext      *dec_ctx;           /* video */
    AVCodecContext      *adec_ctx;          /* audio */
    SwrContext          *swr;
    int                  video_stream_idx;
    int                  audio_stream_idx;
    AVPacket            *pkt;
    AVFrame             *sw_frame;
    AVFrame             *a_frame;
    int                  eof;
    int                  active;
    pthread_t            thread;
    int                  thread_started;
    int                  stop;

    pthread_mutex_t      lock;
    AVFifo              *video_q;           /* ordered decoded video (PTS playout) */
    AVFifo              *audio_q;           /* queued decoded audio frames */
    int64_t              audio_samples_out; /* monotonic audio PTS, per pad */
    int64_t              audio_drops;       /* frames dropped on full queue */
    int64_t              video_drops;       /* frames dropped on full video queue */
    int64_t              v_catchup;         /* video frames dropped to follow audio */
    int64_t              last_drop_log_us;
    int64_t              last_pts;

    /* Container A/V sync: prime on common media PTS, not wall-arrival alone. */
    int                  av_primed;
    int64_t              sync_pts0;         /* media us at prime */
    int64_t              sync_wall0;        /* wall us at prime */
    int64_t              first_audio_wall_us;
    int64_t              first_video_wall_us;
    /* Audio-master playout: last released media PTS (us). */
    int64_t              play_a_media_us;
    int64_t              play_v_media_us;
    int64_t              last_sync_warn_us;

    /* ffmpeg-like discontinuity correction (shared offset for A+V on this URL). */
    int64_t              ts_offset_us;
    int64_t              last_v_media_us;
    int64_t              last_a_media_us;
    int64_t              last_discont_log_us;

    int64_t              last_reconnect_log_us;
    int                  ever_opened;    /* 1 after at least one successful open */
    int                  got_media;      /* 1 after first decoded A/V since open */
    int                  read_fail_streak;
    int                  force_reconnect; /* cmd: soft restart in-place (same URL) */
    int                  pending_remove;  /* cmd: soft-remove (keep thread, drop URL) */
    /* Pre-opened source for seamless update: thread closes old, adopts this,
     * keeps cur_hw so batch update does not black/glitch the mix. */
    struct DynSource    *pending_open;
    int                  is_realtime;    /* RTMP/etc: no decode-side -re sleep */
    int                  have_in_anchor; /* input PTS↔wall clock for file -re */
    int64_t              in_pts0;
    int64_t              in_wall0;
    int64_t              removed_us;      /* wall us when soft-removed (hold last frame) */
    AVFrame             *cur_hw;
    uint64_t             cur_seq;         /* bumps when cur_hw gets a new decode */
    uint64_t             pushed_seq;      /* last cur_seq pushed downstream */
    /* Content generation for mixing_cuda tile transitions. Bumps only on
     * update / cold insert / reconnect — NOT every decoded frame. */
    uint64_t             content_gen;
    int                  need_content_gen; /* arm bump on next new cur_hw */
} DynSource;

typedef struct DynamicInputContext {
    const AVClass       *class;

    AVCUDADeviceContext *hwctx;
    AVBufferRef         *device_ref;
    AVBufferRef         *own_device;
    AVBufferRef         *frames_ctx;

    int                  out_w, out_h;
    char                *size_str;
    AVRational           frame_rate;
    enum AVPixelFormat   sw_format;
    char                *sw_format_str;
    int                  device_idx;
    char                *initial_urls;
    int                  enable_audio;
    int                  sample_rate;
    int                  reconnect_ms;   /* fixed reconnect interval while URL is down */
    int                  live;           /* 1=wall-clock pace (default); 0=ASAP */
    int                  playout_delay_ms; /* small hold after PTS-aligned release */
    int                  av_wait_ms;     /* wait for peer A/V before solo start */
    float                pts_jump_s;     /* discontinuity threshold (like dts_delta_threshold) */
    int                  sync_warn_av_ms; /* |A-V| above this → dyn_sync WARN; else silent */
    int                  sync_warn_interval_ms;

    DynSource            srcs[DYN_INPUT_MAX];
    int                  nb_outputs;
    pthread_mutex_t      url_lock;

    /* Async update open worker: open RTMP off the filtergraph thread. */
    pthread_t            open_thr;
    int                  open_thr_started;
    int                  open_thr_stop;
    pthread_mutex_t      open_mu;
    pthread_cond_t       open_cv;
    struct DynOpenBatch *open_batch;   /* pending job; replaced cancels prior */
    unsigned             update_gen;   /* bump to discard stale opens */

    /* Filter-level wall clock for VIDEO pads (shared timeline). */
    int                  wall_started;
    int64_t              wall_start_us;
    int64_t              next_tick_us;
    int64_t              media_frame_idx; /* monotonic video frame index */

    AVRational           time_base;
    AVChannelLayout      ch_layout;
} DynamicInputContext;

typedef struct DynOpenBatch {
    AVFilterContext *ctx;
    unsigned         gen;
    int              n;
    int              slots[DYN_INPUT_MAX];      /* 1-based */
    char             urls[DYN_INPUT_MAX][1024];
} DynOpenBatch;

typedef struct DynOpenOneArg {
    AVFilterContext *ctx;
    char             url[1024];
    DynSource        opened;
    int              ret;
} DynOpenOneArg;

#define OFFSET(x) offsetof(DynamicInputContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
#define AFLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM
#define TFLAGS FLAGS | AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption dynamic_input_options[] = {
    { "outputs",  "number of output pads per media type", OFFSET(nb_outputs), AV_OPT_TYPE_INT, {.i64 = 8}, 1, DYN_INPUT_MAX, FLAGS },
    { "size",     "output size WxH",         OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = "1920x1080"}, 0, 0, FLAGS },
    { "s",        "output size WxH",         OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = "1920x1080"}, 0, 0, FLAGS },
    { "r",        "output frame rate",        OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, FLAGS },
    { "sw_format","hardware sw format",       OFFSET(sw_format_str), AV_OPT_TYPE_STRING, {.str = "nv12"}, 0, 0, FLAGS },
    { "initial_urls", "comma-separated initial urls", OFFSET(initial_urls), AV_OPT_TYPE_STRING, {0}, 0, 0, TFLAGS },
    { "device",   "CUDA device idx when no -init_hw_device", OFFSET(device_idx), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS },
    { "enable_audio", "also demux/decode audio pads a0..aN from same urls", OFFSET(enable_audio), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS },
    { "sample_rate", "output audio sample rate when enable_audio=1", OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64 = 48000}, 8000, 192000, AFLAGS },
    { "reconnect_ms", "fixed reconnect interval (ms) while a configured URL is down", OFFSET(reconnect_ms), AV_OPT_TYPE_INT, {.i64 = 2000}, 200, 60000, TFLAGS },
    { "live", "1=wall-clock realtime output (like -re); 0=ASAP for recording", OFFSET(live), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, TFLAGS },
    { "playout_delay_ms", "Small hold (ms) after A/V are PTS-aligned "
                          "(0=release as soon as due on the sync timeline).",
            OFFSET(playout_delay_ms), AV_OPT_TYPE_INT, {.i64 = 40}, 0, 2000, TFLAGS },
    { "av_wait_ms", "If audio (or video) arrives first, wait this long for the "
                    "peer before starting alone; then trim to common media PTS.",
            OFFSET(av_wait_ms), AV_OPT_TYPE_INT, {.i64 = 6000}, 0, 30000, TFLAGS },
    { "pts_jump_s", "Correct container PTS/DTS jumps larger than this many seconds "
                    "(ffmpeg dts_delta_threshold style; 0=disable).",
            OFFSET(pts_jump_s), AV_OPT_TYPE_FLOAT, {.dbl = 10.0}, 0, 3600, TFLAGS },
    { "sync_warn_av_ms", "Warn when |audio-video| media PTS skew exceeds this (ms). "
                         "At or below: silent (realtime OK). Default 80.",
            OFFSET(sync_warn_av_ms), AV_OPT_TYPE_INT, {.i64 = 200}, 0, 2000, TFLAGS },
    { "sync_warn_interval_ms", "Min interval between dyn_sync warnings for one pad.",
            OFFSET(sync_warn_interval_ms), AV_OPT_TYPE_INT, {.i64 = 10000}, 1000, 600000, TFLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dynamic_input);

static int dynamic_input_config_video(AVFilterLink *outlink);
static int dynamic_input_config_audio(AVFilterLink *outlink);

static void dyn_close_source(DynSource *ds);
static void dyn_adopt_opened(DynSource *dst, DynSource *src);
static int dyn_start_source(AVFilterContext *ctx, int idx);
static int dyn_open_worker_start(AVFilterContext *ctx);

static int dyn_alloc_hw_frames(AVFilterContext *ctx, int w, int h)
{
    DynamicInputContext *s = ctx->priv;
    AVHWFramesContext *fc;

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = av_hwframe_ctx_alloc(s->device_ref);
    if (!s->frames_ctx)
        return AVERROR(ENOMEM);

    fc = (AVHWFramesContext *)s->frames_ctx->data;
    fc->format    = AV_PIX_FMT_CUDA;
    fc->sw_format = s->sw_format;
    fc->width     = FFALIGN(w, 32);
    fc->height    = FFALIGN(h, 32);
    fc->initial_pool_size = 8;
    return av_hwframe_ctx_init(s->frames_ctx);
}

static int64_t dyn_playout_delay_us(const DynamicInputContext *s)
{
    if (!s || s->playout_delay_ms <= 0)
        return 0;
    return (int64_t)s->playout_delay_ms * 1000;
}

/* Cover playout + peer-wait so early audio is not dropped before video. */
static int dyn_audio_q_depth(const DynamicInputContext *s)
{
    int ms = FFMAX(s->playout_delay_ms, 0) + FFMAX(s->av_wait_ms, 0) + 100;
    int n  = ms / 20 + 8;
    return FFMAX(FFMIN(n, DYN_AUDIO_Q_MAX), DYN_AUDIO_Q_MIN);
}

/* Enough frames for playout_delay + one jitter frame at output rate. */
static int dyn_video_q_depth(const DynamicInputContext *s)
{
    int fps = 25;
    int ms, n;
    if (s->frame_rate.num > 0 && s->frame_rate.den > 0)
        fps = (s->frame_rate.num + s->frame_rate.den - 1) / s->frame_rate.den;
    if (fps < 1)
        fps = 25;
    ms = FFMAX(s->playout_delay_ms, 0) + 80;
    n  = (ms * fps) / 1000 + 2;
    return FFMAX(FFMIN(n, DYN_VIDEO_Q_MAX), DYN_VIDEO_Q_MIN);
}

static void dyn_reset_av_sync(DynSource *ds)
{
    ds->av_primed = 0;
    ds->sync_pts0 = 0;
    ds->sync_wall0 = 0;
    ds->first_audio_wall_us = 0;
    ds->first_video_wall_us = 0;
    ds->ts_offset_us = 0;
    ds->last_v_media_us = AV_NOPTS_VALUE;
    ds->last_a_media_us = AV_NOPTS_VALUE;
    ds->last_discont_log_us = 0;
    ds->play_a_media_us = AV_NOPTS_VALUE;
    ds->play_v_media_us = AV_NOPTS_VALUE;
    ds->v_catchup = 0;
}

static int64_t dyn_meta_int64(const AVFrame *f, const char *key)
{
    AVDictionaryEntry *e;
    if (!f)
        return AV_NOPTS_VALUE;
    e = av_dict_get(f->metadata, key, NULL, 0);
    if (!e || !e->value)
        return AV_NOPTS_VALUE;
    return strtoll(e->value, NULL, 10);
}

static int64_t dyn_stream_pts_us(DynSource *ds, int64_t pts, int is_audio)
{
    AVStream *st;
    int idx = is_audio ? ds->audio_stream_idx : ds->video_stream_idx;
    if (pts == AV_NOPTS_VALUE || !ds->fmt_ctx || idx < 0 ||
        idx >= (int)ds->fmt_ctx->nb_streams)
        return AV_NOPTS_VALUE;
    st = ds->fmt_ctx->streams[idx];
    if (!st)
        return AV_NOPTS_VALUE;
    return av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q);
}

/*
 * Apply ffmpeg-style discontinuity correction before playout.
 * Shared ts_offset_us keeps A/V on the same corrected timeline for this URL.
 */
static int64_t dyn_correct_media_pts(AVFilterContext *ctx, DynamicInputContext *s,
                                     DynSource *ds, int is_audio, int64_t media_us)
{
    int64_t *last = is_audio ? &ds->last_a_media_us : &ds->last_v_media_us;
    int64_t threshold_us, corrected, delta, now;

    if (media_us == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;

    corrected = media_us + ds->ts_offset_us;

    if (s->pts_jump_s > 0 && *last != AV_NOPTS_VALUE) {
        threshold_us = (int64_t)(s->pts_jump_s * AV_TIME_BASE + 0.5);
        if (threshold_us < AV_TIME_BASE)
            threshold_us = AV_TIME_BASE;
        delta = corrected - *last;
        if (FFABS(delta) > threshold_us) {
            /* Continuity: shift offset so this stamp lands near last. */
            ds->ts_offset_us -= delta;
            corrected = media_us + ds->ts_offset_us;
            now = av_gettime_relative();
            if (now - ds->last_discont_log_us > 1000000) {
                ds->last_discont_log_us = now;
                av_log(ctx, AV_LOG_WARNING,
                       "dynamic_input: %s timestamp discontinuity on '%s': "
                       "delta=%.3fs, new offset=%.3fs (pts_jump_s=%.1f)\n",
                       is_audio ? "audio" : "video",
                       ds->url ? ds->url : "?",
                       delta / 1000000.0, ds->ts_offset_us / 1000000.0,
                       s->pts_jump_s);
            }
        }
    }

    *last = corrected;
    return corrected;
}

static void dyn_fifo_clear_frames(AVFifo *q)
{
    AVFrame *f;
    if (!q)
        return;
    while (av_fifo_read(q, &f, 1) >= 0)
        av_frame_free(&f);
}

static void dyn_audio_q_clear_locked(DynSource *ds)
{
    dyn_fifo_clear_frames(ds->audio_q);
}

static void dyn_video_q_clear_locked(DynSource *ds)
{
    dyn_fifo_clear_frames(ds->video_q);
}

static int dyn_audio_q_push_locked(DynSource *ds, AVFrame *frame)
{
    if (!ds->audio_q)
        return AVERROR(EINVAL);
    /* Drop oldest when full so decode thread never blocks. */
    while (av_fifo_can_write(ds->audio_q) < 1) {
        AVFrame *old = NULL;
        if (av_fifo_read(ds->audio_q, &old, 1) < 0)
            break;
        av_frame_free(&old);
        ds->audio_drops++;
    }
    return av_fifo_write(ds->audio_q, &frame, 1);
}

static int dyn_video_q_push_locked(DynSource *ds, AVFrame *frame)
{
    if (!ds->video_q)
        return AVERROR(EINVAL);
    /* Live: drop oldest to keep decode-order tail (not newest-only replace). */
    while (av_fifo_can_write(ds->video_q) < 1) {
        AVFrame *old = NULL;
        if (av_fifo_read(ds->video_q, &old, 1) < 0)
            break;
        av_frame_free(&old);
        ds->video_drops++;
    }
    return av_fifo_write(ds->video_q, &frame, 1);
}

static int dyn_fifo_drain_before_pts(AVFifo *q, int64_t start_pts_us)
{
    int dropped = 0;
    AVFrame *f;

    if (start_pts_us == AV_NOPTS_VALUE || !q)
        return 0;
    while (av_fifo_can_read(q) >= 1) {
        int64_t pts;
        if (av_fifo_peek(q, &f, 1, 0) < 0 || !f)
            break;
        pts = dyn_meta_int64(f, "lavfi.dyn_media_pts_us");
        /* 20ms tolerance — keep frame if at/after start. */
        if (pts == AV_NOPTS_VALUE || pts + 20000 >= start_pts_us)
            break;
        if (av_fifo_read(q, &f, 1) < 0)
            break;
        av_frame_free(&f);
        dropped++;
    }
    return dropped;
}

static int dyn_audio_drain_before_pts_locked(DynSource *ds, int64_t start_pts_us)
{
    return dyn_fifo_drain_before_pts(ds->audio_q, start_pts_us);
}

static int dyn_video_drain_before_pts_locked(DynSource *ds, int64_t start_pts_us)
{
    return dyn_fifo_drain_before_pts(ds->video_q, start_pts_us);
}

/*
 * Establish a common media timeline when both A and V exist.
 * Audio-early: wait for video (or av_wait_ms timeout), then trim audio to
 * video PTS (or max of heads). Video-early: may show video before prime;
 * audio stays gated until primed.
 */
static void dyn_try_prime_av_locked(AVFilterContext *ctx, DynamicInputContext *s,
                                    DynSource *ds)
{
    int need_v = ds->video_stream_idx >= 0;
    int need_a = s->enable_audio && ds->audio_stream_idx >= 0;
    int has_v  = (ds->video_q && av_fifo_can_read(ds->video_q) > 0) || !!ds->cur_hw;
    int has_a  = ds->audio_q && av_fifo_can_read(ds->audio_q) > 0;
    int64_t now = av_gettime_relative();
    int64_t v_pts = AV_NOPTS_VALUE, a_pts = AV_NOPTS_VALUE, start_pts;
    int drained_a, drained_v;
    AVFrame *vf = NULL, *af = NULL;

    if (ds->av_primed || (!need_v && !need_a))
        return;

    if (need_v && need_a) {
        if (has_a && !has_v) {
            if (ds->first_audio_wall_us <= 0)
                ds->first_audio_wall_us = now;
            if (s->av_wait_ms > 0 &&
                now - ds->first_audio_wall_us < (int64_t)s->av_wait_ms * 1000)
                return; /* keep buffering audio */
            /* timeout: start audio alone */
            need_v = 0;
            av_log(ctx, AV_LOG_WARNING,
                   "dynamic_input: AV wait timeout (%dms) on '%s' — "
                   "starting audio without video\n",
                   s->av_wait_ms, ds->url ? ds->url : "?");
        } else if (has_v && !has_a) {
            /* Video may display unprimed; audio waits for peer or timeout. */
            if (ds->first_video_wall_us <= 0)
                ds->first_video_wall_us = now;
            if (s->av_wait_ms > 0 &&
                now - ds->first_video_wall_us < (int64_t)s->av_wait_ms * 1000)
                return;
            need_a = 0;
            av_log(ctx, AV_LOG_WARNING,
                   "dynamic_input: AV wait timeout (%dms) on '%s' — "
                   "starting video without audio\n",
                   s->av_wait_ms, ds->url ? ds->url : "?");
        } else if (!has_v || !has_a) {
            return;
        }
    } else if (need_v && !has_v) {
        return;
    } else if (need_a && !has_a) {
        return;
    }

    if (need_v) {
        if (ds->video_q && av_fifo_can_read(ds->video_q) > 0)
            av_fifo_peek(ds->video_q, &vf, 1, 0);
        else
            vf = ds->cur_hw;
        if (vf)
            v_pts = dyn_meta_int64(vf, "lavfi.dyn_media_pts_us");
    }
    if (need_a && has_a) {
        if (av_fifo_peek(ds->audio_q, &af, 1, 0) >= 0 && af)
            a_pts = dyn_meta_int64(af, "lavfi.dyn_media_pts_us");
    }

    if (v_pts != AV_NOPTS_VALUE && a_pts != AV_NOPTS_VALUE)
        start_pts = FFMAX(v_pts, a_pts); /* jump to where both have data */
    else if (v_pts != AV_NOPTS_VALUE)
        start_pts = v_pts;
    else if (a_pts != AV_NOPTS_VALUE)
        start_pts = a_pts;
    else
        start_pts = 0;

    drained_a = dyn_audio_drain_before_pts_locked(ds, start_pts);
    drained_v = dyn_video_drain_before_pts_locked(ds, start_pts);
    ds->sync_pts0  = start_pts;
    ds->sync_wall0 = now;
    ds->av_primed  = 1;
    av_log(ctx, AV_LOG_INFO,
           "dynamic_input: AV primed '%s' start_pts=%.3fs drained_a=%d drained_v=%d "
           "(shared PTS timeline; playout_delay_ms=%d)\n",
           ds->url ? ds->url : "?", start_pts / 1000000.0, drained_a, drained_v,
           s->playout_delay_ms);
}

/*
 * Shared A/V due rule (ordered FIFOs keep input order):
 *  - after prime: corrected media PTS vs sync_wall0 (lip-sync)
 *  - before prime / no media pts: wall arrival + playout_delay
 * Live skew: if PTS timeline runs ahead of wall, waiting creates audio holes
 * (amixrank silence / re-arm). When the packet has already aged past
 * arrival+delay and PTS due is >80ms in the future, re-anchor sync_wall0
 * and release — same rule for A and V so they stay aligned.
 */
static int dyn_frame_is_due_locked(DynSource *ds, AVFrame *f, int64_t arrival_us,
                                   int64_t now_us, int64_t delay_us)
{
    int64_t media_us, due, ahead;

    if (!f)
        return 0;
    if (arrival_us <= 0)
        arrival_us = dyn_meta_int64(f, "lavfi.dyn_arrival_us");

    if (ds->av_primed) {
        media_us = dyn_meta_int64(f, "lavfi.dyn_media_pts_us");
        if (media_us != AV_NOPTS_VALUE) {
            due = ds->sync_wall0 + (media_us - ds->sync_pts0) + delay_us;
            if (now_us >= due)
                return 1;
            ahead = due - now_us;
            /* Past jitter cushion: re-anchor instead of freezing the pane
             * (multi-pull RTMP / PTS ahead after prime). Shared sync_wall0
             * keeps A/V on the same corrected timeline. */
            if (arrival_us > 0 &&
                now_us >= arrival_us + delay_us + 100000) {
                ds->sync_wall0 = now_us - (media_us - ds->sync_pts0);
                return 1;
            }
            if (ahead > 80000 &&
                (delay_us <= 0 ||
                 (arrival_us > 0 && now_us >= arrival_us + delay_us))) {
                ds->sync_wall0 = now_us - (media_us - ds->sync_pts0);
                return 1;
            }
            return 0;
        }
    }
    if (delay_us <= 0)
        return 1;
    return arrival_us > 0 && now_us >= arrival_us + delay_us;
}

static AVFrame *dyn_q_pop_due_locked(AVFifo *q, DynSource *ds, int64_t now_us,
                                     int64_t delay_us, int64_t *out_media_us)
{
    AVFrame *f = NULL;
    int64_t arrival_us, media_us;

    if (out_media_us)
        *out_media_us = AV_NOPTS_VALUE;
    if (!q || av_fifo_can_read(q) < 1)
        return NULL;
    if (av_fifo_peek(q, &f, 1, 0) < 0 || !f)
        return NULL;
    arrival_us = dyn_meta_int64(f, "lavfi.dyn_arrival_us");
    if (!dyn_frame_is_due_locked(ds, f, arrival_us, now_us, delay_us)) {
        /* Backlog: release head once past playout delay so thumbs keep moving
         * when compose runs faster than a starved demux can pace PTS. */
        if (!(av_fifo_can_read(q) >= 2 && arrival_us > 0 &&
              now_us >= arrival_us + delay_us))
            return NULL;
        {
            media_us = dyn_meta_int64(f, "lavfi.dyn_media_pts_us");
            if (ds->av_primed && media_us != AV_NOPTS_VALUE)
                ds->sync_wall0 = now_us - (media_us - ds->sync_pts0);
        }
    }
    if (av_fifo_read(q, &f, 1) < 0)
        return NULL;
    media_us = dyn_meta_int64(f, "lavfi.dyn_media_pts_us");
    if (out_media_us)
        *out_media_us = media_us;
    av_dict_set(&f->metadata, "lavfi.dyn_arrival_us", NULL, 0);
    av_dict_set(&f->metadata, "lavfi.dyn_media_pts_us", NULL, 0);
    return f;
}

/* Pop when primed (if video exists) and due on the sync timeline. */
static AVFrame *dyn_audio_q_pop_due_locked(DynSource *ds, int64_t now_us,
                                           int64_t delay_us, int64_t *out_media_us)
{
    if (ds->video_stream_idx >= 0 && !ds->av_primed)
        return NULL;
    return dyn_q_pop_due_locked(ds->audio_q, ds, now_us, delay_us, out_media_us);
}

/*
 * Output lip-sync requires A and V to leave on the SAME media→wall due timeline
 * (sync_wall0). Do NOT hold video while audio keeps advancing program PTS — that
 * freezes mouths on the mix while sound moves.
 *
 * Audio-master catch-up only: drop video frames whose media PTS is clearly
 * behind the last released audio (late pictures). Release itself stays on
 * shared due with audio.
 */
static AVFrame *dyn_video_q_pop_follow_audio_locked(DynSource *ds, int64_t now_us,
                                                   int64_t delay_us, int64_t skew_us,
                                                   int64_t *out_media_us)
{
    AVFrame *f = NULL;
    int64_t a_pts = ds->play_a_media_us;
    int64_t v_pts;

    if (out_media_us)
        *out_media_us = AV_NOPTS_VALUE;

    /* Drop stale video behind last released audio (content catch-up only). */
    if (a_pts != AV_NOPTS_VALUE && ds->audio_stream_idx >= 0 && skew_us > 0) {
        while (ds->video_q && av_fifo_can_read(ds->video_q) >= 1) {
            if (av_fifo_peek(ds->video_q, &f, 1, 0) < 0 || !f)
                break;
            v_pts = dyn_meta_int64(f, "lavfi.dyn_media_pts_us");
            if (v_pts == AV_NOPTS_VALUE)
                break;
            if (v_pts + skew_us >= a_pts)
                break;
            if (av_fifo_read(ds->video_q, &f, 1) < 0)
                break;
            av_frame_free(&f);
            ds->v_catchup++;
            ds->video_drops++;
            f = NULL;
        }
    }

    return dyn_q_pop_due_locked(ds->video_q, ds, now_us, delay_us, out_media_us);
}

static void dyn_sync_warn(AVFilterContext *ctx, DynamicInputContext *s,
                          DynSource *ds, int pad, const char *code,
                          int skew_ms, int64_t drops)
{
    int64_t now = av_gettime_relative();
    int interval = FFMAX(s->sync_warn_interval_ms, 1000);

    if (!code || pad < 0)
        return;
    if (now - ds->last_sync_warn_us < (int64_t)interval * 1000)
        return;
    ds->last_sync_warn_us = now;

    if (!strcmp(code, "AV_SKEW"))
        av_log(ctx, AV_LOG_WARNING,
               "dyn_sync: pad%d IMBALANCE code=AV_SKEW av_skew=%dms warn=%d | "
               "cause: source A/V media PTS skew exceeds threshold (not a joint jump) | "
               "fix: check pusher A/V clocks and discontinuity logs; "
               "do not raise amixrank max_latency to hide this\n",
               pad + 1, skew_ms, s->sync_warn_av_ms);
    else if (!strcmp(code, "DROP_A"))
        av_log(ctx, AV_LOG_WARNING,
               "dyn_sync: pad%d IMBALANCE code=DROP_A a_drop=%"PRId64" | "
               "cause: audio queue overflow — audio discarded (violates audio-master) | "
               "fix: check encode/RTMP backpressure and amixrank consume; "
               "reduce load; keep video following audio, never cut sound to chase video\n",
               pad + 1, drops);
    else if (!strcmp(code, "DROP_V"))
        av_log(ctx, AV_LOG_WARNING,
               "dyn_sync: pad%d IMBALANCE code=DROP_V v_drop=%"PRId64" catchup=%"PRId64" | "
               "cause: video queue overflow or heavy catch-up to audio master | "
               "fix: check decode/GPU; light catch-up is OK; if AV_SKEW also fires fix source\n",
               pad + 1, drops, ds->v_catchup);
    else if (!strcmp(code, "V_CATCHUP_HEAVY"))
        av_log(ctx, AV_LOG_WARNING,
               "dyn_sync: pad%d IMBALANCE code=V_CATCHUP_HEAVY catchup=%"PRId64" | "
               "cause: many video frames dropped to follow audio PTS | "
               "fix: video late vs audio — check network/decode; audio kept by design\n",
               pad + 1, ds->v_catchup);
    else
        av_log(ctx, AV_LOG_WARNING,
               "dyn_sync: pad%d IMBALANCE code=%s | cause: sync imbalance | "
               "fix: see docs/dynamic_input.md\n",
               pad + 1, code);
}

static void dyn_sync_check_skew(AVFilterContext *ctx, DynamicInputContext *s,
                                DynSource *ds, int pad)
{
    int64_t skew;
    int skew_ms, warn_ms;

    if (!ds->av_primed || s->sync_warn_av_ms <= 0)
        return;
    if (ds->play_a_media_us == AV_NOPTS_VALUE ||
        ds->play_v_media_us == AV_NOPTS_VALUE)
        return;
    skew = ds->play_a_media_us - ds->play_v_media_us;
    skew_ms = (int)(skew / 1000);
    if (skew_ms < 0)
        skew_ms = -skew_ms;
    warn_ms = s->sync_warn_av_ms;
    if (skew_ms <= warn_ms)
        return;
    dyn_sync_warn(ctx, s, ds, pad, "AV_SKEW", skew_ms, 0);
}

static void dyn_drop_queued(DynSource *ds)
{
    pthread_mutex_lock(&ds->lock);
    dyn_video_q_clear_locked(ds);
    dyn_audio_q_clear_locked(ds);
    av_frame_free(&ds->cur_hw);
    dyn_reset_av_sync(ds);
    pthread_mutex_unlock(&ds->lock);
}

/* Clear decode queues / sync but keep last picture for seamless URL swap. */
static void dyn_drop_queued_keep_picture(DynSource *ds)
{
    pthread_mutex_lock(&ds->lock);
    dyn_video_q_clear_locked(ds);
    dyn_audio_q_clear_locked(ds);
    dyn_reset_av_sync(ds);
    pthread_mutex_unlock(&ds->lock);
}

static void dyn_free_pending_open(DynSource *ds)
{
    if (!ds->pending_open)
        return;
    dyn_close_source(ds->pending_open);
    av_freep(&ds->pending_open);
}

static void dyn_close_codecs(DynSource *ds)
{
    if (ds->dec_ctx)
        avcodec_free_context(&ds->dec_ctx);
    if (ds->adec_ctx)
        avcodec_free_context(&ds->adec_ctx);
    swr_free(&ds->swr);
    if (ds->fmt_ctx)
        avformat_close_input(&ds->fmt_ctx);
    ds->video_stream_idx = -1;
    ds->audio_stream_idx = -1;
    ds->eof = 0;
    /* Rebind input -re anchor on next open; output media_frame_idx stays. */
    ds->have_in_anchor = 0;
    ds->in_pts0 = 0;
    ds->in_wall0 = 0;
    ds->is_realtime = 0;
}

static int dyn_url_is_realtime_scheme(const char *url)
{
    static const char *const schemes[] = {
        "rtmp:", "rtmps:", "rtsp:", "rtsps:", "srt:", "udp:", "rtp:",
        "http:", "https:", NULL
    };
    int i;
    if (!url)
        return 0;
    for (i = 0; schemes[i]; i++) {
        if (!av_strncasecmp(url, schemes[i], strlen(schemes[i])))
            return 1;
    }
    return 0;
}

static int dyn_detect_realtime(AVFormatContext *fmt, const char *url)
{
    if (!fmt)
        return dyn_url_is_realtime_scheme(url);
    if (fmt->iformat && (fmt->iformat->flags & AVFMT_NOFILE))
        return 1;
    if (fmt->pb && !fmt->pb->seekable)
        return 1;
    return dyn_url_is_realtime_scheme(url);
}

static int64_t dyn_frame_duration_us(const DynamicInputContext *s)
{
    return av_rescale_q(1, av_inv_q(s->frame_rate), AV_TIME_BASE_Q);
}

/* File + live=1: pace demux like ffmpeg -re. Realtime sources skip. */
static void dyn_pace_input(DynamicInputContext *s, DynSource *ds,
                           int64_t pts, AVRational tb)
{
    int64_t now, target, delay_us;

    if (!s->live || ds->is_realtime || pts == AV_NOPTS_VALUE || ds->stop)
        return;

    if (!ds->have_in_anchor) {
        ds->in_pts0 = pts;
        ds->in_wall0 = av_gettime_relative();
        ds->have_in_anchor = 1;
        return;
    }

    target = ds->in_wall0 + av_rescale_q(pts - ds->in_pts0, tb, AV_TIME_BASE_Q);
    now = av_gettime_relative();
    delay_us = target - now;
    if (delay_us <= 0)
        return;
    /* Cap runaway sleeps from bad timestamps. */
    if (delay_us > 10LL * 1000000)
        delay_us = 10LL * 1000000;
    while (delay_us > 0 && !ds->stop && s->live) {
        int64_t slice = FFMIN(delay_us, 50 * 1000);
        av_usleep(slice);
        delay_us -= slice;
    }
}

static void dyn_reanchor_wall(DynamicInputContext *s, int64_t now_us)
{
    s->wall_start_us = now_us - av_rescale_q(s->media_frame_idx,
                                              av_inv_q(s->frame_rate),
                                              AV_TIME_BASE_Q);
    s->next_tick_us = now_us;
    s->wall_started = 1;
}

static void dyn_close_source(DynSource *ds)
{
    av_freep(&ds->url);
    dyn_close_codecs(ds);
    av_packet_free(&ds->pkt);
    av_frame_free(&ds->sw_frame);
    av_frame_free(&ds->a_frame);
}

static int dyn_open_audio(AVFilterContext *ctx, DynSource *ds)
{
    DynamicInputContext *s = ctx->priv;
    const AVCodec *dec = NULL;
    AVCodecParameters *par;
    int ret, idx;

    idx = av_find_best_stream(ds->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (idx < 0) {
        /* Not fatal (video-only sources stay usable), but enable_audio=1 says
         * the caller expects sound — silence here looks like a mixer bug. */
        av_log(ctx, AV_LOG_WARNING,
               "dynamic_input: enable_audio=1 but '%s' has no audio stream — "
               "this pad contributes silence to the mix\n",
               ds->url ? ds->url : "?");
        ds->audio_stream_idx = -1;
        return 0;
    }
    ds->audio_stream_idx = idx;
    ds->adec_ctx = avcodec_alloc_context3(dec);
    if (!ds->adec_ctx)
        return AVERROR(ENOMEM);
    par = ds->fmt_ctx->streams[idx]->codecpar;
    ret = avcodec_parameters_to_context(ds->adec_ctx, par);
    if (ret < 0)
        return ret;
    ret = avcodec_open2(ds->adec_ctx, dec, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "dynamic_input: audio codec open failed\n");
        avcodec_free_context(&ds->adec_ctx);
        ds->audio_stream_idx = -1;
        return 0;
    }
    if (!ds->adec_ctx->ch_layout.nb_channels) {
        if (par->ch_layout.nb_channels) {
            ret = av_channel_layout_copy(&ds->adec_ctx->ch_layout, &par->ch_layout);
            if (ret < 0)
                return ret;
        } else {
            av_channel_layout_default(&ds->adec_ctx->ch_layout, 2);
        }
    }
    if (!ds->a_frame)
        ds->a_frame = av_frame_alloc();
    if (!ds->a_frame)
        return AVERROR(ENOMEM);

    ret = swr_alloc_set_opts2(&ds->swr,
                              &s->ch_layout, AV_SAMPLE_FMT_FLTP, s->sample_rate,
                              &ds->adec_ctx->ch_layout, ds->adec_ctx->sample_fmt,
                              ds->adec_ctx->sample_rate, 0, NULL);
    if (ret < 0)
        return ret;
    ret = swr_init(ds->swr);
    if (ret < 0) {
        swr_free(&ds->swr);
        return ret;
    }
    return 0;
}

static enum AVPixelFormat dyn_get_hw_format(AVCodecContext *avctx,
                                            const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

static int dyn_open_source(AVFilterContext *ctx, DynSource *ds, const char *url)
{
    DynamicInputContext *s = ctx->priv;
    const AVCodec *dec = NULL;
    AVCodecParameters *par;
    char *url_copy;
    int ret, idx;

    url_copy = av_strdup(url);
    if (!url_copy)
        return AVERROR(ENOMEM);

    dyn_close_codecs(ds);
    av_freep(&ds->url);
    ds->url = url_copy;

    ds->fmt_ctx = NULL;
    ds->got_media = 0;
    ds->read_fail_streak = 0;
    ds->have_in_anchor = 0;
    ds->is_realtime = 0;
    dyn_reset_av_sync(ds);
    {
        AVDictionary *opts = NULL;
        char timeout[32];
        /* Fail fast on dead RTMP so the fixed reconnect timer can fire. */
        snprintf(timeout, sizeof(timeout), "%d", FFMAX(s->reconnect_ms, 1000) * 1000);
        av_dict_set(&opts, "rw_timeout", timeout, 0);         /* us */
        av_dict_set(&opts, "stimeout", timeout, 0);           /* RTMP */
        av_dict_set(&opts, "rtmp_live", "live", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        ret = avformat_open_input(&ds->fmt_ctx, url, NULL, &opts);
        av_dict_free(&opts);
    }
    if (ret < 0) {
        int64_t now = av_gettime_relative();
        if (now - ds->last_reconnect_log_us > 5000000) {
            av_log(ctx, AV_LOG_WARNING,
                   "dynamic_input: open failed '%s': %s (retry every %dms)\n",
                   url, av_err2str(ret), s->reconnect_ms);
            ds->last_reconnect_log_us = now;
        }
        return ret;
    }
    /* Cap probe so a half-open RTMP cannot block the reconnect loop. */
    ds->fmt_ctx->max_analyze_duration = 2 * AV_TIME_BASE;
    ds->fmt_ctx->probesize = 1 * 1024 * 1024;
    ret = avformat_find_stream_info(ds->fmt_ctx, NULL);
    if (ret < 0)
        av_log(ctx, AV_LOG_WARNING, "dynamic_input: find_stream_info '%s'\n", url);

    idx = av_find_best_stream(ds->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (idx < 0) {
        av_log(ctx, AV_LOG_WARNING, "dynamic_input: no video stream '%s'\n", url);
        avformat_close_input(&ds->fmt_ctx);
        return idx;
    }
    ds->video_stream_idx = idx;

    ds->dec_ctx = avcodec_alloc_context3(dec);
    if (!ds->dec_ctx) {
        avformat_close_input(&ds->fmt_ctx);
        return AVERROR(ENOMEM);
    }
    par = ds->fmt_ctx->streams[idx]->codecpar;
    ret = avcodec_parameters_to_context(ds->dec_ctx, par);
    if (ret < 0) {
        avcodec_free_context(&ds->dec_ctx);
        avformat_close_input(&ds->fmt_ctx);
        return ret;
    }
    if (s->device_ref) {
        ds->dec_ctx->hw_device_ctx = av_buffer_ref(s->device_ref);
        if (!ds->dec_ctx->hw_device_ctx) {
            avcodec_free_context(&ds->dec_ctx);
            avformat_close_input(&ds->fmt_ctx);
            return AVERROR(ENOMEM);
        }
        /* Prefer CUDA frames from NVDEC when device is available. */
        ds->dec_ctx->get_format = dyn_get_hw_format;
    }
    ret = avcodec_open2(ds->dec_ctx, dec, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "dynamic_input: codec open failed '%s': %s\n",
               url, av_err2str(ret));
        avcodec_free_context(&ds->dec_ctx);
        avformat_close_input(&ds->fmt_ctx);
        return ret;
    }

    if (s->enable_audio) {
        ret = dyn_open_audio(ctx, ds);
        if (ret < 0)
            av_log(ctx, AV_LOG_WARNING, "dynamic_input: audio open failed for '%s'\n", url);
    }

    if (!ds->pkt)
        ds->pkt = av_packet_alloc();
    if (!ds->sw_frame)
        ds->sw_frame = av_frame_alloc();
    if (!ds->pkt || !ds->sw_frame)
        return AVERROR(ENOMEM);

    {
        int recovering = ds->ever_opened;
        ds->eof = 0;
        ds->ever_opened = 1;
        ds->is_realtime = dyn_detect_realtime(ds->fmt_ctx, url);
        ds->have_in_anchor = 0;
        av_log(ctx, AV_LOG_INFO,
               "dynamic_input: opened '%s' (codec=%s %dx%d audio=%d realtime=%d)%s\n",
               url, dec->name, ds->dec_ctx->width, ds->dec_ctx->height,
               ds->audio_stream_idx >= 0, ds->is_realtime,
               recovering ? " [recovered]" : "");
        /* Next decoded picture starts a new content generation. */
        ds->need_content_gen = 1;
    }
    ff_filter_set_ready(ctx, 100);
    return 0;
}

static int dyn_push_video_frame(AVFilterContext *ctx, DynamicInputContext *s,
                                DynSource *ds, AVFrame *src)
{
    AVFrame *hw = NULL;
    int ret;

    if (src->format == AV_PIX_FMT_CUDA) {
        hw = av_frame_clone(src);
        if (!hw)
            return AVERROR(ENOMEM);
    } else {
        hw = av_frame_alloc();
        if (!hw)
            return AVERROR(ENOMEM);
        ret = av_hwframe_get_buffer(s->frames_ctx, hw, 0);
        if (ret < 0) {
            av_frame_free(&hw);
            return ret;
        }
        ret = av_hwframe_transfer_data(hw, src, 0);
        if (ret < 0) {
            av_frame_free(&hw);
            return ret;
        }
        if (src->width > 0)  hw->width  = src->width;
        if (src->height > 0) hw->height = src->height;
        if (src->pts != AV_NOPTS_VALUE)
            hw->pts = src->pts;
    }

    av_dict_set_int(&hw->metadata, "lavfi.dyn_arrival_us",
                    av_gettime_relative(), 0);

    pthread_mutex_lock(&ds->lock);
    {
        int64_t media_us = dyn_stream_pts_us(ds, src->pts, 0);
        int ret_q;
        media_us = dyn_correct_media_pts(ctx, s, ds, 0, media_us);
        if (media_us != AV_NOPTS_VALUE)
            av_dict_set_int(&hw->metadata, "lavfi.dyn_media_pts_us", media_us, 0);
        if (ds->first_video_wall_us <= 0)
            ds->first_video_wall_us = av_gettime_relative();
        if (hw->pts != AV_NOPTS_VALUE)
            ds->last_pts = hw->pts;
        ret_q = dyn_video_q_push_locked(ds, hw);
        if (ret_q < 0) {
            pthread_mutex_unlock(&ds->lock);
            av_frame_free(&hw);
            return ret_q;
        }
        dyn_try_prime_av_locked(ctx, s, ds);
    }
    pthread_mutex_unlock(&ds->lock);
    if (!ds->got_media) {
        ds->got_media = 1;
        av_log(ctx, AV_LOG_INFO, "dynamic_input: media recovered '%s' (video)\n",
               ds->url ? ds->url : "?");
    }
    /* Wake filtergraph from decoder thread (do not busy-loop in activate). */
    ff_filter_set_ready(ctx, 100);
    return 0;
}

static int dyn_push_audio_frame(AVFilterContext *ctx, DynamicInputContext *s,
                                DynSource *ds, AVFrame *src)
{
    AVFrame *out;
    int64_t before, total_drops, media_us, now;
    int ret, dst_nb, log_drops = 0;

    if (!ds->swr || !s->enable_audio)
        return 0;

    dst_nb = swr_get_out_samples(ds->swr, src->nb_samples);
    if (dst_nb <= 0)
        return 0;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);
    out->format = AV_SAMPLE_FMT_FLTP;
    out->sample_rate = s->sample_rate;
    ret = av_channel_layout_copy(&out->ch_layout, &s->ch_layout);
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }
    out->nb_samples = dst_nb;
    ret = av_frame_get_buffer(out, 0);
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }

    ret = swr_convert_frame(ds->swr, out, src);
    if (ret < 0 || out->nb_samples <= 0) {
        av_frame_free(&out);
        return ret < 0 ? ret : 0;
    }
    /* Output PTS assigned in activate from ds->audio_samples_out — not input PTS.
     * Keep container media PTS for A/V prime / due timeline. */
    out->pts = AV_NOPTS_VALUE;
    now = av_gettime_relative();
    av_dict_set_int(&out->metadata, "lavfi.dyn_arrival_us", now, 0);

    pthread_mutex_lock(&ds->lock);
    media_us = dyn_stream_pts_us(ds, src->pts, 1);
    media_us = dyn_correct_media_pts(ctx, s, ds, 1, media_us);
    if (media_us != AV_NOPTS_VALUE)
        av_dict_set_int(&out->metadata, "lavfi.dyn_media_pts_us", media_us, 0);
    if (ds->first_audio_wall_us <= 0)
        ds->first_audio_wall_us = now;
    before = ds->audio_drops;
    ret = dyn_audio_q_push_locked(ds, out);
    total_drops = ds->audio_drops;
    if (total_drops > before) {
        int64_t now = av_gettime_relative();
        if (now - ds->last_drop_log_us > 5000000) {
            ds->last_drop_log_us = now;
            log_drops = 1;
        }
    }
    if (ret >= 0)
        dyn_try_prime_av_locked(ctx, s, ds);
    pthread_mutex_unlock(&ds->lock);
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }
    if (log_drops) {
        int pad = (int)(ds - s->srcs);
        dyn_sync_warn(ctx, s, ds, pad, "DROP_A", 0, total_drops);
    }
    if (!ds->got_media) {
        ds->got_media = 1;
        av_log(ctx, AV_LOG_INFO, "dynamic_input: media recovered '%s' (audio)\n",
               ds->url ? ds->url : "?");
    }
    ff_filter_set_ready(ctx, 100);
    return 0;
}

/* Fatal demux errors → reconnect. Transient ones keep the session. */
static int dyn_read_is_fatal(int err)
{
    if (err == AVERROR(EAGAIN) || err == AVERROR(EINTR))
        return 0;
    if (err == AVERROR_EOF || err == AVERROR_EXIT ||
        err == AVERROR(EIO) || err == AVERROR(EPIPE) ||
        err == AVERROR(ETIMEDOUT) || err == AVERROR(ECONNRESET) ||
        err == AVERROR(ENETDOWN) || err == AVERROR(ENETUNREACH) ||
        err == AVERROR(ECONNABORTED) || err == AVERROR_INVALIDDATA)
        return 1;
    /* Unknown negative: treat as fatal after streak (see caller). */
    return 1;
}

static int dyn_handle_packet(AVFilterContext *ctx, DynamicInputContext *s, DynSource *ds)
{
    int ret;

    ret = av_read_frame(ds->fmt_ctx, ds->pkt);
    if (ret < 0) {
        if (!dyn_read_is_fatal(ret)) {
            ds->read_fail_streak = 0;
            return ret;
        }
        /* Allow a few consecutive timeouts before tearing down (live jitter). */
        if (ret == AVERROR(ETIMEDOUT) || ret == AVERROR(EIO)) {
            ds->read_fail_streak++;
            if (ds->read_fail_streak < 3)
                return 0;
        }
        av_log(ctx, AV_LOG_WARNING,
               "dynamic_input: demux lost '%s': %s — will reconnect\n",
               ds->url ? ds->url : "?", av_err2str(ret));
        /* Drop last frames so mixing_cuda immediately shows fill (not freeze).
         * Never propagate EOF to outlinks — pads stay open for reconnect. */
        ds->eof = 1;
        ds->got_media = 0;
        ds->have_in_anchor = 0; /* rebind -re anchor after reconnect */
        ds->need_content_gen = 1;
        dyn_drop_queued(ds);
        ff_filter_set_ready(ctx, 100);
        return AVERROR_EOF;
    }
    ds->read_fail_streak = 0;

    if (ds->pkt->stream_index == ds->video_stream_idx ||
        (s->enable_audio && ds->adec_ctx &&
         ds->pkt->stream_index == ds->audio_stream_idx)) {
        AVStream *st = ds->fmt_ctx->streams[ds->pkt->stream_index];
        dyn_pace_input(s, ds, ds->pkt->pts != AV_NOPTS_VALUE ? ds->pkt->pts
                                                             : ds->pkt->dts,
                       st->time_base);
    }

    if (ds->pkt->stream_index == ds->video_stream_idx) {
        ret = avcodec_send_packet(ds->dec_ctx, ds->pkt);
        av_packet_unref(ds->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
        while ((ret = avcodec_receive_frame(ds->dec_ctx, ds->sw_frame)) == 0) {
            dyn_push_video_frame(ctx, s, ds, ds->sw_frame);
            av_frame_unref(ds->sw_frame);
        }
        return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
    }

    if (s->enable_audio && ds->adec_ctx &&
        ds->pkt->stream_index == ds->audio_stream_idx) {
        ret = avcodec_send_packet(ds->adec_ctx, ds->pkt);
        av_packet_unref(ds->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
        while ((ret = avcodec_receive_frame(ds->adec_ctx, ds->a_frame)) == 0) {
            dyn_push_audio_frame(ctx, s, ds, ds->a_frame);
            av_frame_unref(ds->a_frame);
        }
        return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
    }

    av_packet_unref(ds->pkt);
    return 0;
}

/* Interruptible sleep so stop/remove can join promptly. */
static void dyn_sleep_ms(DynSource *ds, int total_ms)
{
    while (total_ms > 0 && !ds->stop) {
        int slice = FFMIN(total_ms, 100);
        av_usleep(slice * 1000);
        total_ms -= slice;
    }
}

static void *dyn_source_thread(void *arg)
{
    struct { AVFilterContext *ctx; int idx; } *p = arg;
    AVFilterContext *ctx = p->ctx;
    DynamicInputContext *s = ctx->priv;
    DynSource *ds = &s->srcs[p->idx];
    int idx = p->idx;
    av_free(p);

    av_log(ctx, AV_LOG_INFO, "dynamic_input: reconnect loop started for v%d\n", idx);

    while (!ds->stop) {
        if (ds->pending_open) {
            /* Seamless update: new demux already opened on cmd thread. Keep
             * cur_hw so the mix holds the last frame until the first new one. */
            DynSource *po;
            pthread_mutex_lock(&s->url_lock);
            po = ds->pending_open;
            ds->pending_open = NULL;
            ds->pending_remove = 0;
            pthread_mutex_unlock(&s->url_lock);
            if (!po)
                continue;
            dyn_drop_queued_keep_picture(ds);
            dyn_close_codecs(ds);
            av_freep(&ds->url);
            dyn_adopt_opened(ds, po);
            av_free(po);
            ds->active = 1;
            ds->removed_us = 0;
            ds->eof = 0;
            av_log(ctx, AV_LOG_INFO,
                   "dynamic_input: hot-swap v%d -> '%s' (kept picture)\n",
                   idx, ds->url ? ds->url : "?");
            continue;
        }

        if (ds->pending_remove) {
            /* Soft remove: same teardown as disconnect, but drop URL and keep
             * the thread idle — later update reuses this loop (like recover). */
            ds->pending_remove = 0;
            dyn_free_pending_open(ds);
            dyn_drop_queued(ds);
            dyn_close_codecs(ds);
            pthread_mutex_lock(&ds->lock);
            av_freep(&ds->url);
            dyn_reset_av_sync(ds);
            ds->got_media = 0;
            ds->have_in_anchor = 0;
            ds->eof = 0;
            ds->need_content_gen = 1;
            ds->removed_us = av_gettime_relative();
            pthread_mutex_unlock(&ds->lock);
            av_log(ctx, AV_LOG_INFO, "dynamic_input: soft-removed v%d (thread idle)\n", idx);
            continue;
        }

        if (ds->force_reconnect) {
            /* Soft restart (same-URL): keep thread/url/active/cur_hw. */
            ds->force_reconnect = 0;
            dyn_drop_queued_keep_picture(ds);
            dyn_close_codecs(ds);
            pthread_mutex_lock(&ds->lock);
            ds->got_media = 0;
            ds->have_in_anchor = 0;
            ds->eof = 0;
            ds->need_content_gen = 1;
            pthread_mutex_unlock(&ds->lock);
            av_log(ctx, AV_LOG_INFO, "dynamic_input: soft reconnect v%d\n", idx);
        }

        if (!ds->fmt_ctx || ds->eof) {
            char *url = NULL;

            pthread_mutex_lock(&ds->lock);
            if (ds->url)
                url = av_strdup(ds->url);
            pthread_mutex_unlock(&ds->lock);

            if (!url) {
                /* Empty / soft-removed: idle until update assigns a URL.
                 * If cmd already attached an opened fmt_ctx (cold insert),
                 * fall through to demux without reopening. */
                if (ds->fmt_ctx && !ds->eof)
                    goto demux;
                dyn_sleep_ms(ds, 100);
                continue;
            }

            /* Configured URL must keep trying on a fixed timer. */
            dyn_drop_queued(ds);
            dyn_close_codecs(ds);
            if (dyn_open_source(ctx, ds, url) < 0) {
                av_free(url);
                dyn_sleep_ms(ds, s->reconnect_ms);
                continue;
            }
            av_free(url);
            /* Opened: fall through to demux immediately (no extra wait). */
        }

demux:
        if (dyn_handle_packet(ctx, s, ds) < 0) {
            if (ds->eof) {
                /* Disconnect → wait reconnect_ms then reopen (URL kept). */
                dyn_close_codecs(ds);
                dyn_sleep_ms(ds, s->reconnect_ms);
            } else {
                av_usleep(5 * 1000);
            }
        }
    }
    return NULL;
}

static int dyn_start_source(AVFilterContext *ctx, int idx)
{
    DynamicInputContext *s = ctx->priv;
    DynSource *ds = &s->srcs[idx];
    struct { AVFilterContext *ctx; int idx; } *p;

    if (ds->thread_started)
        return 0;
    ds->stop = 0;
    p = av_mallocz(sizeof(*p));
    if (!p)
        return AVERROR(ENOMEM);
    p->ctx = ctx;
    p->idx = idx;
    if (pthread_create(&ds->thread, NULL, dyn_source_thread, p)) {
        av_free(p);
        return AVERROR(errno);
    }
    ds->thread_started = 1;
    return 0;
}

static void dyn_stop_source(DynSource *ds)
{
    if (ds->thread_started) {
        ds->stop = 1;
        pthread_join(ds->thread, NULL);
        ds->thread_started = 0;
    }
    dyn_free_pending_open(ds);
    dyn_close_source(ds);
    pthread_mutex_lock(&ds->lock);
    dyn_video_q_clear_locked(ds);
    dyn_audio_q_clear_locked(ds);
    /* Keep cur_hw: after remove, still emit last frame so mixing/amixrank
     * see a soft gap instead of immediate dyn_empty black + layout thrash. */
    dyn_reset_av_sync(ds);
    pthread_mutex_unlock(&ds->lock);
}

static int dyn_parse_slot_range(const char *tok, int nb, int *lo, int *hi)
{
    int a, b;
    char *dash;

    if (!tok || !*tok || !lo || !hi)
        return AVERROR(EINVAL);
    dash = strchr(tok, '-');
    if (dash) {
        char left[32];
        size_t n = (size_t)(dash - tok);
        if (n == 0 || n >= sizeof(left))
            return AVERROR(EINVAL);
        memcpy(left, tok, n);
        left[n] = 0;
        a = atoi(left);
        b = atoi(dash + 1);
    } else {
        a = b = atoi(tok);
    }
    /* Command slots are 1..N (business pads). 0 = canvas bg — not used here. */
    if (a < 1 || b < 1 || a > nb || b > nb || a > b)
        return AVERROR(EINVAL);
    *lo = a;
    *hi = b;
    return 0;
}

static int dyn_cmd_search(AVFilterContext *ctx, const char *args, char *res, int res_len)
{
    DynamicInputContext *s = ctx->priv;
    int lo = 1, hi = s->nb_outputs, i, off = 0;

    if (args && *args) {
        char tok[64];
        if (sscanf(args, "%63s", tok) != 1)
            return AVERROR(EINVAL);
        if (dyn_parse_slot_range(tok, s->nb_outputs, &lo, &hi) < 0)
            return AVERROR(EINVAL);
    }

    pthread_mutex_lock(&s->url_lock);
    for (i = lo; i <= hi; i++) {
        DynSource *ds = &s->srcs[i - 1];
        const char *url = ds->url && *ds->url ? ds->url : "-";
        const char *st = !ds->url ? "empty"
                        : (!ds->thread_started ? "configured"
                        : (ds->got_media ? "live" : "opening"));
        off += snprintf(res + off, res_len > off ? res_len - off : 0,
                        "%d:%s:%s:%d ", i, url, st, ds->active);
        if (off >= res_len - 1)
            break;
    }
    pthread_mutex_unlock(&s->url_lock);
    if (off == 0 && res_len > 0)
        snprintf(res, res_len, "(none)");
    return 0;
}

static int dyn_cmd_purge(AVFilterContext *ctx)
{
    DynamicInputContext *s = ctx->priv;
    int i;

    pthread_mutex_lock(&s->url_lock);
    for (i = 0; i < s->nb_outputs; i++) {
        DynSource *ds = &s->srcs[i];
        ds->active = 0;
        if (ds->thread_started)
            ds->pending_remove = 1;
        else {
            av_freep(&ds->url);
            dyn_close_codecs(ds);
            dyn_reset_av_sync(ds);
            ds->removed_us = av_gettime_relative();
        }
    }
    pthread_mutex_unlock(&s->url_lock);
    av_log(ctx, AV_LOG_INFO, "dynamic_input: soft-purge all slots\n");
    ff_filter_set_ready(ctx, 100);
    return 0;
}

/* Move a successfully opened (no thread) source into a stopped slot. */
static void dyn_adopt_opened(DynSource *dst, DynSource *src)
{
    av_freep(&dst->url);
    dst->url = src->url;
    src->url = NULL;
    dst->fmt_ctx = src->fmt_ctx;
    src->fmt_ctx = NULL;
    dst->dec_ctx = src->dec_ctx;
    src->dec_ctx = NULL;
    dst->adec_ctx = src->adec_ctx;
    src->adec_ctx = NULL;
    dst->swr = src->swr;
    src->swr = NULL;
    dst->video_stream_idx = src->video_stream_idx;
    dst->audio_stream_idx = src->audio_stream_idx;
    dst->pkt = src->pkt;
    src->pkt = NULL;
    dst->sw_frame = src->sw_frame;
    src->sw_frame = NULL;
    dst->a_frame = src->a_frame;
    src->a_frame = NULL;
    dst->eof = 0;
    dst->ever_opened = src->ever_opened;
    dst->got_media = 0;
    dst->read_fail_streak = 0;
    dst->is_realtime = src->is_realtime;
    dst->have_in_anchor = 0;
    /* Keep last picture until first new decode; arm gen for that picture. */
    dst->need_content_gen = 1;
    dyn_reset_av_sync(dst);
}

/* Wait until soft-remove finished (thread idle, no URL/fmt). */
static int dyn_wait_slot_idle(DynSource *ds, int timeout_ms)
{
    int64_t deadline = av_gettime_relative() + (int64_t)timeout_ms * 1000;
    while (av_gettime_relative() < deadline) {
        if (!ds->pending_remove && !ds->url && !ds->fmt_ctx)
            return 0;
        av_usleep(5 * 1000);
    }
    return AVERROR(ETIMEDOUT);
}

/*
 * Stage a pre-opened source onto a slot. Does not block: live demux thread
 * hot-swaps and recycles the old fmt/codecs; cur_hw is kept until new frames.
 */
static int dyn_cmd_stage_opened(AVFilterContext *ctx, int slot_1based,
                                DynSource *tmp)
{
    DynamicInputContext *s = ctx->priv;
    DynSource *ds = &s->srcs[slot_1based - 1];
    DynSource *po;
    int idx = slot_1based - 1;
    int ret, cold;

    pthread_mutex_lock(&s->url_lock);
    ds->pending_remove = 0;
    ds->active = 1;

    if (!ds->thread_started || (!ds->fmt_ctx && !ds->url && !ds->pending_open)) {
        cold = !ds->url && !ds->fmt_ctx;
        dyn_free_pending_open(ds);
        dyn_adopt_opened(ds, tmp);
        ds->removed_us = 0;
        ret = dyn_start_source(ctx, idx);
        pthread_mutex_unlock(&s->url_lock);
        av_log(ctx, AV_LOG_INFO,
               "dynamic_input: update slot %d -> '%s'%s\n",
               slot_1based, ds->url ? ds->url : "?",
               cold ? " (cold insert)" : "");
        return ret;
    }

    po = av_mallocz(sizeof(*po));
    if (!po) {
        pthread_mutex_unlock(&s->url_lock);
        return AVERROR(ENOMEM);
    }
    *po = *tmp;
    memset(tmp, 0, sizeof(*tmp));
    tmp->video_stream_idx = -1;
    tmp->audio_stream_idx = -1;
    dyn_free_pending_open(ds);
    ds->pending_open = po;
    pthread_mutex_unlock(&s->url_lock);
    av_log(ctx, AV_LOG_INFO,
           "dynamic_input: update slot %d staged for hot-swap '%s'\n",
           slot_1based, po->url ? po->url : "?");
    return 0;
}

static void *dyn_open_one_thread(void *arg)
{
    DynOpenOneArg *a = arg;

    memset(&a->opened, 0, sizeof(a->opened));
    a->opened.video_stream_idx = -1;
    a->opened.audio_stream_idx = -1;
    a->ret = dyn_open_source(a->ctx, &a->opened, a->url);
    if (a->ret < 0)
        dyn_close_source(&a->opened);
    return NULL;
}

static void *dyn_open_worker(void *arg)
{
    AVFilterContext *ctx = arg;
    DynamicInputContext *s = ctx->priv;

    while (1) {
        DynOpenBatch *batch;
        DynOpenOneArg ones[DYN_INPUT_MAX];
        pthread_t th[DYN_INPUT_MAX];
        int i, n, ok = 0;
        unsigned gen;

        pthread_mutex_lock(&s->open_mu);
        while (!s->open_batch && !s->open_thr_stop)
            pthread_cond_wait(&s->open_cv, &s->open_mu);
        if (s->open_thr_stop && !s->open_batch) {
            pthread_mutex_unlock(&s->open_mu);
            break;
        }
        batch = s->open_batch;
        s->open_batch = NULL;
        pthread_mutex_unlock(&s->open_mu);
        if (!batch)
            continue;

        gen = batch->gen;
        n = batch->n;
        av_log(ctx, AV_LOG_INFO,
               "dynamic_input: async open start n=%d gen=%u\n", n, gen);

        /* Parallel open — graph thread is not blocked. */
        {
            int thr_ok[DYN_INPUT_MAX];
            for (i = 0; i < n; i++) {
                thr_ok[i] = 0;
                ones[i].ctx = ctx;
                av_strlcpy(ones[i].url, batch->urls[i], sizeof(ones[i].url));
                ones[i].ret = AVERROR(EINVAL);
                memset(&ones[i].opened, 0, sizeof(ones[i].opened));
                ones[i].opened.video_stream_idx = -1;
                ones[i].opened.audio_stream_idx = -1;
                if (!pthread_create(&th[i], NULL, dyn_open_one_thread, &ones[i]))
                    thr_ok[i] = 1;
                else
                    ones[i].ret = AVERROR(errno);
            }
            for (i = 0; i < n; i++) {
                if (thr_ok[i])
                    pthread_join(th[i], NULL);
            }
        }

        /* Stage only if this batch is still current; else discard opens. */
        for (i = 0; i < n; i++) {
            int cancelled;
            pthread_mutex_lock(&s->open_mu);
            cancelled = (gen != s->update_gen);
            pthread_mutex_unlock(&s->open_mu);
            if (cancelled) {
                dyn_close_source(&ones[i].opened);
                continue;
            }
            if (ones[i].ret < 0) {
                av_log(ctx, AV_LOG_WARNING,
                       "dynamic_input: async open slot %d failed '%s': %s\n",
                       batch->slots[i], batch->urls[i],
                       av_err2str(ones[i].ret));
                continue;
            }
            if (dyn_cmd_stage_opened(ctx, batch->slots[i], &ones[i].opened) >= 0)
                ok++;
            dyn_close_source(&ones[i].opened);
        }

        {
            int cancelled;
            unsigned now_gen;
            pthread_mutex_lock(&s->open_mu);
            now_gen = s->update_gen;
            cancelled = (gen != now_gen);
            pthread_mutex_unlock(&s->open_mu);
            if (!cancelled)
                av_log(ctx, AV_LOG_INFO,
                       "dynamic_input: async open done gen=%u staged=%d/%d\n",
                       gen, ok, n);
            else
                av_log(ctx, AV_LOG_INFO,
                       "dynamic_input: async open cancelled gen=%u (now %u)\n",
                       gen, now_gen);
        }

        av_free(batch);
        ff_filter_set_ready(ctx, 100);
    }
    return NULL;
}

static int dyn_open_worker_start(AVFilterContext *ctx)
{
    DynamicInputContext *s = ctx->priv;

    if (s->open_thr_started)
        return 0;
    s->open_thr_stop = 0;
    s->open_batch = NULL;
    s->update_gen = 1;
    pthread_mutex_init(&s->open_mu, NULL);
    pthread_cond_init(&s->open_cv, NULL);
    if (pthread_create(&s->open_thr, NULL, dyn_open_worker, ctx)) {
        pthread_cond_destroy(&s->open_cv);
        pthread_mutex_destroy(&s->open_mu);
        return AVERROR(errno);
    }
    s->open_thr_started = 1;
    return 0;
}

static void dyn_open_worker_stop(DynamicInputContext *s)
{
    if (!s->open_thr_started)
        return;
    pthread_mutex_lock(&s->open_mu);
    s->open_thr_stop = 1;
    s->update_gen++;
    if (s->open_batch) {
        av_free(s->open_batch);
        s->open_batch = NULL;
    }
    pthread_cond_signal(&s->open_cv);
    pthread_mutex_unlock(&s->open_mu);
    pthread_join(s->open_thr, NULL);
    s->open_thr_started = 0;
    pthread_cond_destroy(&s->open_cv);
    pthread_mutex_destroy(&s->open_mu);
}

/*
 * update <slot|lo-hi> <url> [url2 ...]
 * Queues async opens on a side thread; returns immediately so the mix does
 * not stall. When opens finish, hot-swap keeps last picture then recycles old.
 */
static int dyn_cmd_update(AVFilterContext *ctx, const char *args)
{
    DynamicInputContext *s = ctx->priv;
    char slot_tok[64];
    char urlbuf[DYN_INPUT_MAX][1024];
    DynOpenBatch *batch;
    int lo, hi, nurl = 0, nslot, i, nwork = 0, unchanged = 0;
    const char *p;
    int ret;

    if (!args || !*args)
        return AVERROR(EINVAL);
    ret = dyn_open_worker_start(ctx);
    if (ret < 0)
        return ret;

    p = args;
    while (*p == ' ' || *p == '\t')
        p++;
    if (sscanf(p, "%63s", slot_tok) != 1)
        return AVERROR(EINVAL);
    while (*p && *p != ' ' && *p != '\t')
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (dyn_parse_slot_range(slot_tok, s->nb_outputs, &lo, &hi) < 0)
        return AVERROR(EINVAL);

    while (*p && nurl < DYN_INPUT_MAX) {
        if (sscanf(p, "%1023s", urlbuf[nurl]) != 1)
            break;
        nurl++;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    if (nurl < 1)
        return AVERROR(EINVAL);

    batch = av_mallocz(sizeof(*batch));
    if (!batch)
        return AVERROR(ENOMEM);
    batch->ctx = ctx;
    nslot = hi - lo + 1;

    for (i = 0; i < nslot; i++) {
        const char *url = urlbuf[i < nurl ? i : nurl - 1];
        DynSource *ds = &s->srcs[lo + i - 1];

        pthread_mutex_lock(&s->url_lock);
        if (ds->url && !strcmp(ds->url, url) && ds->thread_started &&
            !ds->pending_remove && ds->fmt_ctx && !ds->eof) {
            ds->active = 1;
            pthread_mutex_unlock(&s->url_lock);
            unchanged++;
            av_log(ctx, AV_LOG_INFO,
                   "dynamic_input: update slot %d unchanged '%s'\n",
                   lo + i, url);
            continue;
        }
        pthread_mutex_unlock(&s->url_lock);
        batch->slots[nwork] = lo + i;
        av_strlcpy(batch->urls[nwork], url, sizeof(batch->urls[nwork]));
        nwork++;
    }

    if (nwork == 0) {
        av_free(batch);
        av_log(ctx, AV_LOG_INFO,
               "dynamic_input: update slots %d-%d all unchanged\n", lo, hi);
        return 0;
    }

    batch->n = nwork;
    pthread_mutex_lock(&s->open_mu);
    s->update_gen++;
    batch->gen = s->update_gen;
    if (s->open_batch)
        av_free(s->open_batch); /* cancel queued (in-flight checks gen) */
    s->open_batch = batch;
    {
        unsigned gen = batch->gen;
        pthread_cond_signal(&s->open_cv);
        pthread_mutex_unlock(&s->open_mu);
        av_log(ctx, AV_LOG_INFO,
               "dynamic_input: update slots %d-%d queued async open n=%d "
               "unchanged=%d gen=%u\n",
               lo, hi, nwork, unchanged, gen);
    }
    return 0;
}

static int dyn_cmd_remove_slots(AVFilterContext *ctx, const char *args)
{
    DynamicInputContext *s = ctx->priv;
    char slot_tok[64], url[1024];
    int lo, hi, i, n, matched = 0;

    if (!args || !*args)
        return AVERROR(EINVAL);
    n = sscanf(args, "%63s %1023s", slot_tok, url);
    if (n < 1)
        return AVERROR(EINVAL);
    if (dyn_parse_slot_range(slot_tok, s->nb_outputs, &lo, &hi) < 0)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&s->url_lock);
    for (i = lo; i <= hi; i++) {
        DynSource *ds = &s->srcs[i - 1];
        if (n >= 2) {
            if (!ds->url || strcmp(ds->url, url))
                continue;
        }
        /* Soft remove: keep demux thread idle (like disconnect). Do NOT join. */
        ds->active = 0;
        if (ds->thread_started) {
            ds->pending_remove = 1;
        } else {
            av_freep(&ds->url);
            dyn_close_codecs(ds);
            dyn_reset_av_sync(ds);
            ds->removed_us = av_gettime_relative();
        }
        matched++;
    }
    pthread_mutex_unlock(&s->url_lock);

    if (!matched && n >= 2)
        return AVERROR(ENOENT);
    av_log(ctx, AV_LOG_INFO,
           "dynamic_input: soft-remove slots %d-%d%s%s matched=%d\n",
           lo, hi, n >= 2 ? " url=" : "", n >= 2 ? url : "", matched);
    ff_filter_set_ready(ctx, 100);
    return 0;
}

static int dynamic_input_process_command(AVFilterContext *ctx, const char *cmd,
                                         const char *args, char *res,
                                         int res_len, int flags)
{
    int ret;

    av_log(ctx, AV_LOG_WARNING,
           "dynamic_input: cmd='%s' args='%s'\n", cmd, args ? args : "");

    /* Slot indices in commands are 1..N (v1..vN). 0=background, ignored. */
    if (!strcmp(cmd, "search"))
        return dyn_cmd_search(ctx, args, res, res_len);
    if (!strcmp(cmd, "purge") || !strcmp(cmd, "purage"))
        return dyn_cmd_purge(ctx);
    if (!strcmp(cmd, "update") && args)
        return dyn_cmd_update(ctx, args);
    if (!strcmp(cmd, "remove") && args)
        return dyn_cmd_remove_slots(ctx, args);
    if (!strcmp(cmd, "live") && args) {
        DynamicInputContext *s = ctx->priv;
        int val = atoi(args);
        s->live = val ? 1 : 0;
        s->wall_started = 0;
        av_log(ctx, AV_LOG_WARNING,
               "dynamic_input: live=%d (media_frame_idx=%"PRId64")\n",
               s->live, s->media_frame_idx);
        ff_filter_set_ready(ctx, 100);
        return 0;
    }
    if (!strcmp(cmd, "set_active") && args) {
        DynamicInputContext *s = ctx->priv;
        char tok[64];
        int lo, hi, val, slot;
        if (sscanf(args, "%63s %d", tok, &val) != 2) {
            av_log(ctx, AV_LOG_ERROR,
                   "dynamic_input: set_active bad args='%s' "
                   "(need: <slot|lo-hi> <0|1>)\n", args);
            return AVERROR(EINVAL);
        }
        if (dyn_parse_slot_range(tok, s->nb_outputs, &lo, &hi) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "dynamic_input: set_active bad range='%s' (business 1..%d)\n",
                   tok, s->nb_outputs);
            return AVERROR(EINVAL);
        }
        pthread_mutex_lock(&s->url_lock);
        for (slot = lo; slot <= hi; slot++)
            s->srcs[slot - 1].active = val ? 1 : 0;
        pthread_mutex_unlock(&s->url_lock);
        av_log(ctx, AV_LOG_WARNING,
               "dynamic_input: set_active slots %d-%d = %d\n",
               lo, hi, val ? 1 : 0);
        return 0;
    }
    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        av_log(ctx, AV_LOG_ERROR,
               "dynamic_input: unsupported/failed cmd='%s' args='%s' -> %s (%d)\n",
               cmd, args ? args : "", av_err2str(ret), ret);
    else
        av_log(ctx, AV_LOG_WARNING,
               "dynamic_input: runtime opt '%s' applied args='%s'\n",
               cmd, args ? args : "");
    return ret;
}

static av_cold int dynamic_input_init(AVFilterContext *ctx)
{
    DynamicInputContext *s = ctx->priv;
    int i, ret;

    s->sw_format = AV_PIX_FMT_NV12;
    if (s->sw_format_str && *s->sw_format_str) {
        s->sw_format = av_get_pix_fmt(s->sw_format_str);
        if (s->sw_format != AV_PIX_FMT_NV12) {
            av_log(ctx, AV_LOG_ERROR, "dynamic_input: only sw_format=nv12 is supported\n");
            return AVERROR(EINVAL);
        }
    }
    s->time_base = av_inv_q(s->frame_rate);
    av_channel_layout_default(&s->ch_layout, 2);
    pthread_mutex_init(&s->url_lock, NULL);
    if ((ret = dyn_open_worker_start(ctx)) < 0)
        return ret;

    for (i = 0; i < s->nb_outputs; i++) {
        AVFilterPad pad = { 0 };
        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name  = av_asprintf("v%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);
        pad.config_props = dynamic_input_config_video;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
        pthread_mutex_init(&s->srcs[i].lock, NULL);
        s->srcs[i].video_stream_idx = -1;
        s->srcs[i].audio_stream_idx = -1;
        s->srcs[i].active = 1;
        s->srcs[i].video_q = av_fifo_alloc2(dyn_video_q_depth(s),
                                            sizeof(AVFrame *), 0);
        if (!s->srcs[i].video_q)
            return AVERROR(ENOMEM);
    }

    if (s->enable_audio) {
        for (i = 0; i < s->nb_outputs; i++) {
            AVFilterPad pad = { 0 };
            pad.type = AVMEDIA_TYPE_AUDIO;
            pad.name  = av_asprintf("a%d", i);
            if (!pad.name)
                return AVERROR(ENOMEM);
            pad.config_props = dynamic_input_config_audio;
            if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
                return ret;
            s->srcs[i].audio_q = av_fifo_alloc2(dyn_audio_q_depth(s),
                                                sizeof(AVFrame *), 0);
            if (!s->srcs[i].audio_q)
                return AVERROR(ENOMEM);
        }
    }

    av_log(ctx, AV_LOG_WARNING,
           "dynamic_input: outputs=%d enable_audio=%d live=%d "
           "playout_delay_ms=%d av_wait_ms=%d pts_jump_s=%.1f "
           "(shared PTS timeline A/V; correct jumps >= pts_jump_s)\n",
           s->nb_outputs, s->enable_audio, s->live,
           s->playout_delay_ms, s->av_wait_ms, s->pts_jump_s);

    if (s->initial_urls && *s->initial_urls) {
        char *dup = av_strdup(s->initial_urls);
        char *save = NULL, *tok;
        int slot = 0;
        if (!dup)
            return AVERROR(ENOMEM);
        for (tok = av_strtok(dup, ",", &save); tok; tok = av_strtok(NULL, ",", &save)) {
            while (*tok == ' ') tok++;
            if (!*tok)
                continue;
            if (slot >= s->nb_outputs)
                break;
            s->srcs[slot].url = av_strdup(tok);
            s->srcs[slot].active = 1;
            slot++;
        }
        av_free(dup);
    }

    return 0;
}

static int dynamic_input_config_video(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DynamicInputContext *s = ctx->priv;
    FilterLink *l = ff_filter_link(outlink);
    AVHWDeviceContext *device_ctx;
    int ret, i;

    if (!s->device_ref) {
        if (ctx->hw_device_ctx) {
            s->device_ref = ctx->hw_device_ctx;
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", s->device_idx);
            ret = av_hwdevice_ctx_create(&s->own_device, AV_HWDEVICE_TYPE_CUDA, buf, NULL, 0);
            if (ret < 0)
                return ret;
            s->device_ref = s->own_device;
        }
        device_ctx = (AVHWDeviceContext *)s->device_ref->data;
        s->hwctx = device_ctx->hwctx;
    }

    if (s->size_str) {
        int ret2 = av_parse_video_size(&s->out_w, &s->out_h, s->size_str);
        if (ret2 < 0)
            return ret2;
    }
    if (s->out_w <= 0) s->out_w = 1920;
    if (s->out_h <= 0) s->out_h = 1080;

    if (!s->frames_ctx) {
        ret = dyn_alloc_hw_frames(ctx, s->out_w, s->out_h);
        if (ret < 0)
            return ret;
    }

    outlink->w = s->out_w;
    outlink->h = s->out_h;
    outlink->time_base = s->time_base;
    l->frame_rate = s->frame_rate;
    av_buffer_unref(&l->hw_frames_ctx);
    l->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!l->hw_frames_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < s->nb_outputs; i++) {
        if (s->srcs[i].url && !s->srcs[i].thread_started)
            dyn_start_source(ctx, i);
    }
    return 0;
}

static int dynamic_input_config_audio(AVFilterLink *outlink)
{
    DynamicInputContext *s = outlink->src->priv;

    outlink->sample_rate = s->sample_rate;
    outlink->time_base = (AVRational){1, s->sample_rate};
    return av_channel_layout_copy(&outlink->ch_layout, &s->ch_layout);
}

/*
 * FrameSync waits until EVERY input has have_next or EOF before advancing.
 * Empty/dead RTMP pads must still emit frames, or color_cuda+mixing stalls.
 * Placeholders carry metadata lavfi.dyn_empty=1; mixing_cuda skips blit (fill).
 * Output PTS always comes from filter wall-clock tick (never input stream PTS).
 */
static AVFrame *dyn_make_video_placeholder(DynamicInputContext *s, int64_t pts)
{
    AVFrame *hw;
    int ret;

    if (!s->frames_ctx)
        return NULL;
    hw = av_frame_alloc();
    if (!hw)
        return NULL;
    ret = av_hwframe_get_buffer(s->frames_ctx, hw, 0);
    if (ret < 0) {
        av_frame_free(&hw);
        return NULL;
    }
    hw->width  = s->out_w;
    hw->height = s->out_h;
    hw->pts    = pts;
    av_dict_set(&hw->metadata, "lavfi.dyn_empty", "1", 0);
    return hw;
}

/*
 * Audio pads: real decoded audio only (no silence placeholders).
 * PTS from a per-pad monotonic sample counter — independent of video ticks and
 * of the other pads.
 *
 * Push while downstream wants a frame. Also prime at most one frame when the
 * link fifo is empty — otherwise amixrank may never see data to wake up, the
 * decoder-side queue overflows, and the mix stays silent forever.
 * Never dump the whole audio_q in one go (that floods amixrank and causes the
 * same "audio queue full" death spiral).
 */
static void dyn_serve_audio(AVFilterContext *ctx, DynamicInputContext *s)
{
    int i;
    int64_t delay_us = dyn_playout_delay_us(s);

    for (i = 0; i < s->nb_outputs; i++) {
        AVFilterLink *outlink = ctx->outputs[s->nb_outputs + i];
        DynSource *ds = &s->srcs[i];
        int primed = 0;

        for (;;) {
            AVFrame *out = NULL;
            int nb, wanted, prime;
            int64_t now;

            wanted = ff_outlink_frame_wanted(outlink);
            prime  = !wanted && !primed && ff_inlink_queued_frames(outlink) < 1;
            if (!wanted && !prime)
                break;

            now = av_gettime_relative();
            pthread_mutex_lock(&ds->lock);
            if (ds->active) {
                int64_t media_us = AV_NOPTS_VALUE;
                dyn_try_prime_av_locked(ctx, s, ds);
                out = dyn_audio_q_pop_due_locked(ds, now, delay_us, &media_us);
                if (out && media_us != AV_NOPTS_VALUE)
                    ds->play_a_media_us = media_us;
            }
            pthread_mutex_unlock(&ds->lock);

            if (!out)
                break; /* not due yet, or empty — decoder/activate will retry */

            nb = out->nb_samples;
            out->pts = ds->audio_samples_out;
            av_dict_free(&out->metadata);
            /* ff_filter_frame consumes the reference even on failure. */
            if (ff_filter_frame(outlink, out) < 0)
                break;
            ds->audio_samples_out += nb;
            dyn_sync_check_skew(ctx, s, ds, i);
            if (prime) {
                primed = 1;
                break;
            }
        }
    }
}

static int dynamic_input_activate(AVFilterContext *ctx)
{
    DynamicInputContext *s = ctx->priv;
    int i, video_wanted = 0, audio_wanted = 0, video_pushed = 0, need_drain = 0;
    int64_t tick_pts;

    for (i = 0; i < s->nb_outputs; i++) {
        DynSource *ds = &s->srcs[i];
        if (ff_outlink_frame_wanted(ctx->outputs[i]))
            video_wanted = 1;
        /* Decoded video waiting — wake to drain into cur_hw even if FrameSync
         * is not pulling (EXT_INFINITY); otherwise thumbs freeze on old pts. */
        pthread_mutex_lock(&ds->lock);
        if (ds->video_q && av_fifo_can_read(ds->video_q) > 0)
            need_drain = 1;
        pthread_mutex_unlock(&ds->lock);
    }
    if (s->enable_audio) {
        for (i = 0; i < s->nb_outputs; i++) {
            DynSource *ds = &s->srcs[i];
            if (ff_outlink_frame_wanted(ctx->outputs[s->nb_outputs + i]))
                audio_wanted = 1;
            /* Have decoded audio waiting — activate to prime the link even
             * before the first downstream request, or the queue just drops. */
            pthread_mutex_lock(&ds->lock);
            if (ds->audio_q && av_fifo_can_read(ds->audio_q) > 0)
                audio_wanted = 1;
            pthread_mutex_unlock(&ds->lock);
        }
    }
    if (!video_wanted && !audio_wanted && !need_drain)
        return FFERROR_NOT_READY;

    if (s->enable_audio)
        dyn_serve_audio(ctx, s);

    /*
     * Wall-clock gate (live=1) for VIDEO only.
     * Never sleep the full frame duration inside one activate — that stalls
     * the whole filtergraph (amixrank cannot pull audio). Sleep a short slice,
     * serve audio, yield; the graph will reschedule us until the tick is due.
     */
    if (s->live && video_wanted) {
        int64_t now = av_gettime_relative();
        int64_t dur = dyn_frame_duration_us(s);
        if (!s->wall_started) {
            s->wall_start_us = now;
            s->next_tick_us = now;
            s->wall_started = 1;
        }
        if (now < s->next_tick_us) {
            int64_t delay = FFMIN(s->next_tick_us - now, 5000);
            av_usleep(delay);
            if (s->enable_audio)
                dyn_serve_audio(ctx, s);
            now = av_gettime_relative();
            if (now < s->next_tick_us) {
                ff_filter_set_ready(ctx, 10);
                return 0;
            }
        }
        if (now > s->next_tick_us + 2 * dur)
            dyn_reanchor_wall(s, now);
    }

    /* Drain due video into cur_hw for every pad (even if not pushing this
     * tick). Catch up backlog so thumbs do not stick on a stale FrameSync pts. */
    {
        int64_t delay_us = dyn_playout_delay_us(s);
        int64_t skew_us = (int64_t)FFMAX(s->sync_warn_av_ms, 1) * 1000;
        int64_t now = av_gettime_relative();
        for (i = 0; i < s->nb_outputs; i++) {
            DynSource *ds = &s->srcs[i];
            AVFrame *fresh;
            int64_t catchup_before;
            pthread_mutex_lock(&ds->lock);
            catchup_before = ds->v_catchup;
            if (ds->active) {
                int64_t media_us;
                dyn_try_prime_av_locked(ctx, s, ds);
                while ((fresh = dyn_video_q_pop_follow_audio_locked(ds, now, delay_us,
                                                                   skew_us, &media_us))) {
                    av_frame_free(&ds->cur_hw);
                    ds->cur_hw = fresh;
                    ds->cur_seq++;
                    if (media_us != AV_NOPTS_VALUE)
                        ds->play_v_media_us = media_us;
                    if (ds->need_content_gen) {
                        ds->content_gen++;
                        ds->need_content_gen = 0;
                        av_log(ctx, AV_LOG_INFO,
                               "dynamic_input: content_gen=%"PRIu64" on v%d\n",
                               ds->content_gen, i);
                    }
                }
            }
            if (!ds->active && ds->cur_hw && ds->removed_us > 0 &&
                now - ds->removed_us > 800 * 1000LL)
                av_frame_free(&ds->cur_hw);
            if (ds->v_catchup - catchup_before >= 8)
                dyn_sync_warn(ctx, s, ds, i, "V_CATCHUP_HEAVY", 0, 0);
            pthread_mutex_unlock(&ds->lock);
            dyn_sync_check_skew(ctx, s, ds, i);
        }
    }

    /* video pads: same program PTS on this tick; push cur_hw / placeholder. */
    if (video_wanted) {
        tick_pts = s->media_frame_idx;
        for (i = 0; i < s->nb_outputs; i++) {
            AVFilterLink *outlink = ctx->outputs[i];
            DynSource *ds = &s->srcs[i];
            AVFrame *out = NULL;
            int is_hold;

            if (!ff_outlink_frame_wanted(outlink))
                continue;

            pthread_mutex_lock(&ds->lock);
            if (ds->cur_hw)
                out = av_frame_clone(ds->cur_hw);
            is_hold = !out || ds->cur_seq == ds->pushed_seq;
            if (out && !is_hold)
                ds->pushed_seq = ds->cur_seq;
            pthread_mutex_unlock(&ds->lock);

            if (out) {
                out->pts = tick_pts;
                av_dict_set(&out->metadata, "lavfi.dyn_empty", NULL, 0);
                if (is_hold) {
                    av_dict_set(&out->metadata, "lavfi.dyn_hold", "1", 0);
                    av_dict_set(&out->metadata, "lavfi.dyn_gen", NULL, 0);
                } else {
                    av_dict_set(&out->metadata, "lavfi.dyn_hold", NULL, 0);
                    /* Stable across frames of the same open/hot-swap/reconnect. */
                    av_dict_set_int(&out->metadata, "lavfi.dyn_gen",
                                    (int64_t)ds->content_gen, 0);
                }
                av_frame_remove_side_data(out, AV_FRAME_DATA_REGIONS_OF_INTEREST);
            } else {
                out = dyn_make_video_placeholder(s, tick_pts);
                if (!out)
                    return AVERROR(ENOMEM);
            }

            if (ff_filter_frame(outlink, out) < 0) {
                /* out owned/freed by filtergraph on failure paths vary; keep cur_hw */
            } else {
                video_pushed++;
            }
        }

        if (video_pushed > 0) {
            s->media_frame_idx++;
            if (s->live) {
                s->next_tick_us = s->wall_start_us +
                    av_rescale_q(s->media_frame_idx, av_inv_q(s->frame_rate),
                                 AV_TIME_BASE_Q);
            }
        }
    }

    if (s->enable_audio)
        dyn_serve_audio(ctx, s);

    /* Wake when held for delay / waiting to AV-prime. */
    {
        int64_t now = av_gettime_relative();
        int64_t delay_us = dyn_playout_delay_us(s);
        for (i = 0; i < s->nb_outputs; i++) {
            DynSource *ds = &s->srcs[i];
            int pending = 0;
            pthread_mutex_lock(&ds->lock);
            if (!ds->av_primed &&
                ((ds->audio_q && av_fifo_can_read(ds->audio_q) > 0) ||
                 (ds->video_q && av_fifo_can_read(ds->video_q) > 0)))
                pending = 1;
            else if (ds->video_q && av_fifo_can_read(ds->video_q) > 0) {
                AVFrame *f = NULL;
                if (av_fifo_peek(ds->video_q, &f, 1, 0) >= 0 && f &&
                    !dyn_frame_is_due_locked(ds, f,
                                             dyn_meta_int64(f, "lavfi.dyn_arrival_us"),
                                             now, delay_us))
                    pending = 1;
            } else if (ds->audio_q && av_fifo_can_read(ds->audio_q) > 0) {
                AVFrame *f = NULL;
                if (av_fifo_peek(ds->audio_q, &f, 1, 0) >= 0 && f &&
                    !dyn_frame_is_due_locked(ds, f,
                                             dyn_meta_int64(f, "lavfi.dyn_arrival_us"),
                                             now, delay_us))
                    pending = 1;
            }
            pthread_mutex_unlock(&ds->lock);
            if (pending) {
                ff_filter_set_ready(ctx, 10);
                break;
            }
        }
    }

    return 0;
}

static av_cold void dynamic_input_uninit(AVFilterContext *ctx)
{
    DynamicInputContext *s = ctx->priv;
    int i;

    dyn_open_worker_stop(s);
    for (i = 0; i < s->nb_outputs; i++) {
        dyn_stop_source(&s->srcs[i]);
        pthread_mutex_lock(&s->srcs[i].lock);
        dyn_video_q_clear_locked(&s->srcs[i]);
        dyn_audio_q_clear_locked(&s->srcs[i]);
        av_frame_free(&s->srcs[i].cur_hw);
        pthread_mutex_unlock(&s->srcs[i].lock);
        av_fifo_freep2(&s->srcs[i].video_q);
        av_fifo_freep2(&s->srcs[i].audio_q);
        pthread_mutex_destroy(&s->srcs[i].lock);
    }
    pthread_mutex_destroy(&s->url_lock);
    av_channel_layout_uninit(&s->ch_layout);
    av_buffer_unref(&s->frames_ctx);
    av_buffer_unref(&s->own_device);
    s->device_ref = NULL;
    s->hwctx = NULL;
}

static int dynamic_input_query_formats(const AVFilterContext *ctx,
                                       AVFilterFormatsConfig **cfg_in,
                                       AVFilterFormatsConfig **cfg_out)
{
    const DynamicInputContext *s = ctx->priv;
    int pix_list[] = { AV_PIX_FMT_CUDA, -1 };
    int sample_list[] = { AV_SAMPLE_FMT_FLTP, -1 };
    int rate_list[] = { 0, -1 };
    AVChannelLayout layout_list[] = { { 0 }, { 0 } };
    int i, ret;

    for (i = 0; i < s->nb_outputs; i++) {
        if ((ret = ff_formats_ref(ff_make_format_list(pix_list), &cfg_out[i]->formats)) < 0)
            return ret;
    }

    if (!s->enable_audio)
        return 0;

    rate_list[0] = s->sample_rate;
    layout_list[0] = s->ch_layout;

    for (i = 0; i < s->nb_outputs; i++) {
        AVFilterFormatsConfig *cfg = cfg_out[s->nb_outputs + i];
        if ((ret = ff_formats_ref(ff_make_format_list(sample_list), &cfg->formats)) < 0)
            return ret;
        if ((ret = ff_formats_ref(ff_make_format_list(rate_list), &cfg->samplerates)) < 0)
            return ret;
        if ((ret = ff_channel_layouts_ref(ff_make_channel_layout_list(layout_list),
                                          &cfg->channel_layouts)) < 0)
            return ret;
    }
    return 0;
}

const FFFilter ff_vsrc_dynamic_input = {
    .p.name         = "dynamic_input",
    .p.description  = NULL_IF_CONFIG_SMALL("Dynamic multi-source CUDA video(+audio) source"),
    .p.inputs       = NULL,
    .p.flags        = AVFILTER_FLAG_DYNAMIC_OUTPUTS | AVFILTER_FLAG_HWDEVICE,
    .p.priv_class   = &dynamic_input_class,
    .priv_size      = sizeof(DynamicInputContext),
    .init           = dynamic_input_init,
    .uninit         = dynamic_input_uninit,
    .activate       = dynamic_input_activate,
    .process_command = dynamic_input_process_command,
    FILTER_QUERY_FUNC2(dynamic_input_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
