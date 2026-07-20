/*
 * Abnormal timeout monitor + AVS status poster lifecycle.
 */
#ifndef FFTOOLS_FFMPEG_MONITOR_H
#define FFTOOLS_FFMPEG_MONITOR_H

#include <stdint.h>
#include "libavcodec/packet.h"
#include "spfutils.h"

#define CNT_SAMPLES 60

typedef struct ShareUnit {
    double   updatetime[CNT_SAMPLES];
    int32_t  is_coppied;
    uint64_t rf_bitrate;
    uint64_t tt_pktsize[CNT_SAMPLES];
    uint64_t nb_packets[CNT_SAMPLES];
    double   packet_dts[CNT_SAMPLES];
    double   packet_pts[CNT_SAMPLES];
    uint64_t rt_bitrate[CNT_SAMPLES];
    double   max_pkt_dts_gap;
    double   max_pkt_pts_gap;
    double   vpkt_loss_rate[3];
    double   apkt_loss_rate[3];
    uint64_t nb_mframes[CNT_SAMPLES];
    double   mframe_dts[CNT_SAMPLES];
    double   mframe_pts[CNT_SAMPLES];
    double   max_frm_dts_gap;
    double   max_frm_pts_gap;
    double   vfrm_loss_rate[3];
    double   afrm_loss_rate[3];
} ShareUnit;

typedef struct VideoUnit {
    ShareUnit p;
    double    gop;
    double    fps;
    int32_t   width;
    int32_t   height;
} VideoUnit;

typedef struct AudioUnit {
    ShareUnit p;
    int32_t   samplerate;
    int32_t   channels;
    int32_t   nb_samples;
    int32_t   bitdepth;
} AudioUnit;

typedef struct AVSPoster {
    IZeromq  *obj_zeromq;
    VideoUnit ist_v_stat;
    AudioUnit ist_a_stat;
    VideoUnit ost_v_stat;
    AudioUnit ost_a_stat;
    uint64_t  ssid;
    double    runnigtime;
    double    v10s_real_secs;
    int64_t   v10s_real_eles[4]; /* R/D/E/W counts in last ~10s window */
} AVSPoster;

extern int   abnormal_timeout;
extern char *avs_poster_zmq_url;
extern int   task_id;
extern AVSPoster *avs_poster;

void zombine_monitor(void);
void ffmpeg_monitor_touch(void);
void ffmpeg_monitor_set_sigterm(int v);

/* Update near_pkt_* / near_Idr_* sliding windows (6.1.1 demux/mux behavior). */
void ffmpeg_monitor_push_pkt_ts(int64_t *near_pkt_pts, int64_t *near_pkt_dts,
                                int64_t *near_Idr_pts, int64_t *near_Idr_dts,
                                const AVPacket *pkt);

AVSPoster *create_avs_poster(const char *zmq_url);
void       delete_avs_poster(AVSPoster **pptr);
void       update_avs_poster(AVSPoster *self, int64_t beg_time);
int        publish_avs_poster(AVSPoster *self, int64_t total_size_bits, double rts);

/* Format @copy/use_fps and R/D/E/W prefixes for print_report. */
void ffmpeg_monitor_format_status_line(char *buf, size_t buf_size);
void ffmpeg_monitor_format_rdew(char *buf, size_t buf_size);
/* Loss-rate abort check after update. Returns <0 to force exit. */
int  ffmpeg_monitor_check_abort(float running_secs);

/* 1s wall-clock report timer (print_report + ZMQ post). */
void ffmpeg_monitor_start_report_timer(void (*cb)(void *, void *));
void ffmpeg_monitor_stop_report_timer(void);

void ffmpeg_monitor_init(void);
void ffmpeg_monitor_uninit(void);

#endif /* FFTOOLS_FFMPEG_MONITOR_H */
