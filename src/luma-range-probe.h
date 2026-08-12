// src/luma-range-probe.h
#pragma once

#include <cstddef>
#include <cstdint>

// What the luma plane of a received frame actually contains. Limited-range
// (studio swing) video floors at 16 and ceilings at 235; full-range video uses
// the whole 0..255 scale. Reading this off a live frame is the only way to know
// which one the Zoom SDK is really delivering, whatever colorspace it was asked
// for. See tests/luma-range-probe-test.cpp.
struct LumaRange {
    uint8_t  min       = 255;
    uint8_t  max       = 0;
    uint32_t below_16  = 0;
    uint32_t above_235 = 0;
    uint32_t sampled   = 0;
};

// Samples every `step`-th pixel of every `step`-th row. `stride` is the plane's
// row pitch, which is not always the visible width — sampling the padding would
// read whatever the encoder left there and invent extremes that are not in the
// picture.
inline LumaRange probe_luma_range(const uint8_t *y_plane, uint32_t width,
                                  uint32_t height, uint32_t stride,
                                  uint32_t step)
{
    LumaRange out;
    if (!y_plane || width == 0 || height == 0 || stride < width)
        return out;
    if (step == 0)
        step = 1;

    for (uint32_t y = 0; y < height; y += step) {
        const uint8_t *row = y_plane + static_cast<std::size_t>(y) * stride;
        for (uint32_t x = 0; x < width; x += step) {
            const uint8_t v = row[x];
            if (v < out.min) out.min = v;
            if (v > out.max) out.max = v;
            if (v < 16)  ++out.below_16;
            if (v > 235) ++out.above_235;
            ++out.sampled;
        }
    }
    if (out.sampled == 0)
        out.min = 0;
    return out;
}
