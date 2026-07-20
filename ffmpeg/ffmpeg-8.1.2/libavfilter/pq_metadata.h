/*
 * Shared PQ / HDR10 frame metadata helpers.
 *
 * This file is part of FFmpeg.
 */

#ifndef AVFILTER_PQ_METADATA_H
#define AVFILTER_PQ_METADATA_H

#include "libavutil/frame.h"
#include "libavutil/mastering_display_metadata.h"

struct AVFilterContext;

enum FFPQMetadataMode {
    /* Overwrite color tags and replace MDM/CLM side data (hlg2pq_cuda). */
    FF_PQ_METADATA_FORCE_ALL = 0,
    /* Only fill missing MDM/CLM on already-tagged PQ/BT.2020 frames. */
    FF_PQ_METADATA_REPAIR_MISSING,
};

void ff_set_bt2020_mastering_metadata(AVMasteringDisplayMetadata *mdm,
                                      float peak_luminance);

int ff_frame_is_standard_pq(const AVFrame *frame);

int ff_frame_needs_pq_hdr_side_data(const AVFrame *frame);

int ff_frame_apply_pq_hdr_metadata(struct AVFilterContext *ctx, AVFrame *frame,
                                   float peak_luminance, enum FFPQMetadataMode mode,
                                   int *metadata_logged);

#endif /* AVFILTER_PQ_METADATA_H */
