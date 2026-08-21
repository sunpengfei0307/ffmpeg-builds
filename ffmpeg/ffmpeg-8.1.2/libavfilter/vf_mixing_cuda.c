/*
 * Multi-input CUDA video mixer (layout JSON + inline scale + stall/collapse).
 * This file is part of FFmpeg.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "libavutil/avstring.h"
#include "libavutil/cuda_check.h"
#include "libavutil/dict.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

#include "avfilter.h"
#include "cuda/load_helper.h"
#include "filters.h"
#include "framesync.h"
#include "lavfi_zeromq.h"
#include "vf_mixing_cuda_layout.h"
#include "video.h"

#if CONFIG_LIBZMQ
#include <zmq.h>
#include <pthread.h>
#endif

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))
#define BLOCK_X 32
#define BLOCK_Y 16
#define MIXING_CUDA_MAX_INPUTS 32

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_P010,
};

static int format_is_supported(enum AVPixelFormat fmt)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

enum OnDisconnect {
    ON_DISC_FILL = 0,
    ON_DISC_COLLAPSE,
};

enum InterpMode {
    INTERP_BILINEAR = 0,
    INTERP_LANCZOS,
};

enum LayoutMode {
    LAYOUT_ADAPTIVE = 0,
    LAYOUT_FIXED,
    LAYOUT_SPEAKER,
};

enum SpeakerLayout {
    SPK_OBS = 0,
    SPK_GRID,
};

enum TransEffect {
    TRANS_NONE = 0,
    TRANS_FADE,
    TRANS_SLIDE_LEFT,
    TRANS_SLIDE_RIGHT,
    TRANS_SLIDE_UP,
    TRANS_SLIDE_DOWN,
    TRANS_FLY_LEFT,
    TRANS_FLY_RIGHT,
    TRANS_FLY_UP,
    TRANS_FLY_DOWN,
};

typedef struct MixingSlot {
    int input;
    float x, y, w, h;
    int z;
    int visible;
    int stalled;
    int border_px;          /* -1 = inherit tile_border_px */
    int radius_px;          /* -1 = inherit radius_px */
    uint8_t border_rgba[4];
    int has_border_color;   /* 1 = use border_rgba, else tile_border_color */
} MixingSlot;

typedef struct MixingInputState {
    int64_t last_pts;
    int64_t last_arrival_us;
    int64_t live_since_us;      /* when became continuously active (join debounce) */
    int     have_frame;
    int     holding;            /* 1=showing dyn_hold / empty+hold (use hold_stall_ms) */
    AVFrame *hold;              /* last non-empty frame for remove/reconnect gap */
    int64_t last_gen;           /* lavfi.dyn_gen from dynamic_input */
    int     trans_active;
    int64_t trans_start_us;
    AVFrame *trans_under;       /* underlay during tile transition */
    int     empty_streak;       /* consecutive empty (no hold) ticks */
    int     seen_real;          /* saw a real frame since open */
    int64_t last_sync_warn_us;
} MixingInputState;

typedef struct MixingCUDAContext {
    const AVClass *class;

    int nb_inputs;
    int out_w, out_h;
    char *size_str;
    char *format_str;
    enum AVPixelFormat sw_format;

    char *layout_str;
    char *layout_file;
    char *layout_cache;          /* last successfully applied JSON (owned) */
    int64_t layout_mtime;
    uint8_t fill_rgba[4];
    uint16_t fill_y, fill_u, fill_v;
    int on_disconnect;
    int stall_ms;
    int hold_stall_ms;          /* longer timeout while repeating last frame */
    int interp;
    int bg_input;

    MixingSlot slots[MIXING_CUDA_MAX_INPUTS];
    MixingSlot layout_slots[MIXING_CUDA_MAX_INPUTS];
    int nb_slots;
    int nb_layout_slots;
    int collapse_active;

    MixingInputState in_state[MIXING_CUDA_MAX_INPUTS];

    AVBufferRef *hw_frames_ctx;
    AVCUDADeviceContext *hwctx;
    CUmodule cu_module;
    CUfunction cu_fill_y;
    CUfunction cu_fill_uv;
    CUfunction cu_copy_y;
    CUfunction cu_copy_uv;
    CUfunction cu_blit_y;
    CUfunction cu_blit_uv;
    CUfunction cu_blit_alpha_y;
    CUfunction cu_blit_alpha_uv;
    CUfunction cu_blit_off_alpha_y;
    CUfunction cu_blit_off_alpha_uv;
    CUfunction cu_border_y;
    CUfunction cu_border_uv;
    CUstream cu_stream;

    int  trans_effect;                           /* TransEffect */
    int  trans_ms;                               /* transition duration */

    FFFrameSync fs;
    AVFrame *frames[MIXING_CUDA_MAX_INPUTS];

    /* ---- extensions: dynamic activation / speaker / zmq / CUevent ---- */
    int input_active[MIXING_CUDA_MAX_INPUTS];   /* logical active flags (set_active) */
    int primary_input;                           /* index driving output PTS (default 0) */
    int layout_mode;                             /* adaptive / fixed / speaker */
    int speaker_layout;                          /* obs / grid */
    char *rank_endpoint;                         /* zmq SUB endpoint; overridden by task_id */
    int  task_id;                                /* prefer ipc:///data/LCMS/sock/gain_<id>.sock */
    int  speaker_hold_ms;                         /* candidate hold time before switch */
    int  join_stable_ms;                          /* require stable live before layout join */
    float speaker_switch_db;                     /* loudness diff threshold to switch */
    int  speaker_switch;                         /* 1=follow amixrank incentive; 0=manual only */
    int  active_speaker;                         /* current main speaker input idx */
    int  speaker_candidate;                       /* candidate speaker idx */
    int64_t candidate_since_us;                  /* when candidate became candidate */
    float input_levels[MIXING_CUDA_MAX_INPUTS];  /* latest loudness per input (dB) */
    int  last_nb_active;                          /* cached active count (rebuild trigger) */
    int  last_speaker;                           /* cached speaker (rebuild trigger) */
    int  border_px;                              /* speaker highlight border width (0=off) */
    uint8_t border_rgba[4];
    uint16_t border_y, border_u, border_v;
    int  tile_border_px;                         /* default per-tile border (0=off) */
    uint8_t tile_border_rgba[4];
    uint16_t tile_border_y, tile_border_u, tile_border_v;
    int  radius_px;                              /* default per-tile corner radius */

#if CONFIG_LIBZMQ
    LavfiZmq *zmq_sub;                           /* IZeromq-style SUB */
    pthread_t zmq_thread;
    int    zmq_stop;
    pthread_mutex_t spk_lock;
    int    spk_active_speaker;
    float  spk_levels[MIXING_CUDA_MAX_INPUTS];
#endif

    CUevent cu_event;                            /* pipeline sync event */
    int cu_event_armed;                          /* whether a previous event is pending */

    int     sync_warn_interval_ms;               /* mix_sync warning throttle */
    int64_t mix_start_us;
} MixingCUDAContext;

/*
 * Clock source for FrameSync: prefer bg_input (color_cuda) so business pads
 * can appear/disappear without blocking the whole graph. primary_input is only
 * used when bg_input < 0.
 */
static int mixing_cuda_clock_input(const MixingCUDAContext *s)
{
    if (s->bg_input >= 0 && s->bg_input < s->nb_inputs)
        return s->bg_input;
    if (s->primary_input >= 0 && s->primary_input < s->nb_inputs)
        return s->primary_input;
    return 0;
}

/* Apply primary/secondary FrameSync roles after configure or clock change. */
static void apply_framesync_roles(MixingCUDAContext *s)
{
    int i, clock = mixing_cuda_clock_input(s);
    if (!s->fs.in)
        return;
    for (i = 0; i < s->nb_inputs; i++) {
        if (i == clock) {
            /* Never-ending plate drives output cadence. */
            s->fs.in[i].sync   = 1;
            s->fs.in[i].before = EXT_STOP;
            s->fs.in[i].after  = EXT_INFINITY;
        } else {
            /* Missing/stalled dynamic pads must not block compose. */
            s->fs.in[i].sync   = 0;
            s->fs.in[i].before = EXT_NULL;
            s->fs.in[i].after  = EXT_INFINITY;
        }
    }
}

static void rgb_to_yuv_bt709(uint8_t r, uint8_t g, uint8_t b,
                             enum AVPixelFormat sw_format,
                             uint16_t *y, uint16_t *u, uint16_t *v)
{
    int yi = (int)(0.2126 * r + 0.7152 * g + 0.0722 * b + 0.5);
    int ui = (int)(-0.1146 * r - 0.3854 * g + 0.5 * b + 128.5);
    int vi = (int)(0.5 * r - 0.4542 * g - 0.0458 * b + 128.5);
    if (sw_format == AV_PIX_FMT_P010) {
        /* 10-bit in high bits of little-endian uint16 (same as color_cuda). */
        *y = av_clip_uintp2(yi * 1023 / 255, 10) << 6;
        *u = av_clip_uintp2(ui * 1023 / 255, 10) << 6;
        *v = av_clip_uintp2(vi * 1023 / 255, 10) << 6;
    } else {
        *y = av_clip_uint8(yi);
        *u = av_clip_uint8(ui);
        *v = av_clip_uint8(vi);
    }
}

static void apply_fill_from_rgba(MixingCUDAContext *s)
{
    rgb_to_yuv_bt709(s->fill_rgba[0], s->fill_rgba[1], s->fill_rgba[2],
                     s->sw_format, &s->fill_y, &s->fill_u, &s->fill_v);
}

static void apply_border_from_rgba(MixingCUDAContext *s)
{
    rgb_to_yuv_bt709(s->border_rgba[0], s->border_rgba[1], s->border_rgba[2],
                     s->sw_format, &s->border_y, &s->border_u, &s->border_v);
}

static void apply_tile_border_from_rgba(MixingCUDAContext *s)
{
    rgb_to_yuv_bt709(s->tile_border_rgba[0], s->tile_border_rgba[1], s->tile_border_rgba[2],
                     s->sw_format, &s->tile_border_y, &s->tile_border_u, &s->tile_border_v);
}

static void mixing_slot_style_defaults(MixingSlot *sl)
{
    sl->border_px = -1;
    sl->radius_px = -1;
    sl->has_border_color = 0;
    sl->border_rgba[0] = sl->border_rgba[1] = sl->border_rgba[2] = 0;
    sl->border_rgba[3] = 255;
}

/* Resolve per-slot border/radius/color; speaker highlight may override color/width. */
static void resolve_slot_style(MixingCUDAContext *s, const MixingSlot *sl,
                               int *border_px, int *radius_px,
                               uint16_t *y, uint16_t *u, uint16_t *v)
{
    int bpx = sl->border_px >= 0 ? sl->border_px : s->tile_border_px;
    int rad = sl->radius_px >= 0 ? sl->radius_px : s->radius_px;
    uint16_t by, bu, bv;

    if (sl->has_border_color) {
        rgb_to_yuv_bt709(sl->border_rgba[0], sl->border_rgba[1], sl->border_rgba[2],
                         s->sw_format, &by, &bu, &bv);
    } else {
        by = s->tile_border_y;
        bu = s->tile_border_u;
        bv = s->tile_border_v;
    }

    /* Speaker highlight: thicken / recolor with global border_* when enabled. */
    if (sl->input == s->active_speaker && s->border_px > 0) {
        if (bpx < s->border_px)
            bpx = s->border_px;
        by = s->border_y;
        bu = s->border_u;
        bv = s->border_v;
    }

    *border_px = bpx;
    *radius_px = rad > 0 ? rad : 0;
    *y = by;
    *u = bu;
    *v = bv;
}

static int parse_bg_color(MixingCUDAContext *s, const char *bg, void *log)
{
    uint8_t rgba[4];
    if (!bg || !*bg)
        return 0;
    if (av_parse_color(rgba, bg, -1, log) < 0)
        return AVERROR(EINVAL);
    s->fill_rgba[0] = rgba[0];
    s->fill_rgba[1] = rgba[1];
    s->fill_rgba[2] = rgba[2];
    s->fill_rgba[3] = rgba[3];
    apply_fill_from_rgba(s);
    return 0;
}

static int64_t file_size_marker(const char *path)
{
    FILE *f;
    long size;
    if (!path || !*path)
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    fclose(f);
    return size > 0 ? (int64_t)size : 0;
}

static const char *json_find_key(const char *p, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return p ? av_stristr(p, pat) : NULL;
}

static int json_read_number_after(const char *p, const char *key, double *out)
{
    const char *k = json_find_key(p, key);
    const char *c;
    char *end = NULL;
    if (!k)
        return -1;
    c = strchr(k, ':');
    if (!c)
        return -1;
    c++;
    while (*c == ' ' || *c == '\t')
        c++;
    *out = strtod(c, &end);
    return end == c ? -1 : 0;
}

static int json_read_int_after(const char *p, const char *key, int *out)
{
    double d;
    if (json_read_number_after(p, key, &d) < 0)
        return -1;
    *out = (int)d;
    return 0;
}

static int json_read_string_after(const char *p, const char *key, char *buf, int buf_size)
{
    const char *k = json_find_key(p, key);
    const char *c, *e;
    int n;
    if (!k)
        return -1;
    c = strchr(k, ':');
    if (!c)
        return -1;
    c++;
    while (*c == ' ' || *c == '\t')
        c++;
    if (*c != '"')
        return -1;
    c++;
    e = strchr(c, '"');
    if (!e)
        return -1;
    n = FFMIN((int)(e - c), buf_size - 1);
    memcpy(buf, c, n);
    buf[n] = 0;
    return 0;
}

/*
 * Default equal gallery over BUSINESS pads only. bg_input is the full-frame
 * plate and must never become a tile.
 */
static void apply_default_grid(MixingCUDAContext *s, int n)
{
    MixingLayoutRect rects[MIXING_LAYOUT_MAX];
    int business[MIXING_CUDA_MAX_INPUTS];
    int nb_business = 0, i, use, nr;

    for (i = 0; i < s->nb_inputs && nb_business < MIXING_CUDA_MAX_INPUTS; i++) {
        if (i == s->bg_input)
            continue;
        business[nb_business++] = i;
    }
    if (nb_business == 0)
        return;

    use = FFMIN(FFMIN(n, nb_business), MIXING_LAYOUT_MAX);
    nr = ff_mixing_layout_equal(use, s->out_w, s->out_h, rects);
    if (nr <= 0)
        return;

    s->nb_slots = 0;
    for (i = 0; i < nr; i++) {
        MixingSlot *sl = &s->slots[s->nb_slots++];
        memset(sl, 0, sizeof(*sl));
        mixing_slot_style_defaults(sl);
        sl->input = business[i];
        sl->x = rects[i].x;
        sl->y = rects[i].y;
        sl->w = rects[i].w;
        sl->h = rects[i].h;
        sl->z = i;
        sl->visible = 1;
        sl->stalled = 0;
    }
}

/* Fixed-layout JSON must place at least one business pad with positive size. */
static int layout_slots_valid(const MixingCUDAContext *s)
{
    int i, business = 0;
    if (s->nb_layout_slots <= 0)
        return 0;
    for (i = 0; i < s->nb_layout_slots; i++) {
        const MixingSlot *sl = &s->layout_slots[i];
        if (sl->input < 0 || sl->input >= s->nb_inputs)
            return 0;
        if (!(sl->w > 0.f) || !(sl->h > 0.f))
            return 0;
        if (sl->input != s->bg_input)
            business++;
    }
    return business > 0;
}

static void apply_default_layout_fallback(AVFilterContext *ctx, const char *reason)
{
    MixingCUDAContext *s = ctx->priv;
    int j, n_business = 0;

    for (j = 0; j < s->nb_inputs; j++)
        if (j != s->bg_input)
            n_business++;
    apply_default_grid(s, FFMIN(n_business, MIXING_LAYOUT_MAX));
    for (j = 0; j < s->nb_slots && j < MIXING_CUDA_MAX_INPUTS; j++)
        s->layout_slots[j] = s->slots[j];
    s->nb_layout_slots = s->nb_slots;
    s->collapse_active = 0;
    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: layout invalid (%s) ...using default %d-slot business grid "
           "(skip bg_input=%d)\n",
           reason ? reason : "unknown", s->nb_slots, s->bg_input);
}

/*
 * apply_active_grid: adaptive equal gallery (1..MIXING_LAYOUT_MAX).
 * Only active inputs are placed; inactive ones skipped (fill rendered).
 */
static void apply_active_grid(MixingCUDAContext *s, const int *alive, int n_alive)
{
    MixingLayoutRect rects[MIXING_LAYOUT_MAX];
    int n = FFMIN(FFMAX(n_alive, 1), MIXING_LAYOUT_MAX);
    int nr, i;

    nr = ff_mixing_layout_equal(n, s->out_w, s->out_h, rects);
    s->nb_slots = 0;
    for (i = 0; i < nr && i < n_alive; i++) {
        MixingSlot *sl = &s->slots[s->nb_slots++];
        memset(sl, 0, sizeof(*sl));
        mixing_slot_style_defaults(sl);
        sl->input = alive[i];
        sl->x = rects[i].x;
        sl->y = rects[i].y;
        sl->w = rects[i].w;
        sl->h = rects[i].h;
        sl->z = i;
        sl->visible = 1;
        sl->stalled = 0;
    }
}

/*
 * build_speaker_layout: spotlight set (separate from equal gallery).
 * rects[0]=main; remaining=thumbs. Map speaker ->main, others in alive order.
 */
static void build_speaker_layout(MixingCUDAContext *s, int speaker,
                                  const int *alive, int n_alive)
{
    MixingLayoutRect rects[MIXING_LAYOUT_MAX];
    int n = FFMIN(n_alive, MIXING_LAYOUT_MAX);
    int i, ti, speaker_alive = 0, nr, style;

    s->nb_slots = 0;
    if (n == 0)
        return;

    for (i = 0; i < n; i++) {
        if (alive[i] == speaker) {
            speaker_alive = 1;
            break;
        }
    }
    if (!speaker_alive)
        speaker = alive[0];

    style = (s->speaker_layout == SPK_OBS) ? 0 : 1;
    nr = ff_mixing_layout_speaker(n, style, s->out_w, s->out_h, rects);
    if (nr <= 0)
        return;

    {
        MixingSlot *sp = &s->slots[s->nb_slots++];
        memset(sp, 0, sizeof(*sp));
        mixing_slot_style_defaults(sp);
        sp->input = speaker;
        sp->x = rects[0].x;
        sp->y = rects[0].y;
        sp->w = rects[0].w;
        sp->h = rects[0].h;
        sp->z = 0;
        sp->visible = 1;
        sp->stalled = 0;
    }
    ti = 1;
    for (i = 0; i < n && ti < nr; i++) {
        MixingSlot *sp;
        if (alive[i] == speaker)
            continue;
        sp = &s->slots[s->nb_slots++];
        memset(sp, 0, sizeof(*sp));
        mixing_slot_style_defaults(sp);
        sp->input = alive[i];
        sp->x = rects[ti].x;
        sp->y = rects[ti].y;
        sp->w = rects[ti].w;
        sp->h = rects[ti].h;
        sp->z = ti;
        sp->visible = 1;
        sp->stalled = 0;
        ti++;
    }
}

#if CONFIG_LIBZMQ
/* zmq SUB background thread: recv loudness rank JSON via lavfi_zeromq. */
static void *mixing_cuda_zmq_thread(void *arg)
{
    AVFilterContext *ctx = arg;
    MixingCUDAContext *s = ctx->priv;
    while (!s->zmq_stop) {
        char buf[2048];
        int n = lavfi_zmq_recv(s->zmq_sub, buf, sizeof(buf) - 1, 50);
        if (n == AVERROR(EAGAIN))
            continue;
        if (n < 0)
            break;
        buf[n] = 0;
        /* parse {"rank":[i,j,...],"levels":[db,...]} ...levels[i]=loudness of input i */
        {
            const char *rank = av_stristr(buf, "\"rank\"");
            const char *levels = av_stristr(buf, "\"levels\"");
            int order[MIXING_CUDA_MAX_INPUTS];
            float lv[MIXING_CUDA_MAX_INPUTS];
            int rcnt = 0, lcnt = 0, ok = 0;
            const char *p;
            if (rank) {
                rank = strchr(rank, '[');
                if (rank) {
                    p = rank + 1;
                    while (rcnt < MIXING_CUDA_MAX_INPUTS) {
                        int val;
                        if (sscanf(p, " %d", &val) != 1)
                            break;
                        order[rcnt++] = val;
                        while (*p && *p == ' ') p++;
                        while (*p && (*p >= '0' && *p <= '9' || *p == '-')) p++;
                        while (*p && (*p == ' ' || *p == ',')) p++;
                        if (*p == ']') { ok = 1; break; }
                    }
                }
            }
            if (levels) {
                levels = strchr(levels, '[');
                if (levels) {
                    p = levels + 1;
                    while (lcnt < MIXING_CUDA_MAX_INPUTS) {
                        float val;
                        if (sscanf(p, " %f", &val) != 1)
                            break;
                        lv[lcnt++] = val;
                        while (*p && *p == ' ') p++;
                        while (*p && (*p >= '0' && *p <= '9' || *p == '-' || *p == '.')) p++;
                        while (*p && (*p == ' ' || *p == ',')) p++;
                        if (*p == ']') break;
                    }
                }
            }
            if (ok && rcnt > 0 && order[0] >= 0 && order[0] < MIXING_CUDA_MAX_INPUTS) {
                pthread_mutex_lock(&s->spk_lock);
                s->spk_active_speaker = order[0];
                for (int k = 0; k < lcnt; k++)
                    s->spk_levels[k] = lv[k];
                pthread_mutex_unlock(&s->spk_lock);
            }
        }
    }
    return NULL;
} 
#endif

static int parse_layout_json(MixingCUDAContext *s, const char *json, void *log)
{
    const char *videos, *p, *end;
    char od[32] = {0};
    char bg[64] = {0};
    double cw = 0, ch = 0;

    if (!json || !*json)
        return 0;

    if (json_read_string_after(json, "on_disconnect", od, sizeof(od)) == 0) {
        if (!av_strcasecmp(od, "collapse"))
            s->on_disconnect = ON_DISC_COLLAPSE;
        else
            s->on_disconnect = ON_DISC_FILL;
    }

    if (json_read_string_after(json, "bg", bg, sizeof(bg)) == 0)
        parse_bg_color(s, bg, log);

    {
        const char *canvas = json_find_key(json, "canvas");
        const char *cend;
        if (canvas) {
            canvas = strchr(canvas, '{');
            if (canvas) {
                cend = strchr(canvas, '}');
                if (cend) {
                    char tmp[256];
                    int n = FFMIN((int)(cend - canvas + 1), (int)sizeof(tmp) - 1);
                    memcpy(tmp, canvas, n);
                    tmp[n] = 0;
                    if (json_read_number_after(tmp, "w", &cw) == 0 && cw > 0)
                        s->out_w = (int)cw;
                    if (json_read_number_after(tmp, "h", &ch) == 0 && ch > 0)
                        s->out_h = (int)ch;
                }
            }
        }
    }

    /* audios[] parsed for forward-compat; not used for video mixing */
    (void)json_find_key(json, "audios");

    videos = json_find_key(json, "videos");
    if (!videos) {
        av_log(log, AV_LOG_WARNING, "mixing_cuda: layout JSON missing videos[]\n");
        return AVERROR(EINVAL);
    }
    videos = strchr(videos, '[');
    if (!videos) {
        av_log(log, AV_LOG_WARNING, "mixing_cuda: layout videos[] is not an array\n");
        return AVERROR(EINVAL);
    }
    end = strchr(videos, ']');
    if (!end)
        end = videos + strlen(videos);

    s->nb_layout_slots = 0;
    p = videos + 1;
    while (p < end && s->nb_layout_slots < MIXING_CUDA_MAX_INPUTS) {
        const char *obj = strchr(p, '{');
        const char *obj_end;
        MixingSlot sl = {0};
        double d;
        char color[64];
        int tmpi;
        if (!obj || obj >= end)
            break;
        obj_end = strchr(obj, '}');
        if (!obj_end || obj_end > end)
            break;

        mixing_slot_style_defaults(&sl);
        sl.visible = 1;
        if (json_read_int_after(obj, "input", &sl.input) < 0) {
            /* Skip bg_input: slot order maps to business pads [bg][v0]... ->1,2,3...*/
            int bi = 0, k;
            for (k = 0; k < s->nb_inputs; k++) {
                if (k == s->bg_input)
                    continue;
                if (bi == s->nb_layout_slots) {
                    sl.input = k;
                    break;
                }
                bi++;
            }
            if (k >= s->nb_inputs)
                sl.input = s->nb_layout_slots;
        }
        if (json_read_number_after(obj, "x", &d) == 0) sl.x = (float)d;
        if (json_read_number_after(obj, "y", &d) == 0) sl.y = (float)d;
        if (json_read_number_after(obj, "w", &d) == 0) sl.w = (float)d;
        if (json_read_number_after(obj, "h", &d) == 0) sl.h = (float)d;
        json_read_int_after(obj, "z", &sl.z);
        json_read_int_after(obj, "visible", &sl.visible);
        if (json_read_int_after(obj, "border_px", &tmpi) == 0)
            sl.border_px = tmpi;
        if (json_read_int_after(obj, "radius_px", &tmpi) == 0)
            sl.radius_px = tmpi;
        else if (json_read_int_after(obj, "radius", &tmpi) == 0)
            sl.radius_px = tmpi;
        if (json_read_string_after(obj, "border_color", color, sizeof(color)) == 0) {
            uint8_t rgba[4];
            if (av_parse_color(rgba, color, -1, log) >= 0) {
                memcpy(sl.border_rgba, rgba, 4);
                sl.has_border_color = 1;
            }
        }

        if (sl.input < 0 || sl.input >= s->nb_inputs) {
            av_log(log, AV_LOG_WARNING, "mixing_cuda: skip slot input=%d\n", sl.input);
        } else {
            s->layout_slots[s->nb_layout_slots++] = sl;
        }
        p = obj_end + 1;
    }

    /*
     * Common pitfall: layout.json uses input 0..N-1 for tiles while the
     * filtergraph is [bg][v0][v1]... with bg_input=0. Compose already skips
     * blitting bg into a cell, so that tile stays the plate color (looks like
     * "bg stole the first pane"). Remap indices up so business pads fill the grid.
     */
    if (s->bg_input >= 0 && s->nb_layout_slots > 0) {
        int refs_bg = 0, i;
        for (i = 0; i < s->nb_layout_slots; i++)
            if (s->layout_slots[i].input == s->bg_input)
                refs_bg = 1;
        if (refs_bg) {
            av_log(log, AV_LOG_WARNING,
                   "mixing_cuda: layout tile(s) reference bg_input=%d ..."
                   "remapping input>=%d to input+1 so [bg][v0]... fills the grid. "
                   "Prefer writing input:%d.. in layout.json\n",
                   s->bg_input, s->bg_input, s->bg_input + 1);
            for (i = 0; i < s->nb_layout_slots; ) {
                if (s->layout_slots[i].input >= s->bg_input)
                    s->layout_slots[i].input++;
                if (s->layout_slots[i].input < 0 ||
                    s->layout_slots[i].input >= s->nb_inputs) {
                    av_log(log, AV_LOG_ERROR,
                           "mixing_cuda: drop slot after bg remap input=%d "
                           "(nb_inputs=%d)\n",
                           s->layout_slots[i].input, s->nb_inputs);
                    memmove(&s->layout_slots[i], &s->layout_slots[i + 1],
                            (s->nb_layout_slots - i - 1) * sizeof(s->layout_slots[0]));
                    s->nb_layout_slots--;
                    continue;
                }
                i++;
            }
        }
    }

    if (s->nb_layout_slots > 0) {
        int nb = FFMIN(s->nb_layout_slots, MIXING_CUDA_MAX_INPUTS);
        for (int i = 0; i < nb; i++)
            s->slots[i] = s->layout_slots[i];
        s->nb_slots = nb;
        s->collapse_active = 0;
    }

    if (!layout_slots_valid(s)) {
        av_log(log, AV_LOG_WARNING,
               "mixing_cuda: parsed layout has no usable business slots "
               "(need input鈮燽g_input with w/h>0)\n");
        return AVERROR(EINVAL);
    }

    av_log(log, AV_LOG_INFO, "mixing_cuda: parsed %d slots, canvas %dx%d, on_disconnect=%s\n",
           s->nb_slots, s->out_w, s->out_h,
           s->on_disconnect == ON_DISC_COLLAPSE ? "collapse" : "fill");
    return 0;
}

#define MIXING_LAYOUT_DIR "/data/LCMS/layouts"

static int mixing_cuda_resolve_task_id(MixingCUDAContext *s)
{
    const char *e;
    if (s->task_id > 0)
        return s->task_id;
    e = getenv("FFMPEG_TASK_ID");
    if (e && *e)
        return atoi(e);
    return 0;
}

static int mixing_cuda_task_layout_path(MixingCUDAContext *s, char *buf, size_t buflen)
{
    int tid = mixing_cuda_resolve_task_id(s);
    if (tid <= 0 || !buf || buflen < 64)
        return AVERROR(EINVAL);
    snprintf(buf, buflen, MIXING_LAYOUT_DIR "/%d.layout", tid);
    return 0;
}

static int mixing_cuda_mkdir_p_layouts(void *log)
{
#ifdef _WIN32
    (void)log;
    return 0;
#else
    if (mkdir("/data/LCMS", 0755) < 0 && errno != EEXIST) {
        av_log(log, AV_LOG_WARNING, "mixing_cuda: mkdir /data/LCMS failed: %s\n",
               strerror(errno));
        return AVERROR(errno);
    }
    if (mkdir(MIXING_LAYOUT_DIR, 0755) < 0 && errno != EEXIST) {
        av_log(log, AV_LOG_WARNING, "mixing_cuda: mkdir %s failed: %s\n",
               MIXING_LAYOUT_DIR, strerror(errno));
        return AVERROR(errno);
    }
    return 0;
#endif
}

/* Persist successful layout JSON for next start/restart. */
static int mixing_cuda_persist_layout(AVFilterContext *ctx, const char *json)
{
    MixingCUDAContext *s = ctx->priv;
    char path[256];
    FILE *f;
    size_t n, nw;

    if (!json || !*json)
        return 0;
    if (mixing_cuda_task_layout_path(s, path, sizeof(path)) < 0) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: skip layout persist (no task_id / FFMPEG_TASK_ID)\n");
        return 0;
    }
    if (mixing_cuda_mkdir_p_layouts(ctx) < 0)
        return AVERROR(EIO);
    f = fopen(path, "wb");
    if (!f) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: cannot write layout persist '%s'\n", path);
        return AVERROR(EIO);
    }
    n = strlen(json);
    nw = fwrite(json, 1, n, f);
    fclose(f);
    if (nw != n) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: short write layout persist '%s'\n", path);
        return AVERROR(EIO);
    }
    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: layout persisted ->%s (%zu bytes)\n", path, n);
    return 0;
}

static int mixing_cuda_apply_layout_json(AVFilterContext *ctx, const char *json,
                                         int persist)
{
    MixingCUDAContext *s = ctx->priv;
    char *copy;
    int ret;

    if (!json || !*json)
        return AVERROR(EINVAL);
    ret = parse_layout_json(s, json, ctx);
    if (ret < 0)
        return ret;
    copy = av_strdup(json);
    if (!copy)
        return AVERROR(ENOMEM);
    av_freep(&s->layout_cache);
    s->layout_cache = copy;
    s->last_nb_active = -1;
    s->last_speaker = -1;
    if (persist)
        mixing_cuda_persist_layout(ctx, json);
    return 0;
}

/*
 * adapt + template: N=min(alive,M); alive>M discard extras; alive<M shrink to N
 * largest cells. speaker_switch=1 ->main speaker in largest/frontmost cell.
 */
static void apply_adapt_from_template(AVFilterContext *ctx, MixingCUDAContext *s,
                                      const int *alive, int n_alive)
{
    int order[MIXING_CUDA_MAX_INPUTS];
    int cell[MIXING_CUDA_MAX_INPUTS];
    int pads[MIXING_CUDA_MAX_INPUTS];
    int M = s->nb_layout_slots;
    int N, i, j, main_i, sp, have_sp;

    s->nb_slots = 0;
    if (M <= 0 || n_alive <= 0)
        return;

    N = FFMIN(n_alive, M);

    for (i = 0; i < M; i++)
        order[i] = i;
    /* area desc, then smaller z, then lower index */
    for (i = 0; i < M; i++) {
        for (j = i + 1; j < M; j++) {
            MixingSlot *a = &s->layout_slots[order[i]];
            MixingSlot *b = &s->layout_slots[order[j]];
            float aa = a->w * a->h, bb = b->w * b->h;
            int swap = 0;
            if (bb > aa + 1e-6f)
                swap = 1;
            else if (bb >= aa - 1e-6f && bb <= aa + 1e-6f) {
                if (b->z < a->z || (b->z == a->z && order[j] < order[i]))
                    swap = 1;
            }
            if (swap)
                FFSWAP(int, order[i], order[j]);
        }
    }

    if (n_alive < M) {
        /* shrink: keep N largest cells; cell[0] is main */
        for (i = 0; i < N; i++)
            cell[i] = order[i];
    } else {
        /* full / discard: keep JSON order for stable positions */
        for (i = 0; i < N; i++)
            cell[i] = i;
    }

    for (i = 0; i < N; i++)
        pads[i] = alive[i];

    sp = s->active_speaker;
    have_sp = 0;
    if (s->speaker_switch && sp >= 0 && sp != s->bg_input) {
        for (i = 0; i < n_alive; i++) {
            if (alive[i] == sp) {
                have_sp = 1;
                break;
            }
        }
    }
    if (have_sp) {
        int k = 0;
        pads[k++] = sp;
        for (i = 0; i < n_alive && k < N; i++) {
            if (alive[i] != sp)
                pads[k++] = alive[i];
        }
    }

    /* main cell index within cell[] */
    main_i = 0;
    if (n_alive >= M) {
        float best = s->layout_slots[cell[0]].w * s->layout_slots[cell[0]].h;
        for (i = 1; i < N; i++) {
            float a = s->layout_slots[cell[i]].w * s->layout_slots[cell[i]].h;
            MixingSlot *bi = &s->layout_slots[cell[i]];
            MixingSlot *bm = &s->layout_slots[cell[main_i]];
            if (a > best + 1e-6f ||
                (a >= best - 1e-6f && (bi->z < bm->z ||
                 (bi->z == bm->z && cell[i] < cell[main_i])))) {
                best = a;
                main_i = i;
            }
        }
    }

    for (i = 0; i < N; i++) {
        s->slots[i] = s->layout_slots[cell[i]];
        s->slots[i].visible = 1;
        s->slots[i].stalled = 0;
    }
    if (have_sp) {
        int pi = 1;
        s->slots[main_i].input = pads[0];
        for (i = 0; i < N; i++) {
            if (i == main_i)
                continue;
            s->slots[i].input = pads[pi++];
        }
    } else {
        for (i = 0; i < N; i++)
            s->slots[i].input = pads[i];
    }
    s->nb_slots = N;
    if (n_alive > M)
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: adapt discard %d extra live pad(s) (template=%d)\n",
               n_alive - M, M);
}

static int load_layout_file(AVFilterContext *ctx)
{
    MixingCUDAContext *s = ctx->priv;
    FILE *f;
    uint8_t *buf = NULL;
    long buf_size = 0;
    int64_t marker;
    int ret;
    size_t nread;

    if (!s->layout_file || !*s->layout_file)
        return 0;

    marker = file_size_marker(s->layout_file);
    if (marker && marker == s->layout_mtime)
        return 0;

    f = fopen(s->layout_file, "rb");
    if (!f) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: cannot open layout_file '%s'\n", s->layout_file);
        if (!layout_slots_valid(s))
            apply_default_layout_fallback(ctx, "layout_file open failed");
        return AVERROR(EIO);
    }
    if (fseek(f, 0, SEEK_END) < 0) {
        fclose(f);
        return AVERROR(EIO);
    }
    buf_size = ftell(f);
    if (buf_size <= 0 || buf_size > (1 << 20)) {
        fclose(f);
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: layout_file '%s' empty or too large\n", s->layout_file);
        if (!layout_slots_valid(s))
            apply_default_layout_fallback(ctx, "layout_file bad size");
        return AVERROR(EINVAL);
    }
    rewind(f);
    buf = av_malloc(buf_size + 1);
    if (!buf) {
        fclose(f);
        return AVERROR(ENOMEM);
    }
    nread = fread(buf, 1, buf_size, f);
    fclose(f);
    if ((long)nread != buf_size) {
        av_free(buf);
        if (!layout_slots_valid(s))
            apply_default_layout_fallback(ctx, "layout_file read failed");
        return AVERROR(EIO);
    }
    buf[buf_size] = 0;

    ret = mixing_cuda_apply_layout_json(ctx, (char *)buf, 1);
    av_free(buf);
    if (ret < 0) {
        apply_default_layout_fallback(ctx, "layout JSON invalid");
        /* Still mark mtime so we do not spin-fail every frame on a bad file. */
        s->layout_mtime = marker ? marker : av_gettime_relative();
        return ret;
    }
    s->layout_mtime = marker ? marker : av_gettime_relative();
    return 0;
}

static int slot_cmp_z(const void *a, const void *b)
{
    const MixingSlot *sa = a, *sb = b;
    return sa->z - sb->z;
}

/* dynamic_input emits lavfi.dyn_empty=1 placeholders so FrameSync can advance
 * when RTMP is down; those must not count as live content. */
static int frame_is_dyn_empty(const AVFrame *fr)
{
    return fr && fr->metadata &&
           av_dict_get(fr->metadata, "lavfi.dyn_empty", NULL, 0) != NULL;
}

/* Repeated last-good frame (decode gap / stalled demux). Still blit, but do
 * not refresh last_arrival ...otherwise advancing program PTS keeps the pad
 * "alive" forever on a frozen picture after disconnect/reconnect. */
static int frame_is_dyn_hold(const AVFrame *fr)
{
    return fr && fr->metadata &&
           av_dict_get(fr->metadata, "lavfi.dyn_hold", NULL, 0) != NULL;
}

/* Anomaly-only mix_sync (OK pads stay silent). */
static void mix_sync_warn(AVFilterContext *ctx, MixingCUDAContext *s, int idx,
                          const char *code, int empty_streak)
{
    MixingInputState *st;
    int64_t now;
    int interval;

    if (idx < 0 || idx >= s->nb_inputs || !code)
        return;
    st = &s->in_state[idx];
    now = av_gettime_relative();
    interval = FFMAX(s->sync_warn_interval_ms, 1000);
    if (now - st->last_sync_warn_us < (int64_t)interval * 1000)
        return;
    st->last_sync_warn_us = now;

    if (!strcmp(code, "NO_REAL"))
        av_log(ctx, AV_LOG_WARNING,
               "mix_sync: pad%d IMBALANCE code=NO_REAL empty_streak=%d | "
               "cause: business pad expected live content but only empty placeholders | "
               "fix: check dynamic_input reconnect/opened/demux for this slot; check network\n",
               idx, empty_streak);
    else if (!strcmp(code, "EMPTY_SPIKE"))
        av_log(ctx, AV_LOG_WARNING,
               "mix_sync: pad%d IMBALANCE code=EMPTY_SPIKE empty_streak=%d | "
               "cause: sudden dyn_empty burst without hold picture | "
               "fix: correlate dyn_sync for same pad; check disconnect/reconnect\n",
               idx, empty_streak);
    else
        av_log(ctx, AV_LOG_WARNING,
               "mix_sync: pad%d IMBALANCE code=%s | cause: video pad imbalance | "
               "fix: see docs/mixing_cuda.md AV sync section\n",
               idx, code);
}

static void mix_sync_observe_pad(AVFilterContext *ctx, MixingCUDAContext *s,
                                 int idx, const AVFrame *fr_in, int64_t now)
{
    MixingInputState *st;
    int clock, is_empty, is_hold, is_real;
    int64_t age_ms;

    if (idx < 0 || idx >= s->nb_inputs)
        return;
    clock = mixing_cuda_clock_input(s);
    if (idx == clock || idx == s->bg_input)
        return; /* program clock / plate ...not a business AV pad */

    st = &s->in_state[idx];
    /* Startup grace: 10s after mix start. */
    if (s->mix_start_us > 0 && now - s->mix_start_us < 10LL * 1000000)
        return;

    is_empty = frame_is_dyn_empty(fr_in);
    is_hold  = frame_is_dyn_hold(fr_in) || st->holding;
    is_real  = fr_in && !is_empty && !frame_is_dyn_hold(fr_in) && !st->holding;

    if (is_real) {
        st->seen_real = 1;
        st->empty_streak = 0;
        return;
    }
    if (is_hold) {
        /* Idle / remove / reconnect / empty-slot hold is expected.
         * last_arrival is intentionally not refreshed on hold, so aging
         * forever must NOT become STARVE spam. Live decode stalls are
         * reported by dynamic_input (dyn_sync). */
        st->empty_streak = 0;
        return;
    }
    if (is_empty || !fr_in) {
        st->empty_streak++;
        if (!st->seen_real || st->last_arrival_us == AV_NOPTS_VALUE)
            return;
        /* Long-idle empty after a former live pad = unused slot, silent. */
        age_ms = (now - st->last_arrival_us) / 1000;
        if (s->hold_stall_ms > 0 && age_ms > s->hold_stall_ms * 4LL)
            return;
        if (st->empty_streak >= 50)
            mix_sync_warn(ctx, s, idx, "NO_REAL", st->empty_streak);
        else if (st->empty_streak >= 15)
            mix_sync_warn(ctx, s, idx, "EMPTY_SPIKE", st->empty_streak);
    }
}

static int64_t frame_dyn_gen(const AVFrame *fr)
{
    AVDictionaryEntry *e;
    if (!fr || !fr->metadata)
        return AV_NOPTS_VALUE;
    e = av_dict_get(fr->metadata, "lavfi.dyn_gen", NULL, 0);
    if (!e || !e->value)
        return AV_NOPTS_VALUE;
    return strtoll(e->value, NULL, 10);
}

static void mixing_trans_end(MixingInputState *st)
{
    st->trans_active = 0;
    st->trans_start_us = 0;
    av_frame_free(&st->trans_under);
}

static float mixing_trans_progress(MixingCUDAContext *s, MixingInputState *st,
                                   int64_t now)
{
    int64_t dur;
    if (!st->trans_active || s->trans_ms <= 0)
        return 1.f;
    dur = (int64_t)s->trans_ms * 1000;
    if (dur <= 0)
        return 1.f;
    if (now <= st->trans_start_us)
        return 0.f;
    if (now - st->trans_start_us >= dur)
        return 1.f;
    return (float)(now - st->trans_start_us) / (float)dur;
}

/* Call BEFORE replacing st->hold so underlay keeps the previous picture. */
static void mixing_trans_maybe_start(AVFilterContext *ctx, MixingCUDAContext *s,
                                     int input, AVFrame *fr, int64_t now)
{
    MixingInputState *st;
    int64_t gen;

    if (input < 0 || input >= s->nb_inputs)
        return;
    if (input == s->bg_input)
        return;
    st = &s->in_state[input];
    if (!fr || frame_is_dyn_empty(fr) || frame_is_dyn_hold(fr))
        return;
    gen = frame_dyn_gen(fr);
    if (gen == AV_NOPTS_VALUE)
        return;
    if (st->last_gen != AV_NOPTS_VALUE && gen == st->last_gen)
        return;

    /* Always track generation; animate only when effect is enabled. */
    if (s->trans_effect == TRANS_NONE || s->trans_ms <= 0) {
        st->last_gen = gen;
        return;
    }

    mixing_trans_end(st);
    if (st->hold)
        st->trans_under = av_frame_clone(st->hold);
    st->trans_active = 1;
    st->trans_start_us = now;
    st->last_gen = gen;
    av_log(ctx, AV_LOG_INFO,
           "mixing_cuda: tile transition start input=%d gen=%"PRId64
           " effect=%d ms=%d under=%d\n",
           input, gen, s->trans_effect, s->trans_ms, st->trans_under != NULL);
}

static int input_is_active(MixingCUDAContext *s, int input, int64_t now)
{
    MixingInputState *st = &s->in_state[input];
    int limit_ms;

    if (!st->have_frame)
        return 0;
    if (st->last_arrival_us == AV_NOPTS_VALUE)
        return 1;
    /* Fresh decode ->stall_ms. Held/empty-with-hold ->hold_stall_ms so
     * multi-pull RTMP / post-prime keyframe waits don't black out thumbs. */
    if (st->holding)
        limit_ms = s->hold_stall_ms > 0 ? s->hold_stall_ms : s->stall_ms;
    else
        limit_ms = s->stall_ms;
    if (limit_ms > 0 &&
        now - st->last_arrival_us > (int64_t)limit_ms * 1000)
        return 0;
    return 1;
}

/* For adaptive/speaker grid membership: debounce cold join after remove. */
static int input_is_stable_alive(MixingCUDAContext *s, int input, int64_t now)
{
    MixingInputState *st = &s->in_state[input];
    if (!input_is_active(s, input, now)) {
        st->live_since_us = AV_NOPTS_VALUE;
        return 0;
    }
    if (st->live_since_us == AV_NOPTS_VALUE)
        st->live_since_us = now;
    if (s->join_stable_ms > 0 &&
        now - st->live_since_us < (int64_t)s->join_stable_ms * 1000)
        return 0;
    return 1;
}

static void rebuild_active_slots(AVFilterContext *ctx, MixingCUDAContext *s, int64_t now)
{
    int alive[MIXING_CUDA_MAX_INPUTS];
    int nb_alive = 0;
    int i, need_collapse = 0;
    int nb_active_logical = 0;

    /* fixed needs JSON/default slots; speaker/adaptive build their own geometry. */
    if (s->layout_mode == LAYOUT_FIXED && s->nb_layout_slots == 0)
        return;

    /* gather alive BUSINESS inputs only. bg_input is the full-frame plate
     * (color_cuda), never a layout tile ...otherwise "no input" shows bg only
     * in the first cell and fill/black in the rest. */
    for (i = 0; i < s->nb_inputs; i++) {
        if (i == s->bg_input)
            continue;
        if (!s->input_active[i])
            continue;
        nb_active_logical++;
        /* Fixed layout blits as soon as active; adaptive/speaker wait join_stable. */
        if (s->layout_mode == LAYOUT_FIXED ? input_is_active(s, i, now)
                                           : input_is_stable_alive(s, i, now)) {
            if (nb_alive < MIXING_CUDA_MAX_INPUTS)
                alive[nb_alive++] = i;
        }
    }

    /* Keep FrameSync clock on bg plate; never promote a flaky business pad to clock. */
    apply_framesync_roles(s);

    /* Incentive / active_speaker update (adapt + legacy speaker; also for fixed border). */
    if (s->speaker_switch || s->layout_mode != LAYOUT_FIXED) {
        int cand = s->active_speaker;
        int speaker_alive = 0;
#if CONFIG_LIBZMQ
        if (s->speaker_switch && s->zmq_sub) {
            pthread_mutex_lock(&s->spk_lock);
            cand = s->spk_active_speaker;
            for (i = 0; i < MIXING_CUDA_MAX_INPUTS; i++)
                s->input_levels[i] = s->spk_levels[i];
            pthread_mutex_unlock(&s->spk_lock);
        }
#endif
        for (i = 0; i < nb_alive; i++)
            if (alive[i] == s->active_speaker) {
                speaker_alive = 1;
                break;
            }
        {
            int found = 0;
            if (cand == s->bg_input)
                cand = -1;
            for (i = 0; i < nb_alive; i++)
                if (alive[i] == cand) { found = 1; break; }
            if (!found)
                cand = nb_alive > 0 ? alive[0] : -1;
        }
        if (!speaker_alive && nb_alive > 0) {
            s->active_speaker = cand;
            s->speaker_candidate = cand;
            s->candidate_since_us = now;
        } else if (s->speaker_switch &&
                   nb_alive > 0 && cand >= 0 && cand != s->active_speaker) {
            if (cand != s->speaker_candidate) {
                s->speaker_candidate = cand;
                s->candidate_since_us = now;
            }
            if (now - s->candidate_since_us >= (int64_t)s->speaker_hold_ms * 1000) {
                float diff = s->input_levels[cand] - s->input_levels[s->active_speaker];
                if (diff >= s->speaker_switch_db ||
                    s->input_levels[s->active_speaker] <= -120.0f)
                    s->active_speaker = cand;
            }
        } else {
            s->speaker_candidate = cand;
            s->candidate_since_us = now;
        }
    }

    /* ---- adapt (+ deprecated speaker): template fill / shrink / discard ---- */
    if (s->layout_mode == LAYOUT_ADAPTIVE || s->layout_mode == LAYOUT_SPEAKER) {
        int nb = FFMIN(nb_alive, MIXING_CUDA_MAX_INPUTS);
        if (nb == 0) {
            s->nb_slots = 0;
            s->last_nb_active = 0;
            s->last_speaker = -1;
            return;
        }
        if (s->last_speaker != s->active_speaker || s->last_nb_active != nb_alive) {
            if (s->nb_layout_slots > 0) {
                apply_adapt_from_template(ctx, s, alive, nb);
            } else if (s->layout_mode == LAYOUT_SPEAKER || s->speaker_switch) {
                /* No JSON: legacy built-in speaker geometry. */
                build_speaker_layout(s, s->active_speaker, alive, nb);
            } else {
                apply_active_grid(s, alive, nb);
            }
            s->last_speaker = s->active_speaker;
            s->last_nb_active = nb_alive;
        }
        return;
    }

    /* LAYOUT_FIXED: original behavior (mark stalled, optional collapse) */
    for (i = 0; i < s->nb_layout_slots; i++) {
        MixingSlot *ls = &s->layout_slots[i];
        int active;
        if (!ls->visible) {
            if (i < s->nb_slots)
                s->slots[i].stalled = 0;
            continue;
        }
        active = input_is_active(s, ls->input, now);
        if (i < s->nb_slots)
            s->slots[i].stalled = !active;
        if (!active)
            need_collapse = 1;
    }

    if (s->on_disconnect == ON_DISC_COLLAPSE && need_collapse && nb_alive > 0 &&
        nb_alive < s->nb_layout_slots) {
        apply_active_grid(s, alive, nb_alive);
        s->collapse_active = 1;
    } else if (s->collapse_active || !need_collapse) {
        int nb = FFMIN(s->nb_layout_slots, MIXING_CUDA_MAX_INPUTS);
        for (i = 0; i < nb; i++)
            s->slots[i] = s->layout_slots[i];
        s->nb_slots = nb;
        s->collapse_active = need_collapse ? s->collapse_active : 0;
        for (i = 0; i < s->nb_slots; i++) {
            MixingSlot *ls = &s->layout_slots[i];
            s->slots[i].stalled = ls->visible && !input_is_active(s, ls->input, now);
        }
    }
}

static int mixing_cuda_fill(AVFilterContext *ctx, AVFrame *dst)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    void *args_y[] = { &dst->data[0], &dst->linesize[0], &s->out_w, &s->out_h, &s->fill_y };
    void *args_uv[] = { &dst->data[1], &dst->linesize[1], &s->out_w, &s->out_h,
                        &s->fill_u, &s->fill_v };
    int ret;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_fill_y,
                                      DIV_UP(s->out_w, BLOCK_X), DIV_UP(s->out_h, BLOCK_Y), 1,
                                      BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;
    return CHECK_CU(cu->cuLaunchKernel(s->cu_fill_uv,
                                       DIV_UP(s->out_w / 2, BLOCK_X), DIV_UP(s->out_h / 2, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
}

static int mixing_cuda_draw_border(AVFilterContext *ctx, AVFrame *dst,
                                   int dx, int dy, int dw, int dh,
                                   int border_px, int radius,
                                   uint16_t by, uint16_t bu, uint16_t bv)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int rad = radius;
    void *args_y[] = {
        &dst->data[0], &dst->linesize[0],
        &s->out_w, &s->out_h,
        &dx, &dy, &dw, &dh,
        &border_px, &rad, &by
    };
    void *args_uv[] = {
        &dst->data[1], &dst->linesize[1],
        &s->out_w, &s->out_h,
        &dx, &dy, &dw, &dh,
        &border_px, &rad, &bu, &bv
    };
    int ret;

    if (border_px <= 0 || !s->cu_border_y || !s->cu_border_uv)
        return 0;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_border_y,
                                      DIV_UP(dw, BLOCK_X), DIV_UP(dh, BLOCK_Y), 1,
                                      BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;
    return CHECK_CU(cu->cuLaunchKernel(s->cu_border_uv,
                                       DIV_UP(dw / 2, BLOCK_X), DIV_UP(dh / 2, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
}

static int mixing_cuda_blit_slot(AVFilterContext *ctx, AVFrame *dst, AVFrame *src,
                                 int dx, int dy, int dw, int dh, int radius)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int sw = src->width, sh = src->height;
    int rad = radius > 0 ? radius : 0;
    void *args_y[] = {
        &dst->data[0], &dst->linesize[0],
        &src->data[0], &src->linesize[0],
        &sw, &sh, &dx, &dy, &dw, &dh, &rad
    };
    void *args_uv[] = {
        &dst->data[1], &dst->linesize[1],
        &src->data[1], &src->linesize[1],
        &sw, &sh, &dx, &dy, &dw, &dh, &rad
    };
    int ret;

    if (dw < 2 || dh < 2)
        return 0;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_blit_y,
                                      DIV_UP(dw, BLOCK_X), DIV_UP(dh, BLOCK_Y), 1,
                                      BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;
    return CHECK_CU(cu->cuLaunchKernel(s->cu_blit_uv,
                                       DIV_UP(dw / 2, BLOCK_X), DIV_UP(dh / 2, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
}

static int mixing_cuda_blit_alpha(AVFilterContext *ctx, AVFrame *dst, AVFrame *src,
                                  int dx, int dy, int dw, int dh, int alpha_q8)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int sw = src->width, sh = src->height;
    void *args_y[] = {
        &dst->data[0], &dst->linesize[0],
        &src->data[0], &src->linesize[0],
        &sw, &sh, &dx, &dy, &dw, &dh, &alpha_q8
    };
    void *args_uv[] = {
        &dst->data[1], &dst->linesize[1],
        &src->data[1], &src->linesize[1],
        &sw, &sh, &dx, &dy, &dw, &dh, &alpha_q8
    };
    int ret;

    if (dw < 2 || dh < 2 || alpha_q8 <= 0)
        return 0;
    if (alpha_q8 >= 255)
        return mixing_cuda_blit_slot(ctx, dst, src, dx, dy, dw, dh, 0);

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_blit_alpha_y,
                                      DIV_UP(dw, BLOCK_X), DIV_UP(dh, BLOCK_Y), 1,
                                      BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;
    return CHECK_CU(cu->cuLaunchKernel(s->cu_blit_alpha_uv,
                                       DIV_UP(dw / 2, BLOCK_X), DIV_UP(dh / 2, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
}

static int mixing_cuda_blit_offset_alpha(AVFilterContext *ctx, AVFrame *dst, AVFrame *src,
                                         int clip_x, int clip_y, int clip_w, int clip_h,
                                         int dx, int dy, int dw, int dh, int alpha_q8)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    int sw = src->width, sh = src->height;
    void *args_y[] = {
        &dst->data[0], &dst->linesize[0],
        &src->data[0], &src->linesize[0],
        &sw, &sh,
        &clip_x, &clip_y, &clip_w, &clip_h,
        &dx, &dy, &dw, &dh, &alpha_q8
    };
    void *args_uv[] = {
        &dst->data[1], &dst->linesize[1],
        &src->data[1], &src->linesize[1],
        &sw, &sh,
        &clip_x, &clip_y, &clip_w, &clip_h,
        &dx, &dy, &dw, &dh, &alpha_q8
    };
    int ret;

    if (clip_w < 2 || clip_h < 2 || dw < 2 || dh < 2 || alpha_q8 <= 0)
        return 0;

    ret = CHECK_CU(cu->cuLaunchKernel(s->cu_blit_off_alpha_y,
                                      DIV_UP(clip_w, BLOCK_X), DIV_UP(clip_h, BLOCK_Y), 1,
                                      BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
    if (ret < 0)
        return ret;
    return CHECK_CU(cu->cuLaunchKernel(s->cu_blit_off_alpha_uv,
                                       DIV_UP(clip_w / 2, BLOCK_X), DIV_UP(clip_h / 2, BLOCK_Y), 1,
                                       BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
}

static int mixing_cuda_blit_transition(AVFilterContext *ctx, AVFrame *dst, AVFrame *src,
                                       int dx, int dy, int dw, int dh, float t)
{
    MixingCUDAContext *s = ctx->priv;
    int alpha_q8 = 255;
    int off_x = 0, off_y = 0;
    int img_dx, img_dy;
    float u;

    if (t >= 1.f)
        return mixing_cuda_blit_slot(ctx, dst, src, dx, dy, dw, dh, 0);
    if (t <= 0.f && s->trans_effect == TRANS_FADE)
        return 0;

    u = 1.f - t;
    switch (s->trans_effect) {
    case TRANS_FADE:
        alpha_q8 = av_clip_uint8((int)(t * 255.f + 0.5f));
        return mixing_cuda_blit_alpha(ctx, dst, src, dx, dy, dw, dh, alpha_q8);
    case TRANS_SLIDE_LEFT:
    case TRANS_FLY_LEFT:
        off_x = ((int)(u * dw + 0.5f)) & ~1;
        break;
    case TRANS_SLIDE_RIGHT:
    case TRANS_FLY_RIGHT:
        off_x = -(((int)(u * dw + 0.5f)) & ~1);
        break;
    case TRANS_SLIDE_UP:
    case TRANS_FLY_UP:
        off_y = ((int)(u * dh + 0.5f)) & ~1;
        break;
    case TRANS_SLIDE_DOWN:
    case TRANS_FLY_DOWN:
        off_y = -(((int)(u * dh + 0.5f)) & ~1);
        break;
    default:
        return mixing_cuda_blit_slot(ctx, dst, src, dx, dy, dw, dh, 0);
    }

    if (s->trans_effect >= TRANS_FLY_LEFT)
        alpha_q8 = av_clip_uint8((int)(t * 255.f + 0.5f));

    img_dx = dx + off_x;
    img_dy = dy + off_y;
    return mixing_cuda_blit_offset_alpha(ctx, dst, src,
                                         dx, dy, dw, dh,
                                         img_dx, img_dy, dw, dh, alpha_q8);
}

static int mixing_cuda_copy_bg(AVFilterContext *ctx, AVFrame *dst, AVFrame *src)
{
    MixingCUDAContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    void *args_y[] = {
        &dst->data[0], &dst->linesize[0],
        &src->data[0], &src->linesize[0],
        &s->out_w, &s->out_h
    };
    void *args_uv[] = {
        &dst->data[1], &dst->linesize[1],
        &src->data[1], &src->linesize[1],
        &s->out_w, &s->out_h
    };
    int ret;

    if (src->width == s->out_w && src->height == s->out_h) {
        ret = CHECK_CU(cu->cuLaunchKernel(s->cu_copy_y,
                                          DIV_UP(s->out_w, BLOCK_X), DIV_UP(s->out_h, BLOCK_Y), 1,
                                          BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_y, NULL));
        if (ret < 0)
            return ret;
        return CHECK_CU(cu->cuLaunchKernel(s->cu_copy_uv,
                                           DIV_UP(s->out_w / 2, BLOCK_X), DIV_UP(s->out_h / 2, BLOCK_Y), 1,
                                           BLOCK_X, BLOCK_Y, 1, 0, s->cu_stream, args_uv, NULL));
    }

    return mixing_cuda_blit_slot(ctx, dst, src, 0, 0, s->out_w, s->out_h, 0);
}

static void fit_rect(int sw, int sh, int slot_x, int slot_y, int slot_w, int slot_h,
                     int *dx, int *dy, int *dw, int *dh)
{
    double sx = (double)slot_w / sw;
    double sy = (double)slot_h / sh;
    double sc = FFMIN(sx, sy);
    *dw = FFMAX(2, ((int)(sw * sc)) & ~1);
    *dh = FFMAX(2, ((int)(sh * sc)) & ~1);
    *dx = slot_x + ((slot_w - *dw) / 2) & ~1;
    *dy = slot_y + ((slot_h - *dh) / 2) & ~1;
}

static int mixing_cuda_compose(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    MixingCUDAContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    FilterLink *outl = ff_filter_link(outlink);
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUcontext dummy;
    AVFrame *out = NULL;
    int64_t now = av_gettime_relative();
    int i, ret, bg_idx;

    load_layout_file(ctx);

    /* CUevent pipeline: wait on the previous frame's event (not the whole stream). */
    if (s->hwctx && s->cu_stream) {
        CUcontext dummy;
        ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (ret < 0)
            return ret;
        if (s->cu_event_armed) {
            ret = CHECK_CU(cu->cuEventSynchronize(s->cu_event));
            s->cu_event_armed = 0;
        } else {
            ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
        }
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < s->nb_inputs; i++) {
        AVFrame *fr = NULL;
        av_frame_free(&s->frames[i]);
        /* get=0 is a borrowed FFFrameSync pointer; this cache owns it. */
        ret = ff_framesync_get_frame(fs, i, &fr, 1);
        if (ret < 0)
            return ret;
        s->frames[i] = fr;
        if (fr && frame_is_dyn_empty(fr)) {
            /* Empty placeholder: keep pad alive for FrameSync. Prefer last
             * good frame for blit; do NOT reset last_arrival ...ages via
             * hold_stall_ms / stall_ms. */
            if (s->in_state[i].hold) {
                av_frame_free(&s->frames[i]);
                s->frames[i] = av_frame_clone(s->in_state[i].hold);
                s->in_state[i].have_frame = s->frames[i] != NULL;
                s->in_state[i].holding = 1;
            } else {
                s->in_state[i].have_frame = 0;
                s->in_state[i].holding = 0;
            }
        } else if (fr && frame_is_dyn_hold(fr)) {
            /* Held repeat: show picture; age with hold_stall_ms (not stall_ms). */
            int clock = mixing_cuda_clock_input(s);
            s->in_state[i].have_frame = 1;
            s->in_state[i].holding = 1;
            if (i != clock && !s->in_state[i].hold) {
                s->in_state[i].hold = av_frame_clone(fr);
            }
            if (s->in_state[i].last_arrival_us == AV_NOPTS_VALUE)
                s->in_state[i].last_arrival_us = now;
        } else if (fr) {
            int clock = mixing_cuda_clock_input(s);
            int is_repeat = (i != clock &&
                             s->in_state[i].last_pts != AV_NOPTS_VALUE &&
                             fr->pts == s->in_state[i].last_pts);
            s->in_state[i].have_frame = 1;
            s->in_state[i].holding = 0;
            /* Start transition on new content gen before hold is replaced. */
            if (!is_repeat)
                mixing_trans_maybe_start(ctx, s, i, fr, now);
            if (i != clock) {
                av_frame_free(&s->in_state[i].hold);
                s->in_state[i].hold = av_frame_clone(fr);
            }
            if (i == clock) {
                /* Clock plate is always fresh. */
                s->in_state[i].last_pts = fr->pts;
                s->in_state[i].last_arrival_us = now;
            } else if (!is_repeat) {
                /* New program PTS from dynamic_input = new decode tick. */
                s->in_state[i].last_pts = fr->pts;
                s->in_state[i].last_arrival_us = now;
            } else {
                /* Same PTS: FrameSync EXT_INFINITY repeat while source is late.
                 * Treat as hold so stall_ms=800 does not black thumbs when
                 * color_cuda runs faster than business r (e.g. 50 vs 25). */
                s->in_state[i].holding = 1;
                if (s->in_state[i].last_arrival_us == AV_NOPTS_VALUE)
                    s->in_state[i].last_arrival_us = now;
            }
        } else {
            if (s->in_state[i].hold) {
                s->frames[i] = av_frame_clone(s->in_state[i].hold);
                s->in_state[i].have_frame = s->frames[i] != NULL;
                s->in_state[i].holding = 1;
            } else {
                s->in_state[i].have_frame = 0;
                s->in_state[i].holding = 0;
                s->in_state[i].last_pts = AV_NOPTS_VALUE;
                s->in_state[i].last_arrival_us = AV_NOPTS_VALUE;
            }
        }
        mix_sync_observe_pad(ctx, s, i, fr, now);
    }

    rebuild_active_slots(ctx, s, now);

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);
    ret = av_hwframe_get_buffer(outl->hw_frames_ctx, out, 0);
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0) {
        av_frame_free(&out);
        return ret;
    }

    /* Always paint full-frame plate first (color_cuda / fill). bg_input is
     * never a layout tile ...empty business pads reveal this full background. */
    bg_idx = s->bg_input;
    if (bg_idx >= 0 && bg_idx < s->nb_inputs && s->frames[bg_idx] &&
        !frame_is_dyn_empty(s->frames[bg_idx])) {
        ret = mixing_cuda_copy_bg(ctx, out, s->frames[bg_idx]);
    } else {
        ret = mixing_cuda_fill(ctx, out);
    }
    if (ret < 0)
        goto fail;

    {
        MixingSlot ordered[MIXING_CUDA_MAX_INPUTS];
        int n = FFMIN(s->nb_slots, MIXING_CUDA_MAX_INPUTS);
        int k;
        for (k = 0; k < n; k++)
            ordered[k] = s->slots[k];
        qsort(ordered, n, sizeof(*ordered), slot_cmp_z);

        for (i = 0; i < n; i++) {
            MixingSlot *sl = &ordered[i];
            AVFrame *fr;
            int sx, sy, sw, sh, dx, dy, dw, dh;
            int bpx, rad;
            uint16_t by, bu, bv;
            if (!sl->visible || sl->stalled)
                continue;
            if (sl->input < 0 || sl->input >= s->nb_inputs)
                continue;
            /* Background plate is full-frame only; never blit into a cell. */
            if (sl->input == bg_idx)
                continue;
            fr = s->frames[sl->input];
            if (!fr || frame_is_dyn_empty(fr))
                continue;

            resolve_slot_style(s, sl, &bpx, &rad, &by, &bu, &bv);

            sx = (int)(sl->x * s->out_w / 100.0) & ~1;
            sy = (int)(sl->y * s->out_h / 100.0) & ~1;
            sw = FFMAX(2, (int)(sl->w * s->out_w / 100.0) & ~1);
            sh = FFMAX(2, (int)(sl->h * s->out_h / 100.0) & ~1);
            if (sx + sw > s->out_w) sw = s->out_w - sx;
            if (sy + sh > s->out_h) sh = s->out_h - sy;
            if (sx < 0 || sy < 0 || sx >= s->out_w || sy >= s->out_h ||
                sw < 2 || sh < 2)
                continue;

            fit_rect(fr->width, fr->height, sx, sy, sw, sh, &dx, &dy, &dw, &dh);
            if (dx < 0 || dy < 0 || dw < 2 || dh < 2 ||
                dx + dw > s->out_w || dy + dh > s->out_h)
                continue;
            {
                MixingInputState *st = &s->in_state[sl->input];
                float t = 1.f;
                if (st->trans_active) {
                    t = mixing_trans_progress(s, st, now);
                    if (st->trans_under) {
                        int udx, udy, udw, udh;
                        fit_rect(st->trans_under->width, st->trans_under->height,
                                 sx, sy, sw, sh, &udx, &udy, &udw, &udh);
                        if (udx >= 0 && udy >= 0 && udw >= 2 && udh >= 2 &&
                            udx + udw <= s->out_w && udy + udh <= s->out_h) {
                            ret = mixing_cuda_blit_slot(ctx, out, st->trans_under,
                                                        udx, udy, udw, udh, rad);
                            if (ret < 0)
                                goto fail;
                        }
                    }
                    ret = mixing_cuda_blit_transition(ctx, out, fr, dx, dy, dw, dh, t);
                    if (ret < 0)
                        goto fail;
                    if (t >= 1.f)
                        mixing_trans_end(st);
                } else {
                    ret = mixing_cuda_blit_slot(ctx, out, fr, dx, dy, dw, dh, rad);
                    if (ret < 0)
                        goto fail;
                }
            }
            if (bpx > 0) {
                ret = mixing_cuda_draw_border(ctx, out, dx, dy, dw, dh,
                                              bpx, rad, by, bu, bv);
                if (ret < 0)
                    goto fail;
            }
        }
    }

    /* Record event for pipeline sync on the next compose; keep input frames alive. */
    if (s->cu_event) {
        CHECK_CU(cu->cuEventRecord(s->cu_event, s->cu_stream));
        s->cu_event_armed = 1;
    }

    CHECK_CU(cu->cuCtxPopCurrent(&dummy));

    /* PTS follows FrameSync clock (bg plate), not a business pad. */
    {
        int clock = mixing_cuda_clock_input(s);
        AVFrame *pf = (clock >= 0 && clock < s->nb_inputs) ? s->frames[clock] : NULL;
        if (!pf)
            pf = s->frames[0];
        ret = av_frame_copy_props(out, pf ? pf : out);
        if (ret < 0) {
            av_frame_free(&out);
            return ret;
        }
        /* Prefer framesync timeline so encoder sees steady cadence. */
        out->pts = fs->pts;
        if (out->pts == AV_NOPTS_VALUE && pf && pf->pts != AV_NOPTS_VALUE)
            out->pts = pf->pts;
    }
    av_frame_remove_side_data(out, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    av_frame_remove_side_data(out, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
    out->width  = s->out_w;
    out->height = s->out_h;

    return ff_filter_frame(outlink, out);

fail:
    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    av_frame_free(&out);
    return ret;
}

static av_cold int mixing_cuda_init(AVFilterContext *ctx)
{
    MixingCUDAContext *s = ctx->priv;
    int i, ret;

    if (s->nb_inputs < 1 || s->nb_inputs > MIXING_CUDA_MAX_INPUTS)
        return AVERROR(EINVAL);

    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = { 0 };
        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);
        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
        s->in_state[i].last_pts = AV_NOPTS_VALUE;
        s->in_state[i].last_arrival_us = AV_NOPTS_VALUE;
        s->in_state[i].live_since_us = AV_NOPTS_VALUE;
        s->in_state[i].last_gen = AV_NOPTS_VALUE;
        s->in_state[i].trans_active = 0;
        s->in_state[i].trans_start_us = 0;
        s->in_state[i].trans_under = NULL;
        s->in_state[i].empty_streak = 0;
        s->in_state[i].seen_real = 0;
        s->in_state[i].last_sync_warn_us = 0;
        s->input_active[i] = 1;   /* default all active */
    }
    s->mix_start_us = av_gettime_relative();

    /* extension defaults */
    s->primary_input = 0;
    s->active_speaker = 0;
    s->speaker_candidate = 0;
    s->candidate_since_us = AV_NOPTS_VALUE;
    s->last_nb_active = -1;
    s->last_speaker = -1;
    for (i = 0; i < MIXING_CUDA_MAX_INPUTS; i++)
        s->input_levels[i] = -120.0f;
#if CONFIG_LIBZMQ
    s->spk_active_speaker = 0;
    pthread_mutex_init(&s->spk_lock, NULL);
#endif

    if (s->size_str) {
        ret = av_parse_video_size(&s->out_w, &s->out_h, s->size_str);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Invalid size '%s'\n", s->size_str);
            return ret;
        }
    }
    if (s->out_w <= 0) s->out_w = 1920;
    if (s->out_h <= 0) s->out_h = 1080;

    s->sw_format = AV_PIX_FMT_NV12;
    if (s->format_str) {
        s->sw_format = av_get_pix_fmt(s->format_str);
        if (!format_is_supported(s->sw_format)) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unsupported format '%s' (supported: nv12, p010)\n",
                   s->format_str);
            return AVERROR(EINVAL);
        }
    }
    apply_fill_from_rgba(s);
    apply_border_from_rgba(s);
    apply_tile_border_from_rgba(s);

    s->fs.on_event = mixing_cuda_compose;

    if (s->layout_mode == LAYOUT_SPEAKER) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: layout_mode=speaker is deprecated ->adapt "
               "(use speaker_switch for incentive; layout JSON for geometry)\n");
        s->layout_mode = LAYOUT_ADAPTIVE;
    }

    /* fixed/adapt: load layout_str ->layout_file ->/data/LCMS/layouts/<task_id>.layout */
    {
        int parsed = 0;
        char task_path[256];

        if (s->layout_str && *s->layout_str) {
            if (mixing_cuda_apply_layout_json(ctx, s->layout_str, 1) < 0)
                apply_default_layout_fallback(ctx, "inline layout JSON invalid");
            else
                parsed = 1;
        }
        if (!parsed && s->layout_file && *s->layout_file) {
            if (load_layout_file(ctx) >= 0 && layout_slots_valid(s))
                parsed = 1;
        }
        if (!parsed &&
            mixing_cuda_task_layout_path(s, task_path, sizeof(task_path)) == 0) {
            char *saved = s->layout_file;
            s->layout_file = task_path;
            s->layout_mtime = 0;
            if (load_layout_file(ctx) >= 0 && layout_slots_valid(s)) {
                parsed = 1;
                av_log(ctx, AV_LOG_WARNING,
                       "mixing_cuda: restored layout from %s\n", task_path);
            }
            s->layout_file = saved;
        }
        if (s->layout_mode == LAYOUT_FIXED && !layout_slots_valid(s))
            apply_default_layout_fallback(ctx, "no usable layout JSON");
    }

    return 0;
}

static av_cold void mixing_cuda_uninit(AVFilterContext *ctx)
{
    MixingCUDAContext *s = ctx->priv;
    int i;

#if CONFIG_LIBZMQ
    if (s->zmq_sub) {
        s->zmq_stop = 1;
        pthread_join(s->zmq_thread, NULL);
        lavfi_zmq_destroy(&s->zmq_sub);
    }
    pthread_mutex_destroy(&s->spk_lock);
#endif

    if (s->hwctx && s->cu_stream) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
        if (s->cu_event) {
            CHECK_CU(cu->cuEventDestroy(s->cu_event));
            s->cu_event = NULL;
            s->cu_event_armed = 0;
        }
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    ff_framesync_uninit(&s->fs);
    av_freep(&s->layout_cache);
    for (i = 0; i < MIXING_CUDA_MAX_INPUTS; i++) {
        av_frame_free(&s->frames[i]);
        av_frame_free(&s->in_state[i].hold);
        av_frame_free(&s->in_state[i].trans_under);
    }

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
        s->cu_module = NULL;
    }
    av_buffer_unref(&s->hw_frames_ctx);
}

static int mixing_cuda_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MixingCUDAContext *s = ctx->priv;
    FilterLink *outl = ff_filter_link(outlink);
    AVFilterLink *in0 = ctx->inputs[0];
    FilterLink *in0l = ff_filter_link(in0);
    AVHWFramesContext *in_fc, *out_fc;
    AVBufferRef *device_ref;
    int i, ret;

    if (!in0l->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw_frames_ctx on input0 (need CUDA frames)\n");
        return AVERROR(EINVAL);
    }

    in_fc = (AVHWFramesContext *)in0l->hw_frames_ctx->data;
    s->hwctx = in_fc->device_ctx->hwctx;
    device_ref = in_fc->device_ref;

    for (i = 0; i < s->nb_inputs; i++) {
        FilterLink *il = ff_filter_link(ctx->inputs[i]);
        AVHWFramesContext *fc;
        if (!il->hw_frames_ctx) {
            av_log(ctx, AV_LOG_ERROR, "input%d missing hw_frames_ctx\n", i);
            return AVERROR(EINVAL);
        }
        fc = (AVHWFramesContext *)il->hw_frames_ctx->data;
        if (!format_is_supported(fc->sw_format)) {
            av_log(ctx, AV_LOG_ERROR,
                   "input%d unsupported sw_format %s (supported: nv12, p010)\n", i,
                   av_get_pix_fmt_name(fc->sw_format));
            return AVERROR(EINVAL);
        }
        if (fc->sw_format != s->sw_format) {
            av_log(ctx, AV_LOG_ERROR,
                   "input%d sw_format %s must match mixing format=%s\n", i,
                   av_get_pix_fmt_name(fc->sw_format),
                   av_get_pix_fmt_name(s->sw_format));
            return AVERROR(EINVAL);
        }
    }

    av_buffer_unref(&s->hw_frames_ctx);
    s->hw_frames_ctx = av_hwframe_ctx_alloc(device_ref);
    if (!s->hw_frames_ctx)
        return AVERROR(ENOMEM);
    out_fc = (AVHWFramesContext *)s->hw_frames_ctx->data;
    out_fc->format    = AV_PIX_FMT_CUDA;
    out_fc->sw_format = s->sw_format;
    out_fc->width     = FFALIGN(s->out_w, 32);
    out_fc->height    = FFALIGN(s->out_h, 32);
    ret = av_hwframe_ctx_init(s->hw_frames_ctx);
    if (ret < 0)
        return ret;

    outlink->w = s->out_w;
    outlink->h = s->out_h;
    outl->frame_rate = in0l->frame_rate;
    outlink->sample_aspect_ratio = in0->sample_aspect_ratio;
    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        int is_p010 = (s->sw_format == AV_PIX_FMT_P010);
        const char *y_fn, *uv_fn, *fill_y, *fill_uv, *copy_y, *copy_uv;
        const char *alpha_y, *alpha_uv, *off_y, *off_uv, *border_y, *border_uv;
        extern const unsigned char ff_vf_mixing_cuda_ptx_data[];
        extern const unsigned int ff_vf_mixing_cuda_ptx_len;

        if (is_p010) {
            y_fn = s->interp == INTERP_LANCZOS ? "MixingScaleBlit_Y_U16_Lanczos"
                                               : "MixingScaleBlit_Y_U16";
            uv_fn = s->interp == INTERP_LANCZOS ? "MixingScaleBlit_UV_P010_Lanczos"
                                                : "MixingScaleBlit_UV_P010";
            fill_y = "MixingFill_Y_U16";
            fill_uv = "MixingFill_UV_P010";
            copy_y = "MixingCopy_Y_U16";
            copy_uv = "MixingCopy_UV_P010";
            alpha_y = "MixingScaleBlitAlpha_Y_U16";
            alpha_uv = "MixingScaleBlitAlpha_UV_P010";
            off_y = "MixingScaleBlitOffsetAlpha_Y_U16";
            off_uv = "MixingScaleBlitOffsetAlpha_UV_P010";
            border_y = "MixingHighlightBorder_Y_U16";
            border_uv = "MixingHighlightBorder_UV_P010";
        } else {
            y_fn = s->interp == INTERP_LANCZOS ? "MixingScaleBlit_Y_Lanczos"
                                               : "MixingScaleBlit_Y";
            uv_fn = s->interp == INTERP_LANCZOS ? "MixingScaleBlit_UV_NV12_Lanczos"
                                                : "MixingScaleBlit_UV_NV12";
            fill_y = "MixingFill_Y";
            fill_uv = "MixingFill_UV_NV12";
            copy_y = "MixingCopy_Y";
            copy_uv = "MixingCopy_UV_NV12";
            alpha_y = "MixingScaleBlitAlpha_Y";
            alpha_uv = "MixingScaleBlitAlpha_UV_NV12";
            off_y = "MixingScaleBlitOffsetAlpha_Y";
            off_uv = "MixingScaleBlitOffsetAlpha_UV_NV12";
            border_y = "MixingHighlightBorder_Y";
            border_uv = "MixingHighlightBorder_UV_NV12";
        }

        ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        if (ret < 0)
            return ret;
        ret = ff_cuda_load_module(ctx, s->hwctx, &s->cu_module,
                                  ff_vf_mixing_cuda_ptx_data, ff_vf_mixing_cuda_ptx_len);
        if (ret < 0) {
            CHECK_CU(cu->cuCtxPopCurrent(&dummy));
            return ret;
        }
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_fill_y, s->cu_module, fill_y));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_fill_uv, s->cu_module, fill_uv));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_copy_y, s->cu_module, copy_y));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_copy_uv, s->cu_module, copy_uv));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_y, s->cu_module, y_fn));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_uv, s->cu_module, uv_fn));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_alpha_y, s->cu_module, alpha_y));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_alpha_uv, s->cu_module, alpha_uv));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_off_alpha_y, s->cu_module, off_y));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_blit_off_alpha_uv, s->cu_module, off_uv));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_border_y, s->cu_module, border_y));
        if (ret < 0) goto cu_fail;
        ret = CHECK_CU(cu->cuModuleGetFunction(&s->cu_border_uv, s->cu_module, border_uv));
        if (ret < 0) goto cu_fail;
        s->cu_stream = s->hwctx->stream;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    if ((ret = ff_framesync_init(&s->fs, ctx, s->nb_inputs)) < 0)
        return ret;
    s->fs.on_event = mixing_cuda_compose;
    for (i = 0; i < s->nb_inputs; i++) {
        s->fs.in[i].time_base = ctx->inputs[i]->time_base;
        s->fs.in[i].sync   = 1;
        s->fs.in[i].before = EXT_STOP;
        s->fs.in[i].after  = EXT_INFINITY;
    }
    ret = ff_framesync_configure(&s->fs);

    /*
     * FrameSync sync strategy override (anti-blocking core):
     * ff_framesync_configure() only zeroes sync/after for secondary inputs when
     * !opt_repeatlast. mixing_cuda defaults repeatlast=1, so configure leaves
     * sync=1/before=EXT_STOP on ALL inputs -> any stalled input blocks output.
     *
     * Override AFTER configure via apply_framesync_roles().
     */
    apply_framesync_roles(s);

    /* Create CUevent for pipeline sync (replaces per-frame cuStreamSynchronize). */
    {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuEventCreate(&s->cu_event, CU_EVENT_DISABLE_TIMING));
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
        s->cu_event_armed = 0;
    }

#if CONFIG_LIBZMQ
    /* SUB loudness rank from amixrank; prefer ipc gain_<task_id>.sock */
    {
        char url[512];
        if (lavfi_zmq_resolve_url(url, sizeof(url), "gain", s->task_id,
                                  s->rank_endpoint) == 0) {
            s->zmq_sub = lavfi_zmq_create(ZMQ_SUB, 0, url, ctx);
            if (s->zmq_sub) {
                s->zmq_stop = 0;
                pthread_create(&s->zmq_thread, NULL, mixing_cuda_zmq_thread, ctx);
                av_log(ctx, AV_LOG_WARNING, "mixing_cuda: SUB connected to %s\n", url);
            }
        }
    }
#endif

    outlink->time_base = s->fs.time_base;
    return ret;

cu_fail:
    {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }
    return ret;
}

static int mixing_cuda_activate(AVFilterContext *ctx)
{
    MixingCUDAContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static const char *mixing_cuda_layout_mode_name(int mode)
{
    switch (mode) {
    case LAYOUT_ADAPTIVE: return "adaptive";
    case LAYOUT_SPEAKER:  return "speaker";
    default:              return "fixed";
    }
}

static int mixing_cuda_apply_layout_mode(AVFilterContext *ctx, int mode)
{
    MixingCUDAContext *s = ctx->priv;

    if (mode == s->layout_mode) {
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: layout_mode already %s\n",
               mixing_cuda_layout_mode_name(mode));
        return 0;
    }
    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: layout_mode %s -> %s\n",
           mixing_cuda_layout_mode_name(s->layout_mode),
           mixing_cuda_layout_mode_name(mode));
    s->layout_mode = mode;
    s->last_nb_active = -1;
    s->last_speaker = -1;
    if (mode == LAYOUT_FIXED || mode == LAYOUT_ADAPTIVE) {
        if (s->layout_cache && *s->layout_cache) {
            if (parse_layout_json(s, s->layout_cache, ctx) < 0 && mode == LAYOUT_FIXED)
                apply_default_layout_fallback(ctx, "cached layout JSON invalid");
        } else if (s->layout_file) {
            s->layout_mtime = 0;
            load_layout_file(ctx);
        } else if (s->layout_str) {
            if (mixing_cuda_apply_layout_json(ctx, s->layout_str, 0) < 0 &&
                mode == LAYOUT_FIXED)
                apply_default_layout_fallback(ctx, "layout_mode=fixed but layout JSON invalid");
        } else if (mode == LAYOUT_FIXED && s->nb_layout_slots == 0) {
            apply_default_layout_fallback(ctx, "layout_mode=fixed without layout/layout_file");
        }
    }
    return 0;
}

/* Business slots 1..N (exclude bg 0); supports "2" or "1-4". */
static int mixing_cuda_parse_slot_range(const char *tok, int nb_inputs,
                                        int *lo, int *hi)
{
    int a, b;
    char *dash;
    int max_slot;

    if (!tok || !*tok || !lo || !hi || nb_inputs < 2)
        return AVERROR(EINVAL);
    max_slot = nb_inputs - 1;
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

/*
 * Reply: mode=<layout_mode> speaker=<pad> switch=<0|1> then
 *   slot:active:holding:have_frame (business 1..N, space-separated)
 */
static int mixing_cuda_cmd_search(AVFilterContext *ctx, const char *args,
                                  char *res, int res_len)
{
    MixingCUDAContext *s = ctx->priv;
    int i, off = 0;
    int lo = 1, hi = s->nb_inputs > 1 ? s->nb_inputs - 1 : 0;

    if (!res || res_len <= 0)
        return 0;
    res[0] = 0;
    if (args && *args) {
        char tok[64];
        if (sscanf(args, "%63s", tok) != 1 ||
            mixing_cuda_parse_slot_range(tok, s->nb_inputs, &lo, &hi) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: search bad args='%s' (need: [slot|lo-hi])\n",
                   args);
            return AVERROR(EINVAL);
        }
    }

    off += snprintf(res + off, res_len > off ? res_len - off : 0,
                    "mode=%s speaker=%d switch=%d",
                    mixing_cuda_layout_mode_name(s->layout_mode),
                    s->active_speaker, s->speaker_switch);
    for (i = lo; i <= hi && off < res_len - 1; i++) {
        int act = s->input_active[i] ? 1 : 0;
        int hold = (i < s->nb_inputs && s->in_state[i].holding) ? 1 : 0;
        int have = (i < s->nb_inputs && s->in_state[i].have_frame) ? 1 : 0;
        off += snprintf(res + off, res_len > off ? res_len - off : 0,
                        " %d:%d:%d:%d", i, act, hold, have);
    }
    if (off <= 0)
        snprintf(res, res_len, "(none)");
    av_log(ctx, AV_LOG_WARNING, "mixing_cuda: search -> %s\n", res);
    return 0;
}


static int mixing_cuda_cmd_slot_style(AVFilterContext *ctx, const char *args)
{
    MixingCUDAContext *s = ctx->priv;
    int pad = -1, bpx = -2, rad = -2, i;
    char color[64] = {0};
    MixingSlot *target = NULL;
    const char *p;

    if (!args || !*args) {
        av_log(ctx, AV_LOG_ERROR,
               "mixing_cuda: slot_style need: <pad> border_px=N radius_px=N border_color=C\n");
        return AVERROR(EINVAL);
    }
    if (sscanf(args, "%d", &pad) != 1 || pad < 0 || pad >= s->nb_inputs ||
        pad == s->bg_input) {
        av_log(ctx, AV_LOG_ERROR,
               "mixing_cuda: slot_style bad pad='%s' (business pad, not bg)\n", args);
        return AVERROR(EINVAL);
    }
    p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    while (*p) {
        if (!strncmp(p, "border_px=", 10)) {
            bpx = atoi(p + 10);
        } else if (!strncmp(p, "radius_px=", 10)) {
            rad = atoi(p + 10);
        } else if (!strncmp(p, "radius=", 7)) {
            rad = atoi(p + 7);
        } else if (!strncmp(p, "border_color=", 13)) {
            sscanf(p + 13, "%63s", color);
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    for (i = 0; i < s->nb_layout_slots; i++) {
        if (s->layout_slots[i].input == pad) {
            target = &s->layout_slots[i];
            break;
        }
    }
    if (!target) {
        for (i = 0; i < s->nb_slots; i++) {
            if (s->slots[i].input == pad) {
                target = &s->slots[i];
                break;
            }
        }
    }
    if (!target) {
        av_log(ctx, AV_LOG_ERROR,
               "mixing_cuda: slot_style pad=%d not in current layout\n", pad);
        return AVERROR(EINVAL);
    }
    if (bpx >= -1)
        target->border_px = bpx;
    if (rad >= -1)
        target->radius_px = rad;
    if (color[0]) {
        uint8_t rgba[4];
        if (av_parse_color(rgba, color, -1, ctx) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: slot_style bad border_color='%s'\n", color);
            return AVERROR(EINVAL);
        }
        memcpy(target->border_rgba, rgba, 4);
        target->has_border_color = 1;
    }
    for (i = 0; i < s->nb_slots; i++) {
        if (s->slots[i].input == pad) {
            if (bpx >= -1) s->slots[i].border_px = target->border_px;
            if (rad >= -1) s->slots[i].radius_px = target->radius_px;
            if (color[0]) {
                memcpy(s->slots[i].border_rgba, target->border_rgba, 4);
                s->slots[i].has_border_color = 1;
            }
        }
    }
    for (i = 0; i < s->nb_layout_slots; i++) {
        if (s->layout_slots[i].input == pad) {
            if (bpx >= -1) s->layout_slots[i].border_px = target->border_px;
            if (rad >= -1) s->layout_slots[i].radius_px = target->radius_px;
            if (color[0]) {
                memcpy(s->layout_slots[i].border_rgba, target->border_rgba, 4);
                s->layout_slots[i].has_border_color = 1;
            }
        }
    }
    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: slot_style pad=%d border_px=%d radius_px=%d color=%s\n",
           pad, target->border_px, target->radius_px,
           color[0] ? color : "(keep)");
    return 0;
}

static int mixing_cuda_process_command(AVFilterContext *ctx, const char *cmd,
                                       const char *args, char *res, int res_len, int flags)
{
    MixingCUDAContext *s = ctx->priv;
    int ret;

    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: cmd='%s' args='%s'\n", cmd, args ? args : "");

    if (!strcmp(cmd, "search"))
        return mixing_cuda_cmd_search(ctx, args, res, res_len);

    if (!strcmp(cmd, "slot_style") && args)
        return mixing_cuda_cmd_slot_style(ctx, args);

    if (!strcmp(cmd, "layout") && args) {
        if (s->layout_mode != LAYOUT_FIXED && s->layout_mode != LAYOUT_ADAPTIVE &&
            s->layout_mode != LAYOUT_SPEAKER) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: 'layout' unsupported in mode=%s\n",
                   mixing_cuda_layout_mode_name(s->layout_mode));
            return AVERROR(EINVAL);
        }
        ret = mixing_cuda_apply_layout_json(ctx, args, 1);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: layout JSON invalid, applying fallback\n");
            apply_default_layout_fallback(ctx, "layout command JSON invalid");
            return AVERROR(EINVAL);
        }
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: layout JSON applied (mode=%s) and persisted\n",
               mixing_cuda_layout_mode_name(s->layout_mode));
        return 0;
    }
    if (!strcmp(cmd, "reload")) {
        char task_path[256];
        const char *path = s->layout_file;
        char *saved = NULL;
        if ((!path || !*path) &&
            mixing_cuda_task_layout_path(s, task_path, sizeof(task_path)) == 0)
            path = task_path;
        if (!path || !*path) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: 'reload' needs layout_file= or task persist path\n");
            return AVERROR(EINVAL);
        }
        if (path == task_path) {
            saved = s->layout_file;
            s->layout_file = task_path;
        }
        s->layout_mtime = 0;
        load_layout_file(ctx);
        if (saved)
            s->layout_file = saved;
        av_log(ctx, AV_LOG_WARNING, "mixing_cuda: layout reloaded from %s\n", path);
        return 0;
    }
    if (!strcmp(cmd, "layout_mode") && args) {
        int mode;
        if (!strcmp(args, "fixed"))
            mode = LAYOUT_FIXED;
        else if (!strcmp(args, "adaptive") || !strcmp(args, "adapt"))
            mode = LAYOUT_ADAPTIVE;
        else if (!strcmp(args, "speaker")) {
            av_log(ctx, AV_LOG_WARNING,
                   "mixing_cuda: layout_mode=speaker deprecated ->adapt\n");
            mode = LAYOUT_ADAPTIVE;
        } else {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: unsupported layout_mode='%s' "
                   "(need: fixed|adapt|adaptive)\n", args);
            return AVERROR(EINVAL);
        }
        return mixing_cuda_apply_layout_mode(ctx, mode);
    }
    if (!strcmp(cmd, "set_active") && args) {
        char tok[64];
        int lo, hi, val, idx;
        char *dash;
        /* Business slots 1..N (or single 0=bg); range like "1-4 0". */
        if (sscanf(args, "%63s %d", tok, &val) != 2) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: set_active bad args='%s' (need: <slot|lo-hi> <0|1>)\n",
                   args);
            return AVERROR(EINVAL);
        }
        dash = strchr(tok, '-');
        if (dash) {
            char left[32];
            size_t n = (size_t)(dash - tok);
            if (n == 0 || n >= sizeof(left))
                return AVERROR(EINVAL);
            memcpy(left, tok, n);
            left[n] = 0;
            lo = atoi(left);
            hi = atoi(dash + 1);
            /* Range is business pads only (exclude bg 0). */
            if (lo < 1 || hi < 1 || lo >= s->nb_inputs || hi >= s->nb_inputs || lo > hi) {
                av_log(ctx, AV_LOG_ERROR,
                       "mixing_cuda: set_active range '%s' out of business 1..%d\n",
                       tok, s->nb_inputs - 1);
                return AVERROR(EINVAL);
            }
        } else {
            lo = hi = atoi(tok);
            if (lo < 0 || lo >= s->nb_inputs) {
                av_log(ctx, AV_LOG_ERROR,
                       "mixing_cuda: set_active pad %d out of 0..%d\n",
                       lo, s->nb_inputs - 1);
                return AVERROR(EINVAL);
            }
        }
        for (idx = lo; idx <= hi; idx++)
            s->input_active[idx] = val ? 1 : 0;
        s->last_nb_active = -1;  /* force rebuild */
        if (lo != hi)
            av_log(ctx, AV_LOG_WARNING,
                   "mixing_cuda: set_active slots %d-%d = %d\n", lo, hi, val ? 1 : 0);
        else
            av_log(ctx, AV_LOG_WARNING,
                   "mixing_cuda: set_active pad%d = %d\n", lo, val ? 1 : 0);
        return 0;
    }
    if (!strcmp(cmd, "active_speaker") && args) {
        int idx;
        if (s->layout_mode != LAYOUT_SPEAKER) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: active_speaker only in layout_mode=speaker "
                   "(current=%s); try: mixing_cuda layout_mode speaker\n",
                   mixing_cuda_layout_mode_name(s->layout_mode));
            return AVERROR(EINVAL);
        }
        if (sscanf(args, "%d", &idx) != 1 || idx < 0 || idx >= s->nb_inputs ||
            idx == s->bg_input) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: active_speaker bad pad='%s' "
                   "(need business pad 0..%d, not bg_input=%d)\n",
                   args, s->nb_inputs - 1, s->bg_input);
            return AVERROR(EINVAL);
        }
        s->active_speaker = idx;
        s->speaker_candidate = idx;
        s->candidate_since_us = av_gettime_relative();
        s->last_speaker = -1;  /* force rebuild */
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: active_speaker=%d (speaker_switch=%d)\n",
               idx, s->speaker_switch);
        return 0;
    }
    if (!strcmp(cmd, "speaker_layout") && args) {
        if (!strcmp(args, "obs"))       s->speaker_layout = SPK_OBS;
        else if (!strcmp(args, "grid"))  s->speaker_layout = SPK_GRID;
        else {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: unsupported speaker_layout='%s' (obs|grid)\n",
                   args);
            return AVERROR(EINVAL);
        }
        s->last_speaker = -1;
        av_log(ctx, AV_LOG_WARNING, "mixing_cuda: speaker_layout=%s\n", args);
        return 0;
    }
    if (!strcmp(cmd, "speaker_switch") && args) {
        int val = atoi(args);
        s->speaker_switch = val ? 1 : 0;
        av_log(ctx, AV_LOG_WARNING,
               "mixing_cuda: speaker_switch=%d "
               "(%s amixrank audio incentive)\n",
               s->speaker_switch,
               s->speaker_switch ? "follow" : "ignore");
        return 0;
    }
    if (!strcmp(cmd, "primary") && args) {
        int idx;
        if (sscanf(args, "%d", &idx) != 1 || idx < 0 || idx >= s->nb_inputs) {
            av_log(ctx, AV_LOG_ERROR,
                   "mixing_cuda: primary bad pad='%s'\n", args);
            return AVERROR(EINVAL);
        }
        s->primary_input = idx;
        apply_framesync_roles(s);
        av_log(ctx, AV_LOG_WARNING, "mixing_cuda: primary_input=%d\n", idx);
        return 0;
    }

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "mixing_cuda: unsupported/failed cmd='%s' args='%s' -> %s (%d)\n",
               cmd, args ? args : "", av_err2str(ret), ret);
        return ret;
    }
    if (!strcmp(cmd, "fill"))
        apply_fill_from_rgba(s);
    if (!strcmp(cmd, "border_color"))
        apply_border_from_rgba(s);
    if (!strcmp(cmd, "tile_border_color"))
        apply_tile_border_from_rgba(s);
    if (!strcmp(cmd, "primary_input"))
        apply_framesync_roles(s);
    av_log(ctx, AV_LOG_WARNING,
           "mixing_cuda: runtime opt '%s' applied args='%s'\n",
           cmd, args ? args : "");
    return 0;
}

#define OFFSET(x) offsetof(MixingCUDAContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM
#define TFLAGS FLAGS | AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption mixing_cuda_options[] = {
    { "inputs", "number of inputs", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 8}, 1, MIXING_CUDA_MAX_INPUTS, FLAGS },
    { "size", "output size WxH", OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = "1920x1080"}, 0, 0, FLAGS },
    { "s", "output size WxH", OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = "1920x1080"}, 0, 0, FLAGS },
    { "format", "output CUDA sw_format (nv12 or p010; all inputs must match)",
      OFFSET(format_str), AV_OPT_TYPE_STRING, {.str = "nv12"}, 0, 0, FLAGS },
    { "interp", "scaling interpolation", OFFSET(interp), AV_OPT_TYPE_INT, {.i64 = INTERP_BILINEAR}, 0, 1, TFLAGS, .unit = "interp" },
        { "bilinear", "bilinear interpolation", 0, AV_OPT_TYPE_CONST, {.i64 = INTERP_BILINEAR}, 0, 0, TFLAGS, .unit = "interp" },
        { "lanczos", "lanczos2 interpolation", 0, AV_OPT_TYPE_CONST, {.i64 = INTERP_LANCZOS}, 0, 0, TFLAGS, .unit = "interp" },
    { "layout", "layout JSON (VDO.Ninja subset)", OFFSET(layout_str), AV_OPT_TYPE_STRING, {0}, 0, 0, TFLAGS },
    { "layout_file", "layout JSON file (polled each frame)", OFFSET(layout_file), AV_OPT_TYPE_STRING, {0}, 0, 0, TFLAGS },
    { "fill", "background fill color", OFFSET(fill_rgba), AV_OPT_TYPE_COLOR, {.str = "0x101010"}, 0, 0, TFLAGS },
    { "bg_input", "background plate + FrameSync clock (-1=use primary_input as clock)", OFFSET(bg_input), AV_OPT_TYPE_INT, {.i64 = 0}, -1, MIXING_CUDA_MAX_INPUTS - 1, TFLAGS },
    { "on_disconnect", "stall/disconnect behaviour", OFFSET(on_disconnect), AV_OPT_TYPE_INT, {.i64 = ON_DISC_FILL}, 0, 1, TFLAGS, .unit = "od" },
        { "fill", "keep layout, skip stalled panes", 0, AV_OPT_TYPE_CONST, {.i64 = ON_DISC_FILL}, 0, 0, TFLAGS, .unit = "od" },
        { "collapse", "regrid active inputs (1-4 grid)", 0, AV_OPT_TYPE_CONST, {.i64 = ON_DISC_COLLAPSE}, 0, 0, TFLAGS, .unit = "od" },
    { "stall_ms", "Timeout (ms) after last fresh frame when not holding "
                  "(hard disconnect ->fill).",
            OFFSET(stall_ms), AV_OPT_TYPE_INT, {.i64 = 800}, 0, 60000, TFLAGS },
    { "hold_stall_ms", "Timeout (ms) while repeating last frame (dyn_hold). "
                       "Keep > GOP/keyframe wait so thumbs don't black on startup.",
            OFFSET(hold_stall_ms), AV_OPT_TYPE_INT, {.i64 = 3000}, 0, 60000, TFLAGS },
    { "layout_mode", "layout mode", OFFSET(layout_mode), AV_OPT_TYPE_INT, {.i64 = LAYOUT_FIXED}, 0, 2, TFLAGS, .unit = "lm" },
        { "adaptive", "auto grid by active count", 0, AV_OPT_TYPE_CONST, {.i64 = LAYOUT_ADAPTIVE}, 0, 0, TFLAGS, .unit = "lm" },
        { "fixed",    "use JSON layout slots",    0, AV_OPT_TYPE_CONST, {.i64 = LAYOUT_FIXED}, 0, 0, TFLAGS, .unit = "lm" },
        { "speaker",  "speaker-driven layout",    0, AV_OPT_TYPE_CONST, {.i64 = LAYOUT_SPEAKER}, 0, 0, TFLAGS, .unit = "lm" },
    { "speaker_layout", "speaker layout style", OFFSET(speaker_layout), AV_OPT_TYPE_INT, {.i64 = SPK_OBS}, 0, 1, TFLAGS, .unit = "sl" },
        { "obs",  "OBS-style: left main (~75% if <=4, else ~70%) + right strip", 0, AV_OPT_TYPE_CONST, {.i64 = SPK_OBS}, 0, 0, TFLAGS, .unit = "sl" },
        { "grid", "grid-style: top main + bottom filmstrip", 0, AV_OPT_TYPE_CONST, {.i64 = SPK_GRID}, 0, 0, TFLAGS, .unit = "sl" },
    { "rank_endpoint", "zmq SUB endpoint; ignored when task_id set "
                       "(ipc:///data/LCMS/sock/gain_<id>.sock)", OFFSET(rank_endpoint), AV_OPT_TYPE_STRING, {0}, 0, 0, TFLAGS },
    { "task_id", "LCMS task id; prefer ipc gain sock (also -task_id / FFMPEG_TASK_ID)", OFFSET(task_id), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, TFLAGS },
    { "speaker_hold_ms", "speaker switch hold time (ms)", OFFSET(speaker_hold_ms), AV_OPT_TYPE_INT, {.i64 = 500}, 0, 60000, TFLAGS },
    { "join_stable_ms", "Require continuous live this long before adaptive/speaker layout join "
                        "(smooths remove->later update cold insert).",
            OFFSET(join_stable_ms), AV_OPT_TYPE_INT, {.i64 = 300}, 0, 5000, TFLAGS },
    { "speaker_switch_db", "speaker switch loudness diff (dB)", OFFSET(speaker_switch_db), AV_OPT_TYPE_FLOAT, {.dbl = 6.0}, 0, 60, TFLAGS },
    { "speaker_switch", "1=follow amixrank incentive; place speaker in largest/front "
                        "adapt cell (fixed: highlight only). 0=manual active_speaker",
            OFFSET(speaker_switch), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, TFLAGS },
    { "primary_input", "fallback clock input when bg_input=-1 (do NOT point at dynamic RTMP)", OFFSET(primary_input), AV_OPT_TYPE_INT, {.i64 = 0}, 0, MIXING_CUDA_MAX_INPUTS - 1, TFLAGS },
    { "border_px", "speaker highlight border width in pixels (0=off; may thicken tile border)", OFFSET(border_px), AV_OPT_TYPE_INT, {.i64 = 4}, 0, 64, TFLAGS },
    { "border_color", "speaker highlight border color", OFFSET(border_rgba), AV_OPT_TYPE_COLOR, {.str = "0x00FF88"}, 0, 0, TFLAGS },
    { "tile_border_px", "default per-tile border width (0=off; layout/slot_style may override)", OFFSET(tile_border_px), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 64, TFLAGS },
    { "tile_border_color", "default per-tile border color", OFFSET(tile_border_rgba), AV_OPT_TYPE_COLOR, {.str = "0x5B8DEF"}, 0, 0, TFLAGS },
    { "radius_px", "default per-tile corner radius in pixels (0=square; layout/slot_style may override)", OFFSET(radius_px), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 256, TFLAGS },
    { "trans_effect", "tile content switch transition effect",
      OFFSET(trans_effect), AV_OPT_TYPE_INT, {.i64 = TRANS_SLIDE_RIGHT}, TRANS_NONE, TRANS_FLY_DOWN, TFLAGS, .unit = "trans" },
        { "none",        "no transition",               0, AV_OPT_TYPE_CONST, {.i64 = TRANS_NONE},        0, 0, TFLAGS, .unit = "trans" },
        { "fade",        "cross-fade onto underlay",    0, AV_OPT_TYPE_CONST, {.i64 = TRANS_FADE},        0, 0, TFLAGS, .unit = "trans" },
        { "slide_left",  "slide in from right",         0, AV_OPT_TYPE_CONST, {.i64 = TRANS_SLIDE_LEFT},  0, 0, TFLAGS, .unit = "trans" },
        { "slide_right", "slide in from left",          0, AV_OPT_TYPE_CONST, {.i64 = TRANS_SLIDE_RIGHT}, 0, 0, TFLAGS, .unit = "trans" },
        { "slide_up",    "slide in from bottom",        0, AV_OPT_TYPE_CONST, {.i64 = TRANS_SLIDE_UP},    0, 0, TFLAGS, .unit = "trans" },
        { "slide_down",  "slide in from top",           0, AV_OPT_TYPE_CONST, {.i64 = TRANS_SLIDE_DOWN},  0, 0, TFLAGS, .unit = "trans" },
        { "fly_left",    "fly in from right with fade", 0, AV_OPT_TYPE_CONST, {.i64 = TRANS_FLY_LEFT},    0, 0, TFLAGS, .unit = "trans" },
        { "fly_right",   "fly in from left with fade",  0, AV_OPT_TYPE_CONST, {.i64 = TRANS_FLY_RIGHT},   0, 0, TFLAGS, .unit = "trans" },
        { "fly_up",      "fly in from bottom with fade",0, AV_OPT_TYPE_CONST, {.i64 = TRANS_FLY_UP},      0, 0, TFLAGS, .unit = "trans" },
        { "fly_down",    "fly in from top with fade",   0, AV_OPT_TYPE_CONST, {.i64 = TRANS_FLY_DOWN},    0, 0, TFLAGS, .unit = "trans" },
    { "trans_ms", "tile transition duration in milliseconds (0=off)",
      OFFSET(trans_ms), AV_OPT_TYPE_INT, {.i64 = 2000}, 0, 5000, TFLAGS },
    { "sync_warn_interval_ms", "Min interval between mix_sync warnings for one pad",
      OFFSET(sync_warn_interval_ms), AV_OPT_TYPE_INT, {.i64 = 10000}, 1000, 600000, TFLAGS },
    { "eof_action", "action when an input reaches EOF",
        OFFSET(fs.opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, .unit = "eof_action" },
        { "repeat", "Repeat the previous frame.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, .unit = "eof_action" },
        { "endall", "End both streams.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, .unit = "eof_action" },
        { "pass", "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS }, .flags = FLAGS, .unit = "eof_action" },
    { "shortest", "finish when shortest input ends", OFFSET(fs.opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "repeatlast", "repeat last frame after EOF", OFFSET(fs.opt_repeatlast), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(mixing_cuda, MixingCUDAContext, fs);

static const AVFilterPad mixing_cuda_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = mixing_cuda_config_output,
    },
};

const FFFilter ff_vf_mixing_cuda = {
    .p.name        = "mixing_cuda",
    .p.description = NULL_IF_CONFIG_SMALL("Mix multiple CUDA videos with layout JSON (inline scale)"),
    .p.priv_class  = &mixing_cuda_class,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .priv_size     = sizeof(MixingCUDAContext),
    .init          = mixing_cuda_init,
    .uninit        = mixing_cuda_uninit,
    .activate      = mixing_cuda_activate,
    .process_command = mixing_cuda_process_command,
    .p.inputs      = NULL,
    .preinit       = mixing_cuda_framesync_preinit,
    FILTER_OUTPUTS(mixing_cuda_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
