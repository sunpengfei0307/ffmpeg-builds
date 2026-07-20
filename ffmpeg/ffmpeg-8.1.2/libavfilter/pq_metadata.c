/*
 * Shared PQ / HDR10 frame metadata helpers.
 *
 * This file is part of FFmpeg.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/pixfmt.h"

#include "avfilter.h"
#include "pq_metadata.h"

void ff_set_bt2020_mastering_metadata(AVMasteringDisplayMetadata *mdm,
                                      float peak_luminance)
{
    memset(mdm, 0, sizeof(*mdm));

    mdm->display_primaries[0][0] = av_make_q(35400, 50000);
    mdm->display_primaries[0][1] = av_make_q(14600, 50000);
    mdm->display_primaries[1][0] = av_make_q(8500, 50000);
    mdm->display_primaries[1][1] = av_make_q(39850, 50000);
    mdm->display_primaries[2][0] = av_make_q(6500, 50000);
    mdm->display_primaries[2][1] = av_make_q(2300, 50000);
    mdm->white_point[0] = av_make_q(15635, 50000);
    mdm->white_point[1] = av_make_q(16450, 50000);
    mdm->has_primaries = 1;

    mdm->max_luminance = av_make_q((int)(peak_luminance * 10000.0f + 0.5f),
                                   10000);
    mdm->min_luminance = av_make_q(1, 10000);
    mdm->has_luminance = 1;
}

int ff_frame_is_standard_pq(const AVFrame *frame)
{
    if (frame->color_trc != AVCOL_TRC_SMPTE2084)
        return 0;

    if (frame->color_primaries != AVCOL_PRI_BT2020 &&
        frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
        return 0;

    if (frame->colorspace != AVCOL_SPC_BT2020_NCL &&
        frame->colorspace != AVCOL_SPC_UNSPECIFIED)
        return 0;

    return 1;
}

int ff_frame_needs_pq_hdr_side_data(const AVFrame *frame)
{
    return !av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) ||
           !av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
}

static void fill_content_light_metadata(AVContentLightMetadata *clm,
                                        float peak_luminance)
{
    clm->MaxCLL  = (unsigned)av_clip_uintp2((int)(peak_luminance + 0.5f), 16);
    clm->MaxFALL = (unsigned)av_clip_uintp2((int)(peak_luminance * 0.2f + 0.5f), 16);
}

static void apply_standard_color_tags(AVFrame *frame, int repair_only)
{
    if (!repair_only ||
        frame->color_primaries == AVCOL_PRI_UNSPECIFIED)
        frame->color_primaries = AVCOL_PRI_BT2020;

    if (!repair_only ||
        frame->colorspace == AVCOL_SPC_UNSPECIFIED)
        frame->colorspace = AVCOL_SPC_BT2020_NCL;

    if (!repair_only || frame->color_range == AVCOL_RANGE_UNSPECIFIED)
        frame->color_range = AVCOL_RANGE_MPEG;

    if (frame->chroma_location == AVCHROMA_LOC_UNSPECIFIED)
        frame->chroma_location = AVCHROMA_LOC_LEFT;

    if (!repair_only)
        frame->color_trc = AVCOL_TRC_SMPTE2084;
}

int ff_frame_apply_pq_hdr_metadata(AVFilterContext *ctx, AVFrame *frame,
                                   float peak_luminance, enum FFPQMetadataMode mode,
                                   int *metadata_logged)
{
    AVMasteringDisplayMetadata *mdm;
    AVContentLightMetadata *clm;
    AVFrameSideData *sd;
    int repair = mode == FF_PQ_METADATA_REPAIR_MISSING;
    int need_mdm, need_clm;

    if (repair) {
        if (!ff_frame_is_standard_pq(frame))
            return 0;
        if (!ff_frame_needs_pq_hdr_side_data(frame))
            return 0;

        need_mdm = !av_frame_get_side_data(frame,
                                           AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        need_clm = !av_frame_get_side_data(frame,
                                           AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        apply_standard_color_tags(frame, 1);
    } else {
        need_mdm = 1;
        need_clm = 1;
        apply_standard_color_tags(frame, 0);

        av_frame_remove_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        av_frame_remove_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    }

    if (need_mdm) {
        mdm = av_mastering_display_metadata_create_side_data(frame);
        if (!mdm)
            return AVERROR(ENOMEM);
        ff_set_bt2020_mastering_metadata(mdm, peak_luminance);
    }

    if (need_clm) {
        clm = av_content_light_metadata_create_side_data(frame);
        if (!clm)
            return AVERROR(ENOMEM);
        fill_content_light_metadata(clm, peak_luminance);
    }

    if (metadata_logged && !*metadata_logged) {
        unsigned max_cll = 0, max_fall = 0;

        sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        if (sd) {
            clm = (AVContentLightMetadata *)sd->data;
            max_cll  = clm->MaxCLL;
            max_fall = clm->MaxFALL;
        }

        *metadata_logged = 1;
        av_log(ctx, AV_LOG_INFO,
               "PQ HDR metadata %s: max_luminance=%.1f, MaxCLL=%u, MaxFALL=%u\n",
               repair ? "repaired" : "set",
               peak_luminance, max_cll, max_fall);
    }

    return 0;
}
