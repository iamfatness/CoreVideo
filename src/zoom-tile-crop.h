#pragma once

#include "zoom-tile-grid.h"

#include <algorithm>

// The smallest fraction of the source width a slot crop may leave. Two crops
// summing past this would produce a zero- or negative-width sample rect, which
// is reachable from the properties dialog, so it is clamped rather than
// trusted.
constexpr double kMinCropRemainder = 0.1;

// Applies a slot's left/right crop and then the cover-crop, returning the
// sub-rectangle of the source frame to sample.
//
// Order matters and is the reason this is a tested unit: the slot crop narrows
// the usable source FIRST, and the cover-crop then fills the tile from what is
// left. Cover-cropping first and trimming afterwards keeps a different part of
// the frame — close enough to look plausible on a centred subject, and clearly
// wrong on anyone sitting off-centre.
inline CropRect solve_slot_crop(double src_width, double src_height,
                                double dst_aspect,
                                double crop_left_pct, double crop_right_pct)
{
    CropRect out;
    if (src_width <= 0.0 || src_height <= 0.0 || dst_aspect <= 0.0) return out;

    double left  = std::max(crop_left_pct,  0.0) / 100.0;
    double right = std::max(crop_right_pct, 0.0) / 100.0;

    // Preserve the operator's left/right ratio when scaling an over-crop back,
    // so the framing shifts the way they asked even though it is bounded.
    const double total = left + right;
    const double max_total = 1.0 - kMinCropRemainder;
    if (total > max_total && total > 0.0) {
        const double scale = max_total / total;
        left  *= scale;
        right *= scale;
    }

    const double usable_x = src_width * left;
    const double usable_w = src_width * (1.0 - left - right);

    // Cover-crop within the narrowed region, then translate back into
    // full-frame coordinates.
    const CropRect cover = solve_cover_crop(usable_w, src_height, dst_aspect);
    out.x      = usable_x + cover.x;
    out.y      = cover.y;
    out.width  = cover.width;
    out.height = cover.height;
    return out;
}
