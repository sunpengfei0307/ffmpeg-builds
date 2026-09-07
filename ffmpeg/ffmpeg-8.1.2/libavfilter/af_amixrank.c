/*
 * Audio loudness-ranking mix filter (amixrank).
 *
 * Based on af_amix.c. For each input maintains a short-time RMS loudness window,
 * ranks inputs by loudness (descending), publishes the ranking as JSON via zmq
 * PUB at rank_interval_ms, and applies ducking (rank[0] full volume, others
 * attenuated by duck_gain_db). Smooth transition via dropout_transition.
 *
 * Live overlay model: anullsrc (input0) is the fixed-rate clock. RTMP pads are
 * mixed when samples are present and contribute silence when absent — they must
 * never stall the encode. buffer_ms is the single jitter cushion / warn limit;
 * steady-state must not drop business audio (drop = bug → amix_sync WARN).
 * Emergency drain only if FIFO >> buffer (OOM guard).
 *
 * JSON contract: {"rank":[i,...],"levels":[db,...]} where levels[i] is the
 * dBFS loudness of input i (input-index order, NOT rank order).
 *
 * zmq is conditionally compiled (CONFIG_LIBZMQ). Without zmq, ducking still
 * applies and rank is published only at log level.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libavutil/attributes.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/float_dsp.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/eval.h"

#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "lavfi_zeromq.h"

#if CONFIG_LIBZMQ
#include <zmq.h>
#endif

#define INPUT_ON       1
#define INPUT_EOF      2

#define DURATION_LONGEST  0
#define DURATION_SHORTEST 1
#define DURATION_FIRST    2

typedef struct FrameInfo {
    int nb_samples;
    int64_t pts;
    struct FrameInfo *next;
} FrameInfo;

typedef struct FrameList {
    int nb_frames;
    int nb_samples;
    FrameInfo *list;
    FrameInfo *end;
} FrameList;

static void frame_list_clear(FrameList *fl)
{
    if (fl) {
        while (fl->list) {
            FrameInfo *info = fl->list;
            fl->list = info->next;
            av_free(info);
        }
        fl->nb_frames = 0;
        fl->nb_samples = 0;
        fl->end = NULL;
    }
}

static int frame_list_next_frame_size(FrameList *fl)
{
    return fl->list ? fl->list->nb_samples : 0;
}

static int64_t frame_list_next_pts(FrameList *fl)
{
    return fl->list ? fl->list->pts : AV_NOPTS_VALUE;
}

static void frame_list_remove_samples(FrameList *fl, int nb_samples)
{
    if (nb_samples >= fl->nb_samples) {
        frame_list_clear(fl);
    } else {
        int s = nb_samples;
        while (s > 0) {
            FrameInfo *info = fl->list;
            av_assert0(info);
            if (info->nb_samples <= s) {
                s -= info->nb_samples;
                fl->list = info->next;
                if (!fl->list) fl->end = NULL;
                fl->nb_frames--;
                fl->nb_samples -= info->nb_samples;
                av_free(info);
            } else {
                info->nb_samples -= s;
                fl->nb_samples -= s;
                s = 0;
            }
        }
    }
}

static int frame_list_add_frame(FrameList *fl, int nb_samples, int64_t pts)
{
    FrameInfo *info = av_malloc(sizeof(*info));
    if (!info) return AVERROR(ENOMEM);
    info->nb_samples = nb_samples;
    info->pts = pts;
    info->next = NULL;
    if (!fl->list) { fl->list = info; fl->end = info; }
    else { fl->end->next = info; fl->end = info; }
    fl->nb_frames++;
    fl->nb_samples += nb_samples;
    return 0;
}

typedef struct AmixRankContext {
    const AVClass *class;
    AVFloatDSPContext *fdsp;

    int nb_inputs;
    int active_inputs;
    int duration_mode;
    float dropout_transition;
    char *weights_str;
    int normalize;

    int nb_channels;
    int sample_rate;
    int planar;
    AVAudioFifo **fifos;
    uint8_t *input_state;
    float *input_scale;
    float *weights;
    float weight_sum;
    float *scale_norm;
    int64_t next_pts;
    FrameList *frame_list;

    /* ---- amixrank extensions ---- */
    float  *loudness_db;        /* per-input short-time loudness (dBFS) */
    double *loud_mean_sq;       /* EMA of mean square */
    int    *rank;               /* rank[0]=loudest input idx */
    int    *rank_order;         /* scratch for the ranking sort */
    uint8_t *got_audio;         /* input delivered real audio at least once */
    uint8_t *mix_enabled;       /* business pad mix on/off (set_active); input0 always on */
    uint8_t *armed;             /* input has built its jitter cushion */
    int    *dry_samples;        /* consecutive samples mixed as silence */
    int     buffer_ms;          /* single cushion: arm depth + overflow warn */
    int     buffer_samples;
    int     emergency_samples;  /* OOM guard (~max(10*buffer, 2s)); only drain here */
    int     startup_grace_ms;   /* after start: no OVERFLOW warn while encode ramps */
    int64_t start_us;           /* config_output wall time */
    int     sync_warn_interval_ms;
    int64_t *last_sync_warn_us; /* per-input throttle */
    int     ducking;            /* enable ducking */
    float   duck_gain_db;       /* attenuation for non-rank[0] inputs */
    float  *duck_cur;           /* smoothed linear gain per input (0..weight) */
    int    *join_fade_left;     /* samples left in cold-join fade after arm */
    int     duck_attack_ms;     /* rise time toward louder / unduck */
    int     duck_release_ms;    /* fall time toward duck / silence rejoin */
    int     duck_hold_ms;       /* hold current rank[0] before switch */
    int     join_fade_ms;       /* longer fade when arming after remove/hole */
    int     held_rank0;
    int64_t rank0_since_us;
    int     rank_interval_ms;   /* zmq PUB interval */
    char   *rank_endpoint;      /* zmq PUB bind endpoint (tcp/ipc); overridden by task_id */
    int     task_id;            /* prefer ipc:///data/LCMS/sock/gain_<id>.sock */
    int64_t last_publish_us;

#if CONFIG_LIBZMQ
    LavfiZmq *zmq_pub;          /* IZeromq-style wrapper (lavfi_zeromq) */
#endif
} AmixRankContext;

#define OFFSET(x) offsetof(AmixRankContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
#define T AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption amixrank_options[] = {
    { "inputs", "Number of inputs.",
            OFFSET(nb_inputs), AV_OPT_TYPE_INT, { .i64 = 2 }, 1, INT16_MAX, A|F },
    { "duration", "How to determine the end-of-stream.",
            OFFSET(duration_mode), AV_OPT_TYPE_INT, { .i64 = DURATION_LONGEST }, 0, 2, A|F, .unit = "duration" },
        { "longest",  "Duration of longest input.",  0, AV_OPT_TYPE_CONST, { .i64 = DURATION_LONGEST  }, 0, 0, A|F, .unit = "duration" },
        { "shortest", "Duration of shortest input.", 0, AV_OPT_TYPE_CONST, { .i64 = DURATION_SHORTEST }, 0, 0, A|F, .unit = "duration" },
        { "first",    "Duration of first input.",    0, AV_OPT_TYPE_CONST, { .i64 = DURATION_FIRST    }, 0, 0, A|F, .unit = "duration" },
    { "dropout_transition", "Transition time, in seconds, for volume renormalization when an input drops out.",
            OFFSET(dropout_transition), AV_OPT_TYPE_FLOAT, { .dbl = 2.0 }, 0, INT_MAX, A|F },
    { "weights", "Per-input weights; prefer 0|1|1|1|1 (also spaces/commas). "
                 "0=exclude from mix (clock pad).",
            OFFSET(weights_str), AV_OPT_TYPE_STRING, {.str="1 1"}, 0, 0, A|F|T },
    { "normalize", "Scale inputs.",
            OFFSET(normalize), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, A|F|T },
    { "buffer_ms", "Single per-input audio cushion (ms): join depth + overflow warn. "
                   "Steady-state must not drop audio; over buffer → amix_sync WARN only. "
                   "0=no cushion / no overflow warn.",
            OFFSET(buffer_ms), AV_OPT_TYPE_INT, {.i64=200}, 0, 5000, A|F|T },
    { "startup_grace_ms", "After start, suppress OVERFLOW warn while encode ramps (ms).",
            OFFSET(startup_grace_ms), AV_OPT_TYPE_INT, {.i64=10000}, 0, 60000, A|F|T },
    { "sync_warn_interval_ms", "Min interval between amix_sync warnings for the same input.",
            OFFSET(sync_warn_interval_ms), AV_OPT_TYPE_INT, {.i64=10000}, 1000, 600000, A|F|T },
    { "ducking", "Enable ducking (loudest full, others attenuated).",
            OFFSET(ducking), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, A|F|T },
    { "duck_gain_db", "Ducking attenuation in dB for non-loudest inputs.",
            OFFSET(duck_gain_db), AV_OPT_TYPE_FLOAT, {.dbl=-18.0}, -120, 0, A|F|T },
    { "duck_attack_ms", "Soft-duck rise time (ms) when gaining / unducking.",
            OFFSET(duck_attack_ms), AV_OPT_TYPE_INT, {.i64=80}, 1, 2000, A|F|T },
    { "duck_release_ms", "Soft-duck fall time (ms) when ducking / rejoining after hole.",
            OFFSET(duck_release_ms), AV_OPT_TYPE_INT, {.i64=250}, 1, 5000, A|F|T },
    { "duck_hold_ms", "Keep current loudest speaker this long before ducking switch.",
            OFFSET(duck_hold_ms), AV_OPT_TYPE_INT, {.i64=400}, 0, 10000, A|F|T },
    { "join_fade_ms", "Extra fade-in when an input arms after silence/remove (cold join).",
            OFFSET(join_fade_ms), AV_OPT_TYPE_INT, {.i64=500}, 0, 5000, A|F|T },
    { "rank_interval_ms", "zmq PUB interval for loudness rank.",
            OFFSET(rank_interval_ms), AV_OPT_TYPE_INT, {.i64=100}, 10, 10000, A|F },
    { "rank_endpoint", "zmq PUB endpoint; ignored when task_id/FFMPEG_TASK_ID set "
                       "(uses ipc:///data/LCMS/sock/gain_<id>.sock).",
            OFFSET(rank_endpoint), AV_OPT_TYPE_STRING, {0}, 0, 0, A|F },
    { "task_id", "LCMS task id; prefer ipc gain sock (also from -task_id / FFMPEG_TASK_ID).",
            OFFSET(task_id), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, A|F },
    { NULL }
};

AVFILTER_DEFINE_CLASS(amixrank);

/* Business pad participates in mix/rank (weights + set_active overlay). */
static int amixrank_mix_on(const AmixRankContext *s, int i)
{
    if (i < 0 || i >= s->nb_inputs)
        return 0;
    if (!(s->input_state[i] & INPUT_ON) || s->weights[i] == 0.f)
        return 0;
    if (i > 0 && s->mix_enabled && !s->mix_enabled[i])
        return 0;
    return 1;
}

static void amixrank_disable_pad(AVFilterContext *ctx, int idx)
{
    AmixRankContext *s = ctx->priv;

    if (idx <= 0 || idx >= s->nb_inputs)
        return;
    if (s->mix_enabled)
        s->mix_enabled[idx] = 0;
    if (s->fifos && s->fifos[idx])
        av_audio_fifo_reset(s->fifos[idx]);
    if (s->armed)
        s->armed[idx] = 0;
    if (s->dry_samples)
        s->dry_samples[idx] = 0;
    if (s->duck_cur)
        s->duck_cur[idx] = 0.f;
    if (s->join_fade_left)
        s->join_fade_left[idx] = 0;
    if (s->loudness_db)
        s->loudness_db[idx] = -999.0f;
    if (s->loud_mean_sq)
        s->loud_mean_sq[idx] = 0.0;
    if (s->held_rank0 == idx) {
        s->held_rank0 = -1;
        s->rank0_since_us = AV_NOPTS_VALUE;
    }
    av_log(ctx, AV_LOG_WARNING,
           "amixrank: set_active slot%d=0 (input%d muted, FIFO drained)\n",
           idx, idx);
}

static void amixrank_enable_pad(AVFilterContext *ctx, int idx)
{
    AmixRankContext *s = ctx->priv;

    if (idx <= 0 || idx >= s->nb_inputs)
        return;
    if (s->mix_enabled)
        s->mix_enabled[idx] = 1;
    if (s->fifos && s->fifos[idx])
        av_audio_fifo_reset(s->fifos[idx]);
    if (s->armed)
        s->armed[idx] = 0;
    if (s->dry_samples)
        s->dry_samples[idx] = 0;
    if (s->duck_cur)
        s->duck_cur[idx] = 0.f;
    if (s->join_fade_left)
        s->join_fade_left[idx] = 0;
    av_log(ctx, AV_LOG_WARNING,
           "amixrank: set_active slot%d=1 (input%d re-arm with join_fade)\n",
           idx, idx);
}

static int find_passthrough_input(AmixRankContext *s)
{
    int idx = -1, i;
    for (i = 0; i < s->nb_inputs; i++) {
        if (amixrank_mix_on(s, i)) {
            if (idx >= 0) return -1;
            idx = i;
        }
    }
    /* The fast path copies the FIFO straight out, skipping the cushion logic —
     * only take it once that input has actually joined the mix. */
    if (idx > 0 && s->buffer_samples > 0 && !s->armed[idx])
        return -1;
    return idx;
}

static void amixrank_recompute_latency(AmixRankContext *s)
{
    int emerg_ms;

    if (s->sample_rate <= 0)
        return;
    s->buffer_samples = (int)av_rescale(s->buffer_ms, s->sample_rate, 1000);
    /* OOM guard only — not a sync tool. Dropping here means a serious bug. */
    emerg_ms = FFMAX(s->buffer_ms * 10, 2000);
    if (s->buffer_ms <= 0)
        emerg_ms = 2000;
    s->emergency_samples = (int)av_rescale(emerg_ms, s->sample_rate, 1000);
}

static int amixrank_in_grace(const AmixRankContext *s, int64_t now)
{
    return s->startup_grace_ms > 0 && s->start_us > 0 &&
           now - s->start_us < (int64_t)s->startup_grace_ms * 1000;
}

/* Anomaly-only: healthy FIFO near buffer_ms stays silent. */
static void amixrank_sync_warn(AVFilterContext *ctx, int idx, const char *code,
                               int fifo_ms, int drop_ms, const char *extra)
{
    AmixRankContext *s = ctx->priv;
    int64_t now = av_gettime_relative();
    int interval = FFMAX(s->sync_warn_interval_ms, 1000);

    if (idx <= 0 || !code)
        return;
    if (s->last_sync_warn_us &&
        now - s->last_sync_warn_us[idx] < (int64_t)interval * 1000)
        return;
    if (s->last_sync_warn_us)
        s->last_sync_warn_us[idx] = now;

    if (!strcmp(code, "OVERFLOW"))
        av_log(ctx, AV_LOG_WARNING,
               "amix_sync: in%d IMBALANCE code=OVERFLOW fifo=%dms buffer=%d trimmed=%dms%s | "
               "cause: pad audio piled vs anullsrc; trimmed to buffer to keep mix A/V lips "
               "(video already on program clock — late audio = mouth mismatch) | "
               "fix: check this pad dyn_sync a_out / bursts; do NOT raise buffer_ms to hide\n",
               idx, fifo_ms, s->buffer_ms, drop_ms, extra ? extra : "");
    else if (!strcmp(code, "AUDIO_DROP"))
        av_log(ctx, AV_LOG_WARNING,
               "amix_sync: in%d IMBALANCE code=AUDIO_DROP dropped=%dms fifo=%dms buffer=%d%s | "
               "cause: emergency OOM guard drained audio | "
               "fix: severe input>>mix rate; check dyn_sync DROP_A / encode stall\n",
               idx, drop_ms, fifo_ms, s->buffer_ms, extra ? extra : "");
    else
        av_log(ctx, AV_LOG_WARNING,
               "amix_sync: in%d IMBALANCE code=%s fifo=%dms%s | cause: sync imbalance | "
               "fix: see docs/amixrank.md\n",
               idx, code, fifo_ms, extra ? extra : "");
}

/*
 * Lip-sync on the muxed output: video is already paced by color_cuda while
 * this FIFO delays pad audio. Sitting slightly over buffer is OK; clearly
 * above 2x buffer → trim back to buffer_ms (keep mouths matched) + WARN.
 * emergency_samples still hard-caps unbounded growth.
 */
static void amixrank_check_input_fifo(AVFilterContext *ctx, int idx)
{
    AmixRankContext *s = ctx->priv;
    int sz, fifo_ms, drop, warn_ms;
    int64_t now;
    int in_grace;

    if (idx <= 0 || !s->fifos[idx] || s->sample_rate <= 0)
        return;
    if (!amixrank_mix_on(s, idx))
        return;
    if (s->join_fade_left && s->join_fade_left[idx] > 0)
        return;

    now = av_gettime_relative();
    in_grace = amixrank_in_grace(s, now);
    sz = av_audio_fifo_size(s->fifos[idx]);
    fifo_ms = (int)(sz * 1000LL / s->sample_rate);

    if (s->emergency_samples > 0 && sz > s->emergency_samples) {
        drop = sz - (s->buffer_samples > 0 ? s->buffer_samples : s->emergency_samples / 2);
        if (drop > 0) {
            av_audio_fifo_drain(s->fifos[idx], drop);
            fifo_ms = (int)(av_audio_fifo_size(s->fifos[idx]) * 1000LL / s->sample_rate);
            amixrank_sync_warn(ctx, idx, "AUDIO_DROP", fifo_ms,
                               (int)(drop * 1000LL / s->sample_rate), NULL);
        }
        return;
    }

    if (in_grace || s->buffer_ms <= 0 || s->buffer_samples <= 0)
        return;
    if (!s->armed || !s->armed[idx])
        return;
    /* 2x cushion → trim to buffer so pad audio does not lag program video. */
    warn_ms = s->buffer_ms * 2;
    if (fifo_ms > warn_ms) {
        drop = sz - s->buffer_samples;
        if (drop > 0) {
            av_audio_fifo_drain(s->fifos[idx], drop);
            fifo_ms = (int)(av_audio_fifo_size(s->fifos[idx]) * 1000LL / s->sample_rate);
            amixrank_sync_warn(ctx, idx, "OVERFLOW", fifo_ms,
                               (int)(drop * 1000LL / s->sample_rate), NULL);
        }
    }
}

/*
 * Update per-input loudness from the buffer just read: true dBFS RMS over all
 * channels, smoothed by an EMA (alpha 0.1, ~10 frames). Reported levels are
 * absolute, so downstream can threshold on them.
 */
static void amixrank_update_loudness(AmixRankContext *s, const AVFrame *buf,
                                     int nb_samples, int idx)
{
    int planes     = s->planar ? s->nb_channels : 1;
    int per_plane  = s->planar ? nb_samples : nb_samples * s->nb_channels;
    int64_t total  = (int64_t)planes * per_plane;
    double sum_sq  = 0.0;
    int p, i;

    if (total <= 0)
        return;

    if (buf->format == AV_SAMPLE_FMT_FLT || buf->format == AV_SAMPLE_FMT_FLTP) {
        for (p = 0; p < planes; p++) {
            const float *x = (const float *)buf->extended_data[p];
            for (i = 0; i < per_plane; i++)
                sum_sq += (double)x[i] * (double)x[i];
        }
    } else {
        for (p = 0; p < planes; p++) {
            const double *x = (const double *)buf->extended_data[p];
            for (i = 0; i < per_plane; i++)
                sum_sq += x[i] * x[i];
        }
    }

    s->loud_mean_sq[idx] = s->loud_mean_sq[idx] * 0.9 + 0.1 * (sum_sq / total);
    if (s->loud_mean_sq[idx] > 1e-18) {
        double rms = sqrt(s->loud_mean_sq[idx]);
        s->loudness_db[idx] = rms > 1e-6 ? (float)(20.0 * log10(rms)) : -120.0f;
    } else {
        s->loudness_db[idx] = -120.0f;
    }
}

static void amixrank_compute_rank(AmixRankContext *s)
{
    int *order = s->rank_order;
    int i, j, tmp;
    for (i = 0; i < s->nb_inputs; i++) {
        order[i] = i;
        /* Clock / silent / not-yet-armed pads must never win ducking rank.
         * Stale loudness after remove/reconnect was causing volume pumping. */
        if (!amixrank_mix_on(s, i) ||
            (i > 0 && s->buffer_samples > 0 && !s->armed[i]) ||
            (i > 0 && s->dry_samples && s->dry_samples[i] > 0))
            s->loudness_db[i] = -999.0f;
    }
    for (i = 1; i < s->nb_inputs; i++) {
        j = i;
        while (j > 0 && s->loudness_db[order[j-1]] < s->loudness_db[order[j]]) {
            tmp = order[j-1]; order[j-1] = order[j]; order[j] = tmp;
            j--;
        }
    }
    for (i = 0; i < s->nb_inputs; i++)
        s->rank[i] = order[i];
}

static void amixrank_publish_rank(AVFilterContext *ctx)
{
    AmixRankContext *s = ctx->priv;
    int64_t now = av_gettime_relative();
    char buf[1024];
    int off = 0, i;

    if (now - s->last_publish_us < (int64_t)s->rank_interval_ms * 1000)
        return;
    s->last_publish_us = now;

    off += snprintf(buf + off, sizeof(buf) - off, "{\"rank\":[");
    for (i = 0; i < s->nb_inputs && off < (int)sizeof(buf) - 32; i++)
        off += snprintf(buf + off, sizeof(buf) - off, "%s%d", i ? "," : "", s->rank[i]);
    /* levels[i] = loudness of input i (input-index order, NOT rank order) */
    off += snprintf(buf + off, sizeof(buf) - off, "],\"levels\":[");
    for (i = 0; i < s->nb_inputs && off < (int)sizeof(buf) - 32; i++)
        off += snprintf(buf + off, sizeof(buf) - off, "%s%.1f", i ? "," : "", s->loudness_db[i]);
    off += snprintf(buf + off, sizeof(buf) - off, "]}");

#if CONFIG_LIBZMQ
    if (s->zmq_pub)
        lavfi_zmq_send(s->zmq_pub, buf, strlen(buf), 0);
#endif
    av_log(ctx, AV_LOG_DEBUG, "amixrank: %s\n", buf);
}

/* Smooth loudness drop while an input is dry (remove / reconnect hole). */
static void amixrank_decay_loudness(AmixRankContext *s, int idx)
{
    if (idx < 0 || idx >= s->nb_inputs || !amixrank_mix_on(s, idx))
        return;
    s->loud_mean_sq[idx] *= 0.90;
    if (s->loud_mean_sq[idx] > 1e-18) {
        double rms = sqrt(s->loud_mean_sq[idx]);
        s->loudness_db[idx] = rms > 1e-6 ? (float)(20.0 * log10(rms)) : -120.0f;
    } else {
        s->loudness_db[idx] = -120.0f;
        s->loud_mean_sq[idx] = 0.0;
    }
}

/* Hold rank[0] briefly so remove/reconnect does not pump ducking. */
static void amixrank_stabilize_rank0(AmixRankContext *s)
{
    int cand, i;
    int64_t now;

    if (!s->ducking || s->duck_hold_ms <= 0 || s->nb_inputs <= 0)
        return;
    cand = s->rank[0];
    if (cand < 0)
        return;
    now = av_gettime_relative();
    if (s->held_rank0 < 0 || s->held_rank0 >= s->nb_inputs ||
        !amixrank_mix_on(s, s->held_rank0) ||
        s->loudness_db[s->held_rank0] <= -90.0f) {
        s->held_rank0 = cand;
        s->rank0_since_us = now;
    } else if (cand != s->held_rank0) {
        float diff = s->loudness_db[cand] - s->loudness_db[s->held_rank0];
        if (diff < 6.0f &&
            now - s->rank0_since_us < (int64_t)s->duck_hold_ms * 1000) {
            /* Keep previous loudest in rank[0] slot. */
            int old = s->held_rank0;
            for (i = 0; i < s->nb_inputs; i++) {
                if (s->rank[i] == old) {
                    s->rank[i] = cand;
                    break;
                }
            }
            s->rank[0] = old;
            return;
        }
        s->held_rank0 = cand;
        s->rank0_since_us = now;
    } else {
        s->rank0_since_us = now;
    }
}

/* Override input_scale with smoothed ducking. */
static void amixrank_apply_ducking(AmixRankContext *s, int nb_samples)
{
    float duck_lin = powf(10.0f, s->duck_gain_db / 20.0f);
    float scale_sum = 0.f;
    float atk, rel;
    int i;

    if (!s->ducking || s->nb_inputs <= 0)
        return;

    amixrank_stabilize_rank0(s);

    atk = nb_samples / FFMAX(1.f, (float)s->duck_attack_ms * s->sample_rate / 1000.f);
    rel = nb_samples / FFMAX(1.f, (float)s->duck_release_ms * s->sample_rate / 1000.f);
    atk = av_clipf(atk, 0.01f, 1.f);
    rel = av_clipf(rel, 0.01f, 1.f);

    for (i = 0; i < s->nb_inputs; i++) {
        float target, cur, alpha, atk_i;
        if (!amixrank_mix_on(s, i)) {
            s->input_scale[i] = 0.0f;
            if (s->duck_cur)
                s->duck_cur[i] = 0.0f;
            if (s->join_fade_left)
                s->join_fade_left[i] = 0;
            continue;
        }
        target = (i == s->rank[0]) ? FFABS(s->weights[i])
                                   : FFABS(s->weights[i]) * duck_lin;
        /* Rejoining after silence: start from 0 so volume ramps in. */
        if (!s->armed[i] && s->buffer_samples > 0)
            target = 0.0f;
        cur = s->duck_cur ? s->duck_cur[i] : target;
        atk_i = atk;
        /* Cold join after remove/long hole: slower fade-in (~join_fade_ms). */
        if (s->join_fade_left && s->join_fade_left[i] > 0 && s->join_fade_ms > 0) {
            atk_i = nb_samples / FFMAX(1.f, (float)s->join_fade_ms * s->sample_rate / 1000.f);
            atk_i = av_clipf(atk_i, 0.005f, 1.f);
            s->join_fade_left[i] = FFMAX(0, s->join_fade_left[i] - nb_samples);
        }
        alpha = (target > cur) ? atk_i : rel;
        cur += (target - cur) * alpha;
        if (s->duck_cur)
            s->duck_cur[i] = cur;
        s->input_scale[i] = cur;
        scale_sum += s->input_scale[i];
    }
    /* Ducking replaces calculate_scales() entirely, so honour normalize here
     * too — otherwise the summed gains can exceed 1.0 and clip. */
    if (s->normalize && scale_sum > 1.f) {
        for (i = 0; i < s->nb_inputs; i++)
            s->input_scale[i] /= scale_sum;
    }
}

static void calculate_scales(AmixRankContext *s, int nb_samples)
{
    float weight_sum = 0.f;
    int i;
    for (i = 0; i < s->nb_inputs; i++)
        if (amixrank_mix_on(s, i))
            weight_sum += FFABS(s->weights[i]);
    for (i = 0; i < s->nb_inputs; i++) {
        if (amixrank_mix_on(s, i)) {
            if (s->scale_norm[i] > weight_sum / FFABS(s->weights[i])) {
                s->scale_norm[i] -= ((s->weight_sum / FFABS(s->weights[i])) / s->nb_inputs) *
                                    nb_samples / (s->dropout_transition * s->sample_rate);
                s->scale_norm[i] = FFMAX(s->scale_norm[i], weight_sum / FFABS(s->weights[i]));
            }
        }
    }
    for (i = 0; i < s->nb_inputs; i++) {
        if (amixrank_mix_on(s, i)) {
            if (!s->normalize)
                s->input_scale[i] = FFABS(s->weights[i]);
            else
                s->input_scale[i] = 1.0f / s->scale_norm[i] * FFSIGN(s->weights[i]);
        } else {
            s->input_scale[i] = 0.0f;
        }
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AmixRankContext *s = ctx->priv;
    int i;

    s->planar = av_sample_fmt_is_planar(outlink->format);
    s->sample_rate = outlink->sample_rate;
    outlink->time_base = (AVRational){ 1, outlink->sample_rate };
    s->next_pts = AV_NOPTS_VALUE;

    s->frame_list = av_mallocz(sizeof(*s->frame_list));
    if (!s->frame_list) return AVERROR(ENOMEM);

    s->fifos = av_calloc(s->nb_inputs, sizeof(*s->fifos));
    if (!s->fifos) return AVERROR(ENOMEM);

    s->nb_channels = outlink->ch_layout.nb_channels;
    for (i = 0; i < s->nb_inputs; i++) {
        s->fifos[i] = av_audio_fifo_alloc(outlink->format, s->nb_channels, 1024);
        if (!s->fifos[i]) return AVERROR(ENOMEM);
    }

    s->input_state = av_malloc(s->nb_inputs);
    if (!s->input_state) return AVERROR(ENOMEM);
    memset(s->input_state, INPUT_ON, s->nb_inputs);
    s->active_inputs = s->nb_inputs;

    s->input_scale = av_calloc(s->nb_inputs, sizeof(*s->input_scale));
    s->scale_norm  = av_calloc(s->nb_inputs, sizeof(*s->scale_norm));
    if (!s->input_scale || !s->scale_norm) return AVERROR(ENOMEM);
    for (i = 0; i < s->nb_inputs; i++)
        s->scale_norm[i] = s->weights[i] != 0.f
                           ? s->weight_sum / FFABS(s->weights[i]) : 0.f;
    calculate_scales(s, 0);
    av_log(ctx, AV_LOG_WARNING,
           "amixrank: inputs=%d normalize=%d ducking=%d buffer_ms=%d "
           "startup_grace_ms=%d weights=[",
           s->nb_inputs, s->normalize, s->ducking, s->buffer_ms,
           s->startup_grace_ms);
    for (i = 0; i < s->nb_inputs; i++)
        av_log(ctx, AV_LOG_WARNING, "%s%g", i ? " " : "", s->weights[i]);
    av_log(ctx, AV_LOG_WARNING, "] (use 0|1|1|1|1 if muted due to escaping)\n");

    /* amixrank extensions */
    s->loudness_db     = av_calloc(s->nb_inputs, sizeof(*s->loudness_db));
    s->loud_mean_sq    = av_calloc(s->nb_inputs, sizeof(*s->loud_mean_sq));
    s->rank            = av_calloc(s->nb_inputs, sizeof(*s->rank));
    s->rank_order      = av_calloc(s->nb_inputs, sizeof(*s->rank_order));
    s->got_audio       = av_calloc(s->nb_inputs, sizeof(*s->got_audio));
    s->mix_enabled     = av_malloc(s->nb_inputs);
    s->armed           = av_calloc(s->nb_inputs, sizeof(*s->armed));
    s->dry_samples     = av_calloc(s->nb_inputs, sizeof(*s->dry_samples));
    s->duck_cur        = av_calloc(s->nb_inputs, sizeof(*s->duck_cur));
    s->join_fade_left  = av_calloc(s->nb_inputs, sizeof(*s->join_fade_left));
    s->last_sync_warn_us = av_calloc(s->nb_inputs, sizeof(*s->last_sync_warn_us));
    if (!s->loudness_db || !s->loud_mean_sq || !s->rank || !s->rank_order ||
        !s->got_audio || !s->mix_enabled || !s->armed || !s->dry_samples ||
        !s->duck_cur || !s->join_fade_left || !s->last_sync_warn_us)
        return AVERROR(ENOMEM);
    memset(s->mix_enabled, 1, s->nb_inputs);
    s->held_rank0 = -1;
    s->rank0_since_us = AV_NOPTS_VALUE;
    s->start_us = av_gettime_relative();
    amixrank_recompute_latency(s);
    for (i = 0; i < s->nb_inputs; i++) {
        s->loudness_db[i] = -120.0f;
        s->rank[i] = i;
    }

#if CONFIG_LIBZMQ
    {
        char url[512];
        if (lavfi_zmq_resolve_url(url, sizeof(url), "gain", s->task_id,
                                  s->rank_endpoint) == 0) {
            s->zmq_pub = lavfi_zmq_create(ZMQ_PUB, 1, url, ctx);
            if (s->zmq_pub)
                av_log(ctx, AV_LOG_WARNING, "amixrank: PUB bound to %s\n", url);
        }
    }
#endif
    s->last_publish_us = 0;
    return 0;
}

static int output_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AmixRankContext *s = ctx->priv;
    AVFrame *out_buf, *in_buf;
    int nb_samples, ns, i;
    int passthrough_idx;

    if (s->input_state[0] & INPUT_ON) {
        /* Clock is input0 (anullsrc) ONLY. Never shrink nb_samples based on
         * optional RTMP inputs (EOF/empty would otherwise yield nb_samples=0
         * and stop the whole encode). Missing pads contribute silence. */
        nb_samples = frame_list_next_frame_size(s->frame_list);
        s->next_pts = frame_list_next_pts(s->frame_list);
    } else {
        nb_samples = INT_MAX;
        for (i = 1; i < s->nb_inputs; i++) {
            if (amixrank_mix_on(s, i)) {
                ns = av_audio_fifo_size(s->fifos[i]);
                nb_samples = FFMIN(nb_samples, ns);
            }
        }
        if (nb_samples == INT_MAX) {
            ff_outlink_set_status(outlink, AVERROR_EOF, s->next_pts);
            return 0;
        }
    }

    frame_list_remove_samples(s->frame_list, nb_samples);

    passthrough_idx = find_passthrough_input(s);
    if (passthrough_idx < 0) {
        calculate_scales(s, nb_samples);
        /* Ducking must run BEFORE mix; previously it ran after and never applied. */
        amixrank_compute_rank(s);
        amixrank_apply_ducking(s, nb_samples);
        amixrank_publish_rank(ctx);
    }

    if (nb_samples == 0)
        return 0;

    out_buf = ff_get_audio_buffer(outlink, nb_samples);
    if (!out_buf) return AVERROR(ENOMEM);
    in_buf = ff_get_audio_buffer(outlink, nb_samples);
    if (!in_buf) { av_frame_free(&out_buf); return AVERROR(ENOMEM); }

    /* Buffers come from a recycling pool and are NOT re-zeroed, while the mix
     * below accumulates with fmac and passthrough may read fewer samples than
     * requested — either way stale audio would leak back into the output. */
    av_samples_set_silence(out_buf->extended_data, 0, nb_samples,
                           out_buf->ch_layout.nb_channels, out_buf->format);

    if (passthrough_idx >= 0) {
        av_audio_fifo_read(s->fifos[passthrough_idx],
                           (void **)out_buf->extended_data, nb_samples);
        for (i = 0; i < s->nb_inputs; i++) {
            if (i != passthrough_idx && (s->input_state[i] & INPUT_ON))
                av_audio_fifo_read(s->fifos[i], (void **)in_buf->extended_data,
                                   nb_samples);
        }
    } else {
        for (i = 0; i < s->nb_inputs; i++) {
            if (s->input_state[i] & INPUT_ON) {
                int planes, plane_size, p, got;

                /*
                 * Wait until the jitter cushion is filled once, then mix every
                 * tick. Do not drain down to nb_samples here — that removes the
                 * cushion and turns normal jitter into silence holes.
                 * Cushion = buffer_ms; overflow warns, emergency-only drop.
                 */
                if (i > 0 && s->buffer_samples > 0 && !s->armed[i] &&
                    amixrank_mix_on(s, i)) {
                    if (av_audio_fifo_size(s->fifos[i]) < s->buffer_samples)
                        continue;
                    s->armed[i] = 1;
                    s->dry_samples[i] = 0;
                    if (s->duck_cur)
                        s->duck_cur[i] = 0.f; /* ramp in after remove/reconnect */
                    if (s->join_fade_left && s->join_fade_ms > 0)
                        s->join_fade_left[i] = (int)av_rescale(s->join_fade_ms,
                                                               s->sample_rate, 1000);
                    av_log(ctx, AV_LOG_INFO,
                           "amixrank: input%d joined the mix (%d samples / %dms cushion, "
                           "join_fade_ms=%d)\n",
                           i, av_audio_fifo_size(s->fifos[i]), s->buffer_ms,
                           s->join_fade_ms);
                }

                /* Zero-fill so missing/partial FIFO reads become silence. */
                av_samples_set_silence(in_buf->extended_data, 0, nb_samples,
                                      in_buf->ch_layout.nb_channels, in_buf->format);
                got = av_audio_fifo_read(s->fifos[i], (void **)in_buf->extended_data,
                                         nb_samples);
                if (got <= 0 && i != 0) {
                    s->dry_samples[i] += nb_samples;
                    amixrank_decay_loudness(s, i);
                    if (s->duck_cur)
                        s->duck_cur[i] *= 0.92f;
                    /* Re-arm only after a long hole (~500ms). Short underruns
                     * from live PTS re-anchor must not drop the pad out of the
                     * mix (single-input "audio on/off" stutter). */
                    if (s->buffer_samples > 0 &&
                        s->dry_samples[i] > FFMAX(4 * s->buffer_samples,
                                                  s->sample_rate / 2)) {
                        s->armed[i] = 0;
                        s->dry_samples[i] = 0;
                        if (s->duck_cur)
                            s->duck_cur[i] = 0.f;
                    }
                    continue;
                }
                s->dry_samples[i] = 0;

                if (got > 0 && amixrank_mix_on(s, i))
                    amixrank_update_loudness(s, in_buf, got, i);

                if (!amixrank_mix_on(s, i))
                    continue;
                if (s->input_scale[i] == 0.f)
                    continue;
                planes = s->planar ? s->nb_channels : 1;
                plane_size = nb_samples * (s->planar ? 1 : s->nb_channels);
                plane_size = FFALIGN(plane_size, 16);

                if (out_buf->format == AV_SAMPLE_FMT_FLT ||
                    out_buf->format == AV_SAMPLE_FMT_FLTP) {
                    for (p = 0; p < planes; p++) {
                        s->fdsp->vector_fmac_scalar((float *)out_buf->extended_data[p],
                                                    (float *) in_buf->extended_data[p],
                                                    s->input_scale[i], plane_size);
                    }
                } else {
                    for (p = 0; p < planes; p++) {
                        s->fdsp->vector_dmac_scalar((double *)out_buf->extended_data[p],
                                                    (double *) in_buf->extended_data[p],
                                                    s->input_scale[i], plane_size);
                    }
                }
            }
        }
    }
    av_frame_free(&in_buf);

    out_buf->pts = s->next_pts;
    out_buf->sample_rate = outlink->sample_rate;
    out_buf->duration = av_rescale_q(out_buf->nb_samples, av_make_q(1, outlink->sample_rate),
                                     outlink->time_base);
    if (s->next_pts != AV_NOPTS_VALUE)
        s->next_pts += nb_samples;

    for (i = 1; i < s->nb_inputs; i++) {
        if (amixrank_mix_on(s, i))
            amixrank_check_input_fifo(ctx, i);
    }

    return ff_filter_frame(outlink, out_buf);
}

static int request_samples(AVFilterContext *ctx, int min_samples)
{
    AmixRankContext *s = ctx->priv;
    int i, target;
    av_assert0(s->nb_inputs > 1);
    if (min_samples == 1 && s->duration_mode == DURATION_FIRST)
        min_samples = av_audio_fifo_size(s->fifos[0]);
    /* Live: pull next tick (+ buffer cushion). */
    target = FFMAX(min_samples, s->buffer_samples);
    /* Nudge optional inputs, but never block output on them. */
    for (i = 1; i < s->nb_inputs; i++) {
        if (!(s->input_state[i] & INPUT_ON) || (s->input_state[i] & INPUT_EOF))
            continue;
        if (s->weights[i] == 0.f)
            continue;
        /* Disabled pads: still pull+drop so upstream audio_q does not stall. */
        if (amixrank_mix_on(s, i) &&
            av_audio_fifo_size(s->fifos[i]) >= target)
            continue;
        ff_inlink_request_frame(ctx->inputs[i]);
    }
    return output_frame(ctx->outputs[0]);
}

static int calc_active_inputs(AmixRankContext *s)
{
    int i, active = 0;
    for (i = 0; i < s->nb_inputs; i++)
        active += !!(s->input_state[i] & INPUT_ON);
    if (s->active_inputs != active) {
        s->active_inputs = active;
        calculate_scales(s, 0);
    }
    switch (s->duration_mode) {
    case DURATION_FIRST: return s->input_state[0] & INPUT_EOF;
    case DURATION_SHORTEST: return active < s->nb_inputs;
    default: return 0;
    }
}

static int activate(AVFilterContext *ctx)
{
    AmixRankContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int i, ret;

    for (i = 0; i < s->nb_inputs; i++) {
        AVFrame *buf = NULL;
        if (!s->fifos[i])
            continue;
        ret = ff_inlink_consume_frame(ctx->inputs[i], &buf);
        if (ret > 0) {
            if (i == 0) {
                buf->pts = av_rescale_q(buf->pts, ctx->inputs[0]->time_base, outlink->time_base);
                ret = frame_list_add_frame(s->frame_list, buf->nb_samples, buf->pts);
                if (ret < 0) { av_frame_free(&buf); return ret; }
            } else if (i > 0 && s->mix_enabled && !s->mix_enabled[i]) {
                /* Disabled pad: drain graph without filling FIFO. */
                av_frame_free(&buf);
                continue;
            } else if (buf->nb_samples > 0 && s->weights[i] != 0.f) {
                if (!s->got_audio[i]) {
                    /* Enables the jitter grace period for this pad: an input
                     * that never spoke must not be waited for. */
                    s->got_audio[i] = 1;
                    av_log(ctx, AV_LOG_WARNING,
                           "amixrank: first real audio on input%d nb_samples=%d weight=%g\n",
                           i, buf->nb_samples, s->weights[i]);
                }
            }
            ret = av_audio_fifo_write(s->fifos[i], (void **)buf->extended_data,
                                      buf->nb_samples);
            av_frame_free(&buf);
            if (ret < 0) return ret;
            if (i > 0)
                amixrank_check_input_fifo(ctx, i);
            ret = output_frame(outlink);
            if (ret < 0) return ret;
        } else if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
                return ret;
        }
    }

    for (i = 0; i < s->nb_inputs; i++) {
        int64_t pts; int status;
        if (ff_inlink_acknowledge_status(ctx->inputs[i], &status, &pts)) {
            if (status == AVERROR_EOF) {
                s->input_state[i] |= INPUT_EOF;
                if (av_audio_fifo_size(s->fifos[i]) == 0)
                    s->input_state[i] &= ~INPUT_ON;
                /* Only input0 (anullsrc) EOF ends the mix; RTMP EOFs are ignored. */
                if (i == 0 && av_audio_fifo_size(s->fifos[0]) == 0) {
                    ff_outlink_set_status(outlink, status, pts);
                    return 0;
                }
            }
        }
    }

    /* Do not end on DURATION_SHORTEST when optional RTMP pads drop. */
    if (s->duration_mode == DURATION_FIRST && calc_active_inputs(s)) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->next_pts);
        return 0;
    }

    if (ff_outlink_frame_wanted(outlink)) {
        int wanted_samples;
        if (!(s->input_state[0] & INPUT_ON))
            return request_samples(ctx, 1);
        if (s->frame_list->nb_frames == 0) {
            ff_inlink_request_frame(ctx->inputs[0]);
            return 0;
        }
        av_assert0(s->frame_list->nb_frames > 0);
        wanted_samples = frame_list_next_frame_size(s->frame_list);
        return request_samples(ctx, wanted_samples);
    }

    /* Do NOT keep-request RTMP audio while the muxer is not pulling aout —
     * that fills FIFOs with old samples and audio drifts behind video. When
     * the encoder pauses, dynamic_input drops at the source instead. */
    return 0;
}

static void parse_weights(AVFilterContext *ctx)
{
    AmixRankContext *s = ctx->priv;
    float last_weight = 1.f;
    char *p;
    int i;
    s->weight_sum = 0.f;
    p = s->weights_str ? s->weights_str : "";
    for (i = 0; i < s->nb_inputs; i++) {
        while (p && (*p == ' ' || *p == '\t' || *p == ',' || *p == '|' || *p == ';'))
            p++;
        if (!p || !*p)
            break;
        last_weight = av_strtod(p, &p);
        s->weights[i] = last_weight;
        s->weight_sum += FFABS(last_weight);
    }
    /* Pad remaining with last value (amix behavior). */
    for (; i < s->nb_inputs; i++) {
        s->weights[i] = last_weight;
        s->weight_sum += FFABS(last_weight);
    }
    /*
     * Common filtergraph pitfall: weights=0\ 1\ 1 becomes just "0" after shell
     * eating escapes, then the loop above fills ALL inputs with 0 → mute.
     * Prefer weights=0|1|1|1|1 (no spaces) in command lines.
     */
    if (s->weight_sum == 0.f) {
        av_log(ctx, AV_LOG_ERROR,
               "amixrank: all weights are 0 (bad escaping?). "
               "Use weights=0|1|1|1|1 — falling back to 0|1|1|...\n");
        s->weight_sum = 0.f;
        for (i = 0; i < s->nb_inputs; i++) {
            s->weights[i] = (i == 0) ? 0.f : 1.f;
            s->weight_sum += FFABS(s->weights[i]);
        }
    }
}

static av_cold int init(AVFilterContext *ctx)
{
    AmixRankContext *s = ctx->priv;
    int i, ret;
    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = { 0 };
        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name) return AVERROR(ENOMEM);
        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }
    s->fdsp = avpriv_float_dsp_alloc(0);
    if (!s->fdsp) return AVERROR(ENOMEM);
    s->weights = av_calloc(s->nb_inputs, sizeof(*s->weights));
    if (!s->weights) return AVERROR(ENOMEM);
    parse_weights(ctx);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AmixRankContext *s = ctx->priv;
    int i;
    if (s->fifos) {
        for (i = 0; i < s->nb_inputs; i++)
            av_audio_fifo_free(s->fifos[i]);
        av_freep(&s->fifos);
    }
    frame_list_clear(s->frame_list);
    av_freep(&s->frame_list);
    av_freep(&s->input_state);
    av_freep(&s->input_scale);
    av_freep(&s->scale_norm);
    av_freep(&s->weights);
    av_freep(&s->fdsp);
    av_freep(&s->loudness_db);
    av_freep(&s->loud_mean_sq);
    av_freep(&s->rank);
    av_freep(&s->rank_order);
    av_freep(&s->got_audio);
    av_freep(&s->mix_enabled);
    av_freep(&s->armed);
    av_freep(&s->dry_samples);
    av_freep(&s->duck_cur);
    av_freep(&s->join_fade_left);
    av_freep(&s->last_sync_warn_us);
#if CONFIG_LIBZMQ
    lavfi_zmq_destroy(&s->zmq_pub);
#endif
}

/* Business slots 1..N (N = nb_inputs-1); supports "2" or "1-4". */
static int amixrank_parse_slot_range(const char *tok, int nb_inputs, int *lo, int *hi)
{
    int a, b;
    char *dash;
    int max_slot;

    if (!tok || !*tok || !lo || !hi || nb_inputs < 2)
        return AVERROR(EINVAL);
    max_slot = nb_inputs - 1; /* input0 = anullsrc */
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
    if (a < 1 || b < 1 || a > max_slot || b > max_slot || a > b)
        return AVERROR(EINVAL);
    *lo = a;
    *hi = b;
    return 0;
}

static int amixrank_cmd_search(AVFilterContext *ctx, const char *args,
                               char *res, int res_len)
{
    AmixRankContext *s = ctx->priv;
    int i, off = 0;
    int lo = 1, hi = s->nb_inputs > 1 ? s->nb_inputs - 1 : 0;

    if (!res || res_len <= 0)
        return 0;
    res[0] = 0;
    if (args && *args) {
        char tok[64];
        if (sscanf(args, "%63s", tok) != 1 ||
            amixrank_parse_slot_range(tok, s->nb_inputs, &lo, &hi) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "amixrank: search bad args='%s' (need: [slot|lo-hi])\n",
                   args);
            return AVERROR(EINVAL);
        }
    }
    /* Business slots 1..N (= amixrank input index): slot:enabled:weight */
    for (i = lo; i <= hi; i++) {
        int en = (!s->mix_enabled || s->mix_enabled[i]) ? 1 : 0;
        off += snprintf(res + off, res_len > off ? res_len - off : 0,
                        "%s%d:%d:%g", off ? " " : "", i, en, s->weights[i]);
        if (off >= res_len)
            break;
    }
    if (!off)
        snprintf(res, res_len, "(none)");
    av_log(ctx, AV_LOG_WARNING, "amixrank: search -> %s\n", res);
    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    AmixRankContext *s = ctx->priv;
    int ret;

    av_log(ctx, AV_LOG_WARNING,
           "amixrank: cmd='%s' args='%s'\n", cmd, args ? args : "");

    if (!strcmp(cmd, "search"))
        return amixrank_cmd_search(ctx, args, res, res_len);
    if (!strcmp(cmd, "set_active") && args) {
        char tok[64];
        int lo, hi, val, slot;
        /* Slot 1..N or 1-4; matches dynamic_input / mixing business pads. */
        if (sscanf(args, "%63s %d", tok, &val) != 2) {
            av_log(ctx, AV_LOG_ERROR,
                   "amixrank: set_active bad args='%s' "
                   "(need: <slot|lo-hi> <0|1>)\n", args);
            return AVERROR(EINVAL);
        }
        if (amixrank_parse_slot_range(tok, s->nb_inputs, &lo, &hi) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "amixrank: set_active bad range='%s' (business 1..%d)\n",
                   tok, s->nb_inputs - 1);
            return AVERROR(EINVAL);
        }
        if (!s->mix_enabled) {
            av_log(ctx, AV_LOG_ERROR,
                   "amixrank: set_active before config_output (mix_enabled unset)\n");
            return AVERROR(EINVAL);
        }
        for (slot = lo; slot <= hi; slot++) {
            if (val) {
                if (!s->mix_enabled[slot])
                    amixrank_enable_pad(ctx, slot);
            } else {
                amixrank_disable_pad(ctx, slot);
            }
        }
        av_log(ctx, AV_LOG_WARNING,
               "amixrank: set_active slots %d-%d = %d\n", lo, hi, val ? 1 : 0);
        calculate_scales(s, 0);
        ff_filter_set_ready(ctx, 100);
        return 0;
    }

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "amixrank: unsupported/failed cmd='%s' args='%s' -> %s (%d)\n",
               cmd, args ? args : "", av_err2str(ret), ret);
        return ret;
    }
    parse_weights(ctx);
    for (int i = 0; i < s->nb_inputs; i++)
        s->scale_norm[i] = s->weights[i] != 0.f
                           ? s->weight_sum / FFABS(s->weights[i]) : 0.f;
    calculate_scales(s, 0);
    if (s->sample_rate > 0) {
        amixrank_recompute_latency(s);
        for (int i = 1; i < s->nb_inputs; i++) {
            amixrank_check_input_fifo(ctx, i);
        }
    }
    av_log(ctx, AV_LOG_WARNING,
           "amixrank: runtime opt '%s' applied args='%s'\n",
           cmd, args ? args : "");
    return 0;
}

static const AVFilterPad amixrank_outputs[] = {
    { .name = "default", .type = AVMEDIA_TYPE_AUDIO, .config_props = config_output },
};

const FFFilter ff_af_amixrank = {
    .p.name         = "amixrank",
    .p.description  = NULL_IF_CONFIG_SMALL("Audio mixing with loudness ranking + ducking"),
    .p.priv_class   = &amixrank_class,
    .p.inputs       = NULL,
    .p.flags        = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .priv_size      = sizeof(AmixRankContext),
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    FILTER_OUTPUTS(amixrank_outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
                      AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP),
    .process_command = process_command,
};
