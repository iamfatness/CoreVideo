#pragma once

// Aspect-fit geometry adapted from CoreVideo Pro IsoFrameConform.h.
// ISO output is always 1920x1080; chroma-aligned bars preserve aspect ratio.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace corevideo::modules
{

struct IsoFitRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Largest centred rect of the source's aspect that fits the destination, with
// every edge EVEN: I420 and NV12 carry chroma at half resolution, so an odd
// origin or extent splits a chroma sample across the letterbox edge and fringes
// it. A degenerate input returns an empty rect rather than dividing by zero.
[[nodiscard]] inline IsoFitRect isoFitRect(int srcW, int srcH, int dstW, int dstH)
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return {};
    }
    const auto even = [](int value) { return value & ~1; };
    // Compare aspects in integer cross-products: no float, no rounding drift.
    const long long srcAspect = static_cast<long long>(srcW) * dstH;
    const long long dstAspect = static_cast<long long>(dstW) * srcH;
    int width = dstW;
    int height = dstH;
    if (srcAspect > dstAspect) {
        // Source is wider: full width, bars top and bottom.
        height = static_cast<int>((static_cast<long long>(dstW) * srcH + srcW / 2) / srcW);
    } else if (srcAspect < dstAspect) {
        // Source is taller: full height, bars left and right.
        width = static_cast<int>((static_cast<long long>(dstH) * srcW + srcH / 2) / srcH);
    }
    width = std::max(2, std::min(even(width), even(dstW)));
    height = std::max(2, std::min(even(height), even(dstH)));
    return {even((dstW - width) / 2), even((dstH - height) / 2), width, height};
}

} // namespace corevideo::modules
