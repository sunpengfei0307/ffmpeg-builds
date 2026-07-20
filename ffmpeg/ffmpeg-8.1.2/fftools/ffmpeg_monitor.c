/*
 * Abnormal timeout monitor + AVS status poster.
 */

#include "ffmpeg.h"
#include "ffmpeg_monitor.h"

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/time.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int abnormal_timeout = -1;
char *avs_poster_zmq_url;
int task_id = 0;
AVSPoster *avs_poster;

static int64_t last_update_time;
static volatile int monitor_got_sigterm;
static ITimer *report_timer;

void ffmpeg_monitor_set_sigterm(int v)
{
    monitor_got_sigterm = v;
}

void ffmpeg_monitor_touch(void)
{
    last_update_time = av_gettime_relative();
}

static void *monitor_worker(void *arg)
{
    (void)arg;
    pthread_detach(pthread_self());
    war("@zombine_monitor is started!(abnormal_timeout=%d s)\n", abnormal_timeout);
    while (!monitor_got_sigterm) {
        if (last_update_time == 0)
            last_update_time = av_gettime_relative();
        int64_t offset = av_gettime_relative() - last_update_time;
        if (offset > (int64_t)abnormal_timeout * 1000000) {
            err("Too long[%ld>=%ds] wait for transcode loop! maybe block, force exit!\n",
                (long)(offset / 1000000), abnormal_timeout);
            term_exit();
            fflush(stdout);
            abort();
        }
        av_usleep(10 * 1000);
    }
    war("@zombine_monitor is exitted!\n");
    return NULL;
}

void zombine_monitor(void)
{
    pthread_t id;
    int ret;
    if ((ret = pthread_create(&id, NULL, monitor_worker, NULL))) {
        av_log(NULL, AV_LOG_ERROR,
               "pthread_create failed: %s.\n", strerror(ret));
        return;
    }
}

AVSPoster *create_avs_poster(const char *zmq_url)
{
    AVSPoster *self = (AVSPoster *)calloc(1, sizeof(AVSPoster));
    av_assert0(self);
    self->obj_zeromq = create_zeromq(ZMQ_PUB, 1, zmq_url);
    av_assert0(self->obj_zeromq);
    return self;
}

void delete_avs_poster(AVSPoster **pptr)
{
    if (pptr && *pptr) {
        AVSPoster *self = *pptr;
        delete_zeromq(&self->obj_zeromq);
        free(self);
        *pptr = NULL;
    }
}

static void shift_share_unit(ShareUnit *u)
{
    for (int i = CNT_SAMPLES - 1; i >= 1; --i) {
        u->updatetime[i] = u->updatetime[i - 1];
        u->tt_pktsize[i] = u->tt_pktsize[i - 1];
        u->nb_packets[i] = u->nb_packets[i - 1];
        u->packet_dts[i] = u->packet_dts[i - 1];
        u->packet_pts[i] = u->packet_pts[i - 1];
        u->rt_bitrate[i] = u->rt_bitrate[i - 1];
        u->nb_mframes[i] = u->nb_mframes[i - 1];
        u->mframe_dts[i] = u->mframe_dts[i - 1];
        u->mframe_pts[i] = u->mframe_pts[i - 1];
    }
}

void ffmpeg_monitor_push_pkt_ts(int64_t *near_pkt_pts, int64_t *near_pkt_dts,
                                int64_t *near_Idr_pts, int64_t *near_Idr_dts,
                                const AVPacket *pkt)
{
    int i;

    if (!pkt)
        return;

    for (i = FF_NEAR_PKT_SAMPLES - 1; i > 0; --i)
        near_pkt_dts[i] = near_pkt_dts[i - 1];
    near_pkt_dts[0] = pkt->dts;

    for (i = FF_NEAR_PKT_SAMPLES - 1; i > 0; --i)
        near_pkt_pts[i] = near_pkt_pts[i - 1];
    near_pkt_pts[0] = pkt->pts;

    if (pkt->flags & AV_PKT_FLAG_KEY) {
        for (i = FF_NEAR_IDR_SAMPLES - 1; i > 0; --i)
            near_Idr_dts[i] = near_Idr_dts[i - 1];
        near_Idr_dts[0] = pkt->dts;

        for (i = FF_NEAR_IDR_SAMPLES - 1; i > 0; --i)
            near_Idr_pts[i] = near_Idr_pts[i - 1];
        near_Idr_pts[0] = pkt->pts;
    }
}

static void update_max_pkt_gaps(ShareUnit *u, const int64_t *near_pkt_pts,
                                const int64_t *near_pkt_dts, AVRational tb)
{
    double tb_d = av_q2d(tb);

    for (int i = FF_NEAR_PKT_SAMPLES - 1; i > 0; --i) {
        double dts_gap = (near_pkt_dts[i - 1] - near_pkt_dts[i]) * tb_d;
        double pts_gap = (near_pkt_pts[i - 1] - near_pkt_pts[i]) * tb_d;

        if (dts_gap > u->max_pkt_dts_gap)
            u->max_pkt_dts_gap = dts_gap;
        if (pts_gap > u->max_pkt_pts_gap)
            u->max_pkt_pts_gap = pts_gap;
    }
}

static double calc_gop_sec(const int64_t *near_Idr_dts, AVRational tb)
{
    if (!near_Idr_dts[0] && !near_Idr_dts[1])
        return 0;
    return (near_Idr_dts[0] - near_Idr_dts[1]) * av_q2d(tb);
}

static void sample_share_unit(ShareUnit *u, int64_t beg_time,
                              uint64_t data_size, uint64_t nb_packets,
                              const int64_t *near_pkt_pts,
                              const int64_t *near_pkt_dts,
                              AVRational tb, uint64_t nb_frames)
{
    double now = (av_gettime_relative() - beg_time) / 1000000.0;
    double tb_d = av_q2d(tb);
    double dt;

    shift_share_unit(u);
    u->updatetime[0] = now;
    u->tt_pktsize[0] = data_size * 8;
    u->nb_packets[0] = nb_packets;
    u->packet_pts[0] = near_pkt_pts[0] * tb_d;
    u->packet_dts[0] = near_pkt_dts[0] * tb_d;
    u->nb_mframes[0] = nb_frames;
    u->mframe_dts[0] = 0;
    u->mframe_pts[0] = 0;

    dt = u->updatetime[0] - u->updatetime[1];
    if (dt > 0)
        u->rt_bitrate[0] = (uint64_t)((u->tt_pktsize[0] - u->tt_pktsize[1]) / dt);
    else
        u->rt_bitrate[0] = 0;

    update_max_pkt_gaps(u, near_pkt_pts, near_pkt_dts, tb);
}

void update_avs_poster(AVSPoster *self, int64_t beg_time)
{
    int32_t vi = 0, ai = 0, vo = 0, ao = 0;
    double v10s_real_secs;
    int64_t v10s_real_eles[4] = {0};
    double v10s_need_eles[4] = {0};

    av_assert0(self);

    for (InputStream *ist = ist_iter(NULL); ist; ist = ist_iter(ist)) {
        if (!ist->par)
            continue;
        if (ist->par->codec_type == AVMEDIA_TYPE_VIDEO && !vi) {
            uint64_t frames = ist->decoder ? ist->decoder->frames_decoded : 0;
            AVRational tb = ist->st ? ist->st->time_base : AV_TIME_BASE_Q;
            sample_share_unit(&self->ist_v_stat.p, beg_time,
                              ist->demux_data_size, ist->demux_nb_packets,
                              ist->near_pkt_pts, ist->near_pkt_dts, tb, frames);
            self->ist_v_stat.p.is_coppied = !ist->decoding_needed;
            self->ist_v_stat.p.rf_bitrate = ist->par->bit_rate;
            self->ist_v_stat.gop = calc_gop_sec(ist->near_Idr_dts, tb);
            self->ist_v_stat.fps = ist->framerate.num
                ? av_q2d(ist->framerate)
                : (ist->st && ist->st->avg_frame_rate.num
                       ? av_q2d(ist->st->avg_frame_rate) : 0);
            self->ist_v_stat.width  = ist->par->width;
            self->ist_v_stat.height = ist->par->height;
            vi = 1;
        }
        if (ist->par->codec_type == AVMEDIA_TYPE_AUDIO && !ai) {
            uint64_t samples = ist->decoder ? ist->decoder->samples_decoded : 0;
            AVRational tb = ist->st ? ist->st->time_base : AV_TIME_BASE_Q;
            sample_share_unit(&self->ist_a_stat.p, beg_time,
                              ist->demux_data_size, ist->demux_nb_packets,
                              ist->near_pkt_pts, ist->near_pkt_dts, tb, samples);
            self->ist_a_stat.p.is_coppied = !ist->decoding_needed;
            self->ist_a_stat.p.rf_bitrate = ist->par->bit_rate;
            self->ist_a_stat.samplerate = ist->par->sample_rate;
            self->ist_a_stat.channels   = ist->par->ch_layout.nb_channels;
            self->ist_a_stat.bitdepth   = ist->par->bits_per_raw_sample;
            self->ist_a_stat.nb_samples = 0;
            ai = 1;
        }
        if (vi && ai)
            break;
    }

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        if (ost->type == AVMEDIA_TYPE_VIDEO && !vo) {
            uint64_t frames = ost->enc ? ost->enc->frames_encoded : 0;
            uint64_t pkts = atomic_load(&ost->packets_written);
            AVRational tb = ost->st ? ost->st->time_base : AV_TIME_BASE_Q;
            sample_share_unit(&self->ost_v_stat.p, beg_time,
                              ost->mux_data_size, pkts,
                              ost->near_pkt_pts, ost->near_pkt_dts, tb, frames);
            if (ost->enc && ost->enc->enc_ctx) {
                AVCodecContext *enc = ost->enc->enc_ctx;
                self->ost_v_stat.p.rf_bitrate = enc->bit_rate;
                self->ost_v_stat.gop = calc_gop_sec(ost->near_Idr_dts, tb);
                self->ost_v_stat.fps = av_q2d(enc->framerate);
                self->ost_v_stat.width  = enc->width;
                self->ost_v_stat.height = enc->height;
            } else {
                self->ost_v_stat.p.rf_bitrate = self->ist_v_stat.p.rf_bitrate;
                self->ost_v_stat.gop    = self->ist_v_stat.gop;
                self->ost_v_stat.fps    = self->ist_v_stat.fps;
                self->ost_v_stat.width  = self->ist_v_stat.width;
                self->ost_v_stat.height = self->ist_v_stat.height;
            }
            vo = 1;
        }
        if (ost->type == AVMEDIA_TYPE_AUDIO && !ao) {
            uint64_t samples = ost->enc ? ost->enc->samples_encoded : 0;
            uint64_t pkts = atomic_load(&ost->packets_written);
            AVRational tb = ost->st ? ost->st->time_base : AV_TIME_BASE_Q;
            sample_share_unit(&self->ost_a_stat.p, beg_time,
                              ost->mux_data_size, pkts,
                              ost->near_pkt_pts, ost->near_pkt_dts, tb, samples);
            if (ost->enc && ost->enc->enc_ctx) {
                AVCodecContext *enc = ost->enc->enc_ctx;
                self->ost_a_stat.p.rf_bitrate = enc->bit_rate;
                self->ost_a_stat.samplerate = enc->sample_rate;
                self->ost_a_stat.channels   = enc->ch_layout.nb_channels;
                self->ost_a_stat.bitdepth   = enc->bits_per_raw_sample;
                self->ost_a_stat.nb_samples = 0;
            } else {
                self->ost_a_stat.p.rf_bitrate = self->ist_a_stat.p.rf_bitrate;
                self->ost_a_stat.samplerate = self->ist_a_stat.samplerate;
                self->ost_a_stat.channels   = self->ist_a_stat.channels;
                self->ost_a_stat.bitdepth   = self->ist_a_stat.bitdepth;
                self->ost_a_stat.nb_samples = self->ist_a_stat.nb_samples;
            }
            ao = 1;
        }
        if (vo && ao)
            break;
    }

    self->runnigtime = (av_gettime_relative() - beg_time) / 1000000.0;
    self->ssid++;

    v10s_real_secs = self->ist_v_stat.p.updatetime[0] - self->ist_v_stat.p.updatetime[10];
    if (v10s_real_secs <= 0)
        v10s_real_secs = 10.0;
    self->v10s_real_secs = v10s_real_secs;

    v10s_need_eles[0] = round(self->ist_v_stat.fps * v10s_real_secs);
    v10s_real_eles[0] = (int64_t)(self->ist_v_stat.p.nb_packets[0] - self->ist_v_stat.p.nb_packets[10]);
    self->v10s_real_eles[0] = v10s_real_eles[0];
    self->ist_v_stat.p.vpkt_loss_rate[2] = self->ist_v_stat.p.vpkt_loss_rate[1];
    self->ist_v_stat.p.vpkt_loss_rate[1] = self->ist_v_stat.p.vpkt_loss_rate[0];
    self->ist_v_stat.p.vpkt_loss_rate[0] =
        v10s_need_eles[0] > 0
            ? (v10s_need_eles[0] - v10s_real_eles[0]) / v10s_need_eles[0]
            : 0;

    v10s_need_eles[1] = v10s_need_eles[0];
    v10s_real_eles[1] = (int64_t)(self->ist_v_stat.p.nb_mframes[0] - self->ist_v_stat.p.nb_mframes[10]);
    self->v10s_real_eles[1] = v10s_real_eles[1];
    self->ist_v_stat.p.vfrm_loss_rate[2] = self->ist_v_stat.p.vfrm_loss_rate[1];
    self->ist_v_stat.p.vfrm_loss_rate[1] = self->ist_v_stat.p.vfrm_loss_rate[0];
    self->ist_v_stat.p.vfrm_loss_rate[0] =
        v10s_need_eles[1] > 0
            ? (v10s_need_eles[1] - v10s_real_eles[1]) / v10s_need_eles[1]
            : 0;

    v10s_need_eles[2] = round(self->ost_v_stat.fps * v10s_real_secs);
    v10s_real_eles[2] = (int64_t)(self->ost_v_stat.p.nb_mframes[0] - self->ost_v_stat.p.nb_mframes[10]);
    self->v10s_real_eles[2] = v10s_real_eles[2];
    self->ost_v_stat.p.vfrm_loss_rate[2] = self->ost_v_stat.p.vfrm_loss_rate[1];
    self->ost_v_stat.p.vfrm_loss_rate[1] = self->ost_v_stat.p.vfrm_loss_rate[0];
    self->ost_v_stat.p.vfrm_loss_rate[0] =
        v10s_need_eles[2] > 0
            ? (v10s_need_eles[2] - v10s_real_eles[2]) / v10s_need_eles[2]
            : 0;

    v10s_need_eles[3] = v10s_need_eles[2];
    v10s_real_eles[3] = (int64_t)(self->ost_v_stat.p.nb_packets[0] - self->ost_v_stat.p.nb_packets[10]);
    self->v10s_real_eles[3] = v10s_real_eles[3];
    self->ost_v_stat.p.vpkt_loss_rate[2] = self->ost_v_stat.p.vpkt_loss_rate[1];
    self->ost_v_stat.p.vpkt_loss_rate[1] = self->ost_v_stat.p.vpkt_loss_rate[0];
    self->ost_v_stat.p.vpkt_loss_rate[0] =
        v10s_need_eles[3] > 0
            ? (v10s_need_eles[3] - v10s_real_eles[3]) / v10s_need_eles[3]
            : 0;
}

int publish_avs_poster(AVSPoster *self, int64_t total_size_bits, double rts)
{
    char send_buf[1024];

    if (!self || !self->obj_zeromq)
        return 0;

    snprintf(send_buf, sizeof(send_buf),
             "zmq://status_publish?index=%d&ist_pkts=%llu&ost_pkts=%llu&ist_fps=%.3lf&ost_fps=%.3lf&size=%lld&base_vb=%lld&base_ab=%lld&in_video_pts=%.3lf&out_video_pts=%.3lf&max_ist_v_skip_dts=%.3lf&in_audio_pts=%.3lf&out_audio_pts=%.3lf&max_ist_a_skip_dts=%.3lf&src_video_gop=%.3lf&dst_video_gop=%.3lf&rts=%.3lf&vb_volatility=%.3lf&input_v_loss=%.3lf&input_a_loss=%.3lf&low_fps_second=%d",
             (int)self->ssid,
             (unsigned long long)self->ist_v_stat.p.nb_packets[0],
             (unsigned long long)self->ost_v_stat.p.nb_packets[0],
             self->ist_v_stat.fps, self->ost_v_stat.fps,
             (long long)total_size_bits,
             (long long)self->ost_v_stat.p.rf_bitrate,
             (long long)self->ost_a_stat.p.rf_bitrate,
             self->ist_v_stat.p.packet_pts[0], self->ost_v_stat.p.packet_pts[0],
             self->ist_v_stat.p.max_pkt_dts_gap,
             self->ist_a_stat.p.packet_pts[0], self->ost_a_stat.p.packet_pts[0],
             self->ist_a_stat.p.max_pkt_dts_gap,
             self->ist_v_stat.gop, self->ost_v_stat.gop, rts, 0.0, 0.0, 0.0, 0);
    dbg("%s\n", send_buf);
    if (zmqbuf_send(self->obj_zeromq, send_buf, strlen(send_buf), 0) == -1) {
        err("zmq publish failed! avs_poster->obj_zeromq=%p\n", self->obj_zeromq);
        return -1;
    }
    return 0;
}

void ffmpeg_monitor_format_status_line(char *buf, size_t buf_size)
{
    double use_fps, avg_10s_fps, secs;
    uint64_t sum_10s_pkts;

    if (!buf || !buf_size)
        return;
    if (!avs_poster) {
        buf[0] = 0;
        return;
    }

    secs = avs_poster->v10s_real_secs > 0 ? avs_poster->v10s_real_secs : 10.0;
    use_fps = avs_poster->ist_v_stat.p.is_coppied
                  ? avs_poster->ost_v_stat.fps
                  : avs_poster->ist_v_stat.fps;
    if (avs_poster->ist_v_stat.p.is_coppied)
        sum_10s_pkts = (uint64_t)FFMAX(avs_poster->v10s_real_eles[3], 0);
    else
        sum_10s_pkts = (uint64_t)FFMAX(avs_poster->v10s_real_eles[1], 0);
    avg_10s_fps = sum_10s_pkts / secs;

    snprintf(buf, buf_size,
             "@copy=%d, use_fps=%.3lf(ist_fps=%.3lf, ost_fps=%.3lf),  "
             "sum_10s_pkts=%llu, avg_10s_fps=%.3lf",
             avs_poster->ist_v_stat.p.is_coppied, use_fps,
             avs_poster->ist_v_stat.fps, avs_poster->ost_v_stat.fps,
             (unsigned long long)sum_10s_pkts, avg_10s_fps);
}

void ffmpeg_monitor_format_rdew(char *buf, size_t buf_size)
{
    if (!buf || !buf_size)
        return;
    if (!avs_poster) {
        snprintf(buf, buf_size, "[%s]", get_fmttime());
        return;
    }

    snprintf(buf, buf_size,
             "[%s] R:%5llu(diff=%llu) D:%5llu(diff=%llu, 10sLost=%.2lf%%) "
             "E:%5llu(diff=%llu) W:%5llu(diff=%llu)",
             get_fmttime(),
             (unsigned long long)avs_poster->ist_v_stat.p.nb_packets[0],
             (unsigned long long)(avs_poster->ist_v_stat.p.nb_packets[0] -
                                  avs_poster->ist_v_stat.p.nb_packets[1]),
             (unsigned long long)avs_poster->ist_v_stat.p.nb_mframes[0],
             (unsigned long long)(avs_poster->ist_v_stat.p.nb_mframes[0] -
                                  avs_poster->ist_v_stat.p.nb_mframes[1]),
             avs_poster->ist_v_stat.p.vfrm_loss_rate[0] * 100.0,
             (unsigned long long)avs_poster->ost_v_stat.p.nb_mframes[0],
             (unsigned long long)(avs_poster->ost_v_stat.p.nb_mframes[0] -
                                  avs_poster->ost_v_stat.p.nb_mframes[1]),
             (unsigned long long)avs_poster->ost_v_stat.p.nb_packets[0],
             (unsigned long long)(avs_poster->ost_v_stat.p.nb_packets[0] -
                                  avs_poster->ost_v_stat.p.nb_packets[1]));
}

int ffmpeg_monitor_check_abort(float running_secs)
{
    double *v10s_lossrate;
    uint64_t v10s_elements;

    if (!avs_poster || abnormal_timeout <= 0)
        return 0;

    v10s_lossrate = avs_poster->ist_v_stat.p.is_coppied
                        ? avs_poster->ost_v_stat.p.vpkt_loss_rate
                        : avs_poster->ist_v_stat.p.vfrm_loss_rate;
    v10s_elements = avs_poster->ist_v_stat.p.is_coppied
                        ? (uint64_t)FFMAX(avs_poster->v10s_real_eles[3], 0)
                        : (uint64_t)FFMAX(avs_poster->v10s_real_eles[1], 0);

    if (running_secs > 60 &&
        (avs_poster->ost_v_stat.fps > 0 && avs_poster->ost_v_stat.fps <= 120) &&
        ((v10s_lossrate[0] > 0.85 && v10s_lossrate[0] > v10s_lossrate[1] &&
          v10s_lossrate[1] > v10s_lossrate[2]) ||
         v10s_elements <= 10)) {
        err("[ERROR] running time=%.3lf s, ist_fps=%.2lf, ost_fps=%.2lf, "
            "detect cur_10s_loss_rate=%.3lf is too large! ABORT!\n",
            running_secs, avs_poster->ist_v_stat.fps, avs_poster->ost_v_stat.fps,
            v10s_lossrate[0]);
        return -1;
    }
    return 0;
}

void ffmpeg_monitor_start_report_timer(void (*cb)(void *, void *))
{
    if (report_timer || !cb)
        return;
    /* Align 6.1.1: first fire at 1s, then every 1s — absolute wall clock. */
    report_timer = new_timer(0, 1000, 1000, -1, cb, NULL);
    if (!report_timer)
        err("failed to create 1s report timer\n");
}

void ffmpeg_monitor_stop_report_timer(void)
{
    del_timer(&report_timer);
}

void ffmpeg_monitor_init(void)
{
    char url_buf[256];
    const char *url = NULL;

    /* Align 6.1.1: -task_id alone implies ipc:///data/LCMS/sock/<id>_running_status.sock */
    if (avs_poster_zmq_url && avs_poster_zmq_url[0]) {
        url = avs_poster_zmq_url;
    } else if (task_id != 0) {
        snprintf(url_buf, sizeof(url_buf),
                 "ipc:///data/LCMS/sock/%d_running_status.sock", task_id);
        url = url_buf;
    }

    if (url) {
        avs_poster = create_avs_poster(url);
        msg("@avs_poster created url=%s task_id=%d\n", url, task_id);
    }
    if (abnormal_timeout > 0)
        zombine_monitor();
}

void ffmpeg_monitor_uninit(void)
{
    ffmpeg_monitor_set_sigterm(1);
    ffmpeg_monitor_stop_report_timer();
    delete_avs_poster(&avs_poster);
}
