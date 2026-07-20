/*
 * SEI copy helpers for fftools (demux extract / mux inject).
 */
#ifndef FFTOOLS_FFMPEG_SEI_H
#define FFTOOLS_FFMPEG_SEI_H

#include <stdint.h>
#include "libavcodec/codec_id.h"
#include "libavcodec/packet.h"

typedef enum SeiPktType {
    SEI_INVALID = 0,
    SEI_AI_SPORT_TEXT,
    SEI_SUBTITLE_TEXT,
    SEI_SUBTITLE_FAST,
    SEI_ABR,
    SEI_HDR_VIVID,
    SEI_AI_EBSP,
    SEI_USR,
} SeiPktType;

typedef struct SEIPacket {
    int32_t  type;
    uint64_t ssid;
    int32_t  code;
    int64_t  ipts;
    int64_t  opts;
    int64_t  idts;
    int64_t  odts;
    int64_t  tick;
    int32_t  size;
    char     data[0];
} SEIPacket;

extern int copy_sei;

void add_sei2cacher(int64_t index, int64_t pts, int64_t dts,
                    uint8_t *data, uint32_t size);
void parse_sei_nalus(enum AVCodecID codec_id, uint8_t *data, size_t data_size,
                     int64_t pts, int64_t dts, int64_t index);

/* Inject cached user SEI into a video mux packet. Returns 0 or AVERROR. */
int ffmpeg_sei_inject_mux_pkt(struct OutputStream *ost, AVPacket *pkt);

#endif /* FFTOOLS_FFMPEG_SEI_H */
