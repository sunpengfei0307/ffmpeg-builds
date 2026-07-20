/*
 * Repair missing PQ / HDR10 side data on standard PQ frames.
 *
 * Pass-through filter: pixels are unchanged. When enabled, frames tagged as
 * SMPTE ST 2084 (PQ) with BT.2020 primaries/matrix but without mastering
 * display or content light level side data receive the same default HDR
 * metadata used by hlg2pq_cuda.
 *
 * Works on software and hardware (CUDA, etc.) frames alike.
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/opt.h"

#include "avfilter.h"
#include "filters.h"
#include "pq_metadata.h"
#include "video.h"

typedef struct RepairPQMetadataContext {
    const AVClass *class;
    int enable;
    float peak_luminance;
    int metadata_logged;
} RepairPQMetadataContext;

static int repair_pq_metadata_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    RepairPQMetadataContext *s = ctx->priv;
    int ret;

    if (!s->enable)
        return ff_filter_frame(ctx->outputs[0], frame);

    ret = ff_frame_apply_pq_hdr_metadata(ctx, frame, s->peak_luminance,
                                         FF_PQ_METADATA_REPAIR_MISSING,
                                         &s->metadata_logged);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    return ff_filter_frame(ctx->outputs[0], frame);
}

#define OFFSET(x) offsetof(RepairPQMetadataContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption repair_pq_metadata_options[] = {
    { "enable", "Enable PQ HDR side data repair",
      OFFSET(enable), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "peak_luminance", "Target PQ mastering peak luminance in nits",
      OFFSET(peak_luminance), AV_OPT_TYPE_FLOAT, { .dbl = 1000.0 },
      100.0, 10000.0, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(repair_pq_metadata);

static const AVFilterPad repair_pq_metadata_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = repair_pq_metadata_filter_frame,
    },
};

const FFFilter ff_vf_repair_pq_metadata = {
    .p.name        = "repair_pq_metadata",
    .p.description = NULL_IF_CONFIG_SMALL("Fill missing PQ/HDR10 side data on standard PQ frames"),
    .p.priv_class  = &repair_pq_metadata_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC |
                     AVFILTER_FLAG_METADATA_ONLY,
    .priv_size     = sizeof(RepairPQMetadataContext),
    FILTER_INPUTS(repair_pq_metadata_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};
