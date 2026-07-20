/*
 * SEI demux extract / mux inject.
 */

#include "ffmpeg.h"
#include "ffmpeg_sei.h"
#include "spfutils.h"
#include "utils_stream.h"

#include "libavutil/mem.h"

#include <stdlib.h>
#include <string.h>

int copy_sei;

static unsigned char get_sei_payload_type(int32_t app_type)
{
    unsigned char payload_type = 0x05;
    switch (app_type) {
    case SEI_AI_SPORT_TEXT:
    case SEI_SUBTITLE_TEXT:
    case SEI_SUBTITLE_FAST:
        payload_type = 0xF6;
        break;
    case SEI_HDR_VIVID:
        payload_type = 0x04;
        break;
    default:
        payload_type = 0x05;
        break;
    }
    return payload_type;
}

static int32_t get_uuid(int32_t app_type, unsigned char *uuid, int32_t uuid_cap)
{
    unsigned char UUID_IQY[6] = {0x4D, 0x43, 0x54, 0x4F, 0x00, 0x09};
    int32_t uuid_size = (int32_t)sizeof(UUID_IQY);

    if (!uuid)
        return 0;

    switch (app_type) {
    case SEI_AI_SPORT_TEXT:
        UUID_IQY[sizeof(UUID_IQY) - 1] = 0x09;
        break;
    case SEI_SUBTITLE_TEXT:
        UUID_IQY[sizeof(UUID_IQY) - 1] = 0x0A;
        break;
    case SEI_SUBTITLE_FAST:
        UUID_IQY[sizeof(UUID_IQY) - 1] = 0x0D;
        break;
    default:
        memset(uuid, 0, uuid_cap);
        return 0;
    }
    memcpy(uuid, UUID_IQY, uuid_size);
    return uuid_size;
}

void add_sei2cacher(int64_t index, int64_t pts, int64_t dts,
                    uint8_t *data, uint32_t size)
{
    SEIPacket *rpkt;
    uint32_t i = 0;

    if (!(data[0] == 0xFF && data[1] == 0x1E))
        return;

    rpkt = (SEIPacket *)calloc(1, sizeof(SEIPacket) + size);
    if (!rpkt)
        return;
    rpkt->type = SEI_AI_EBSP;
    rpkt->ssid = index;
    rpkt->ipts = pts;
    rpkt->idts = dts;
    rpkt->opts = 0;
    rpkt->odts = 0;
    rpkt->tick = get_curtime();
    rpkt->size = size;
    memcpy(rpkt->data, data, size);

    for (OutputStream *ost = ost_iter(NULL); ost; ost = ost_iter(ost)) {
        SEIPacket *tpkt = rpkt;

        if (ost->type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (i++ > 0) {
            tpkt = (SEIPacket *)calloc(1, sizeof(SEIPacket) + size);
            if (!tpkt)
                continue;
            memcpy(tpkt, rpkt, sizeof(SEIPacket) + size);
        }
        if (!ost->usr_sei_dict)
            continue;
        htable_insert((IHtable **)&ost->usr_sei_dict, tpkt->ipts, (void *)tpkt);
        if (ost->min_sei_pts <= 0 || tpkt->ipts < ost->min_sei_pts)
            ost->min_sei_pts = tpkt->ipts;
        out("@output[%d] find and insert user SEI: pts=%ld, dts=%ld, size=%u, "
            "ost[%d].counts=%d, ssid=%ld, min_sei_pts=%ld\n",
            ost->file->index, (long)pts, (long)dts, size, ost->index,
            htable_counts((IHtable **)&ost->usr_sei_dict), (long)index,
            (long)ost->min_sei_pts);
    }
}

void parse_sei_nalus(enum AVCodecID codec_id, uint8_t *data, size_t data_size,
                     int64_t pts, int64_t dts, int64_t index)
{
    size_t offset = 0;

    if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_HEVC)
        return;

    while (offset + 4 < data_size) {
        uint32_t nalu_size = (data[offset] << 24) | (data[offset + 1] << 16) |
                             (data[offset + 2] << 8) | data[offset + 3];
        uint8_t nal_unit_type;

        offset += 4;
        if (offset + nalu_size > data_size)
            break;

        if (codec_id == AV_CODEC_ID_H264) {
            nal_unit_type = data[offset] & 0x1F;
            if (nal_unit_type == 6)
                add_sei2cacher(index, pts, dts, data + offset + 1, nalu_size - 1);
        } else {
            nal_unit_type = (data[offset] >> 1) & 0x3F;
            if (nal_unit_type == 39 || nal_unit_type == 40)
                add_sei2cacher(index, pts, dts, data + offset + 1, nalu_size - 1);
        }
        offset += nalu_size;
    }
}

int ffmpeg_sei_inject_mux_pkt(OutputStream *ost, AVPacket *pkt)
{
    AVStream *st;
    int64_t src_cur_pkt_pts, src_cur_pkt_dts;
    SEIPacket *rpkt;
    unsigned char uuid[16] = {0};
    int32_t uuid_size;
    bool is_hevc;
    uint32_t nalu_size, nalu_size_with_mark;
    unsigned char *nalu_data_with_mark = NULL;
    char *old_pkt_data = NULL;
    int old_pkt_size;
    int ret = 0;

    if (copy_sei != 1 || !ost || !pkt || !ost->usr_sei_dict)
        return 0;

    st = ost->st;
    if (!st || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return 0;
    if (htable_counts((IHtable **)&ost->usr_sei_dict) <= 0)
        return 0;

    src_cur_pkt_pts = (int64_t)(pkt->pts * av_q2d(st->time_base) * 1000);
    src_cur_pkt_dts = (int64_t)(pkt->dts * av_q2d(st->time_base) * 1000);

    if (htable_counts((IHtable **)&ost->usr_sei_dict) > 1000) {
        err("[ALERT] usr_sei_dict size=%u too large!(>=1000) maybe abnormal, reset!\n",
            htable_counts((IHtable **)&ost->usr_sei_dict));
        htable_delete((IHtable **)&ost->usr_sei_dict, src_cur_pkt_pts, free);
    }

    rpkt = (SEIPacket *)htable_search_near((IHtable **)&ost->usr_sei_dict, src_cur_pkt_pts);
    if (!rpkt) {
        err("@ost->file_index=%d (pkt.pts=%lld ms, pkt.dts=%lld ms), not copy user sei!\n",
            ost->file->index, (long long)src_cur_pkt_pts, (long long)src_cur_pkt_dts);
        return 0;
    }

    msg("@ost->file_index=%d (pkt.pts=%lld ms, pkt.dts=%lld ms), copy_user_sei:"
        "(ipts=%lld ms, idts=%lld ms, size=%d, ssid=%d)\n",
        ost->file->index, (long long)src_cur_pkt_pts, (long long)src_cur_pkt_dts,
        (long long)rpkt->ipts, (long long)rpkt->idts, rpkt->size, (int)rpkt->ssid);

    is_hevc = st->codecpar->codec_id == AV_CODEC_ID_HEVC;
    uuid_size = get_uuid(rpkt->type, uuid, (int32_t)sizeof(uuid));
    nalu_size = get_sei_nalu_size(is_hevc, uuid_size, rpkt->size);
    nalu_data_with_mark = (unsigned char *)av_calloc(1, 2 * nalu_size);
    if (!nalu_data_with_mark)
        return AVERROR(ENOMEM);

    nalu_size_with_mark = build_sei_nalu_packet(
        uuid, uuid_size, get_sei_payload_type(rpkt->type), is_hevc,
        ost->enc ? 1 : 0, nalu_data_with_mark, 2 * nalu_size,
        (const char *)rpkt->data, rpkt->size);

    old_pkt_size = pkt->size;
    old_pkt_data = (char *)av_calloc(1, pkt->size);
    if (!old_pkt_data) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    memcpy(old_pkt_data, pkt->data, pkt->size);

    if (av_grow_packet(pkt, nalu_size_with_mark) < 0) {
        av_log(NULL, AV_LOG_ERROR, "@av_grow_packet sei_len=%d failed!\n",
               nalu_size_with_mark);
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    memset(pkt->data, 0, pkt->size);
    memcpy(pkt->data, nalu_data_with_mark, nalu_size_with_mark);
    memcpy(pkt->data + nalu_size_with_mark, old_pkt_data, old_pkt_size);

    htable_delete((IHtable **)&ost->usr_sei_dict, src_cur_pkt_pts, free);

cleanup:
    av_freep(&old_pkt_data);
    av_freep(&nalu_data_with_mark);
    return ret;
}
