// Measures the luma range of a received I420 frame, so the question "does the
// Zoom SDK actually honour the full-range colorspace we ask it for?" is settled
// by measurement instead of by assumption.
//
// The engine requests VideoRawdataColorspace_BT709_F and the plugin declares
// VIDEO_RANGE_FULL to OBS, so the two agree — but a 2026-08-11 measurement of
// program output showed blacks floored around 21 and whites capped near 200,
// which is what limited-range data looks like when nothing expands it. Either
// the SDK ignores the request or that measurement was of something else. This
// probe reads the luma actually delivered: a limited-range frame floors at 16
// and ceilings at 235, a full-range one uses 0..255.

#include "luma-range-probe.h"

#include <iostream>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// A plane whose rows sweep [lo, hi] evenly.
static std::vector<uint8_t> ramp_plane(uint32_t w, uint32_t h,
                                       uint8_t lo, uint8_t hi)
{
    std::vector<uint8_t> plane(static_cast<size_t>(w) * h, lo);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t span = h > 1 ? h - 1 : 1;
        const int value = lo + (hi - lo) * static_cast<int>(y) /
                                   static_cast<int>(span);
        for (uint32_t x = 0; x < w; ++x)
            plane[static_cast<size_t>(y) * w + x] =
                static_cast<uint8_t>(value);
    }
    return plane;
}

int main()
{
    // --- Limited-range (studio swing) frame: 16..235, nothing outside ---
    {
        const uint32_t w = 64, h = 64;
        const auto plane = ramp_plane(w, h, 16, 235);
        const LumaRange r = probe_luma_range(plane.data(), w, h, w, 1);
        check(r.sampled > 0, "probe sampled nothing");
        check(r.min == 16, "limited-range floor should be 16");
        check(r.max == 235, "limited-range ceiling should be 235");
        check(r.below_16 == 0, "limited-range frame must have no luma under 16");
        check(r.above_235 == 0, "limited-range frame must have no luma over 235");
    }

    // --- Full-range frame: uses the whole 0..255 scale ---
    {
        const uint32_t w = 64, h = 64;
        const auto plane = ramp_plane(w, h, 0, 255);
        const LumaRange r = probe_luma_range(plane.data(), w, h, w, 1);
        check(r.min == 0, "full-range floor should be 0");
        check(r.max == 255, "full-range ceiling should be 255");
        check(r.below_16 > 0, "full-range frame should have luma under 16");
        check(r.above_235 > 0, "full-range frame should have luma over 235");
    }

    // --- Stride wider than width: padding must not be sampled ---
    {
        const uint32_t w = 8, h = 4, stride = 16;
        std::vector<uint8_t> plane(static_cast<size_t>(stride) * h, 255);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                plane[static_cast<size_t>(y) * stride + x] = 100;
        const LumaRange r = probe_luma_range(plane.data(), w, h, stride, 1);
        check(r.min == 100 && r.max == 100,
              "probe read past the visible width into stride padding");
    }

    // --- Subsampling still sees the extremes of a ramp ---
    {
        const uint32_t w = 64, h = 64;
        const auto plane = ramp_plane(w, h, 16, 235);
        const LumaRange r = probe_luma_range(plane.data(), w, h, w, 4);
        check(r.sampled > 0, "subsampled probe sampled nothing");
        check(r.min == 16, "subsampled probe missed the floor");
    }

    // --- Degenerate input is not a crash ---
    {
        const LumaRange r = probe_luma_range(nullptr, 0, 0, 0, 1);
        check(r.sampled == 0, "null plane should sample nothing");
    }

    if (failures == 0)
        std::cout << "luma-range-probe tests passed\n";
    return failures == 0 ? 0 : 1;
}
