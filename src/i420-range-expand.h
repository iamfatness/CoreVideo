#pragma once

// Expanding a limited-range (studio swing) I420 frame to full range.
//
// Extracted so it can be tested without the Zoom SDK or libobs, the same
// treatment director-handover.h and iso-video-pacer.h get, and for the same
// reason: the only symptom of a regression is a one-frame brightness pop on a
// live broadcast, which no unit test would otherwise catch.
//
// THE DEFECT THIS EXISTS FOR (2026-08-22, live meeting, root-caused from the
// video SHM by an external read-only probe). The engine asks the SDK for full
// range -- VideoRawdataColorspace_BT709_F, engine/src/main.cpp -- and the
// plugin unconditionally declares VIDEO_RANGE_FULL on every frame it hands
// OBS (set_yuv_frame_color_info in src/zoom-source.cpp). The SDK does NOT
// always honour that request. Measured over 15,203 consecutive frames from a
// real meeting: 16 frames arrived limited-range instead, spread across 5 of 6
// participants. Each one is unmistakable in the luma histogram -- floor lifted
// from 0 to 12-20, ceiling capped from 255 down to 211-238, and the ~13,600
// sub-16 samples its neighbours carry collapsing to single digits:
//
//     seq 226978   mean 33.6   min 0    max 255   under16 13670
//     seq 226994   mean 46.7   min 12   max 234   under16     3   <-- limited
//     seq 227008   mean 33.7   min 0    max 255   under16 13628
//
// Declared full but actually limited, that frame renders unexpanded: blacks
// lift, whites crush, and at 30fps it is a ~33 ms brightness pop. With 6-8
// tiles on air one lands somewhere every 20-30 seconds, which is the "gamma
// flash" operators had been reporting for days. It is also why program output
// measured washed out against mimoLive on 2026-08-11.
//
// The SDK does tell us, per frame -- YUVRawDataI420::IsLimitedI420(). Nothing
// in this codebase had ever called it.
//
// WHY EXPAND THE PIXELS RATHER THAN JUST TAG THE FRAME. The tempting fix is to
// pass the flag through and set obs_source_frame::full_range per frame. Do not:
// libobs keys async texture allocation on that field, and set_async_texture_size
// (libobs/obs-source.c) destroys and recreates EVERY async texture whenever it
// changes. On content that alternates -- which is exactly what this defect is --
// that trades a one-frame pop for a texture-rebuild storm. Normalising the
// pixels instead keeps the declared range constant at full for the whole
// session, so OBS never rebuilds and every frame genuinely is what we say it is.

#include <cstddef>
#include <cstdint>

// Studio-swing limits. Luma occupies 16..235, chroma 16..240 about a 128
// neutral point; anything outside is legal-but-out-of-gamut (superblack /
// superwhite) and clamps rather than wrapping.
inline constexpr uint8_t kI420LimitedLumaMin   = 16;
inline constexpr uint8_t kI420LimitedLumaMax   = 235;
inline constexpr uint8_t kI420LimitedChromaMin = 16;
inline constexpr uint8_t kI420LimitedChromaMax = 240;

namespace cv_i420_detail {

// Integer math with explicit rounding, not floating point: this runs per pixel
// on every plane of a 1080p frame, and a build that contracts the divide
// differently must not shift the picture by a code value.
inline uint8_t expand_luma(uint8_t v)
{
    if (v <= kI420LimitedLumaMin) return 0;
    if (v >= kI420LimitedLumaMax) return 255;
    const int span = kI420LimitedLumaMax - kI420LimitedLumaMin; // 219
    const int num  = (v - kI420LimitedLumaMin) * 255 + span / 2;
    return static_cast<uint8_t>(num / span);
}

inline uint8_t expand_chroma(uint8_t v)
{
    if (v <= kI420LimitedChromaMin) return 0;
    if (v >= kI420LimitedChromaMax) return 255;
    const int span = kI420LimitedChromaMax - kI420LimitedChromaMin; // 224
    const int num  = (v - kI420LimitedChromaMin) * 255 + span / 2;
    return static_cast<uint8_t>(num / span);
}

struct ExpandTables {
    uint8_t luma[256];
    uint8_t chroma[256];
    ExpandTables()
    {
        for (int i = 0; i < 256; ++i) {
            luma[i]   = expand_luma(static_cast<uint8_t>(i));
            chroma[i] = expand_chroma(static_cast<uint8_t>(i));
        }
    }
};

inline const ExpandTables &tables()
{
    static const ExpandTables t;
    return t;
}

} // namespace cv_i420_detail

// Single-sample entry points. The tables are the hot path; these exist so the
// mapping itself can be asserted directly by the tests.
inline uint8_t i420_expand_luma_sample(uint8_t v)
{
    return cv_i420_detail::tables().luma[v];
}

inline uint8_t i420_expand_chroma_sample(uint8_t v)
{
    return cv_i420_detail::tables().chroma[v];
}

// Expand a whole limited-range I420 frame into `dst_*`.
//
// Planes are the SDK's own layout: Y is `y_len` bytes, U and V are `y_len / 4`
// each. src and dst may be the same pointers (expansion is per-sample and
// order-independent, so in-place is safe). A null pointer or a y_len that is
// not a whole number of chroma samples is treated as nothing to do rather than
// a partial conversion -- a half-expanded frame is worse than an unexpanded
// one, because it is not uniformly wrong.
inline void i420_expand_limited_to_full(const uint8_t *src_y,
                                        const uint8_t *src_u,
                                        const uint8_t *src_v,
                                        uint8_t *dst_y,
                                        uint8_t *dst_u,
                                        uint8_t *dst_v,
                                        size_t y_len)
{
    if (!src_y || !src_u || !src_v || !dst_y || !dst_u || !dst_v) return;
    if (y_len == 0 || (y_len % 4) != 0) return;

    const auto &t = cv_i420_detail::tables();
    for (size_t i = 0; i < y_len; ++i)
        dst_y[i] = t.luma[src_y[i]];

    const size_t c_len = y_len / 4;
    for (size_t i = 0; i < c_len; ++i) {
        dst_u[i] = t.chroma[src_u[i]];
        dst_v[i] = t.chroma[src_v[i]];
    }
}
