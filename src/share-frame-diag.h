// src/share-frame-diag.h
#pragma once

#include <cstddef>
#include <cstdint>

// Diagnostics for the screen-share "black flash" (field reports 2026-11-14,
// one Mac and one Windows operator, v0.1.49: a CoreVideo Screen Share source
// blinks to black). Nothing on either side of the pipe writes black on
// purpose, so the question this answers is WHERE a dark frame first appears:
// in what the Zoom SDK hands the engine, or only in what the plugin hands OBS.
// Both sides classify with this one function so their verdicts are comparable.
//
// Pure, no OBS/Qt/SDK, so the engine and the plugin share it and a host test
// pins the classifier. See tests/share-frame-diag-test.cpp.

struct ShareLumaStats {
    uint8_t  max     = 0;
    uint32_t mean    = 0;  // rounded mean of the sampled luma
    uint32_t sampled = 0;
};

// Samples every `step`-th pixel of every `step`-th row of a packed or strided
// luma plane.
inline ShareLumaStats share_luma_stats(const uint8_t *y_plane, uint32_t width,
                                       uint32_t height, uint32_t stride,
                                       uint32_t step)
{
    ShareLumaStats out;
    if (!y_plane || width == 0 || height == 0 || stride < width)
        return out;
    if (step == 0)
        step = 1;
    uint64_t sum = 0;
    for (uint32_t y = 0; y < height; y += step) {
        const uint8_t *row = y_plane + static_cast<std::size_t>(y) * stride;
        for (uint32_t x = 0; x < width; x += step) {
            const uint8_t v = row[x];
            if (v > out.max) out.max = v;
            sum += v;
            ++out.sampled;
        }
    }
    if (out.sampled != 0)
        out.mean = static_cast<uint32_t>((sum + out.sampled / 2) / out.sampled);
    return out;
}

// "Dark" means effectively black everywhere: nothing on the sampled grid is
// brighter than a deep shadow. A real desktop, slide or window always carries
// some text, chrome or highlight well above this, so a dark verdict on a share
// frame is the flash, not content. Thresholds sit above limited-range black
// (Y=16) so a limited-range black frame counts too.
inline constexpr uint8_t  kShareDarkMaxLuma  = 40;
inline constexpr uint32_t kShareDarkMeanLuma = 24;

inline bool share_frame_is_dark(const ShareLumaStats &s)
{
    return s.sampled != 0 && s.max <= kShareDarkMaxLuma &&
           s.mean <= kShareDarkMeanLuma;
}

// Log every dark frame in an episode up to a cap, then every Nth, so a
// sustained black share cannot flood the pipe at 30 fps but an isolated
// one-frame flash is always reported.
inline constexpr uint32_t kShareDarkLogFirst = 20;
inline constexpr uint32_t kShareDarkLogEvery = 100;

inline bool share_dark_should_log(uint32_t dark_count)
{
    return dark_count != 0 &&
           (dark_count <= kShareDarkLogFirst ||
            dark_count % kShareDarkLogEvery == 0);
}
