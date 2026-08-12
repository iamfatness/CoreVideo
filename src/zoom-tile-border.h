// src/zoom-tile-border.h
#pragma once

#include <algorithm>

struct BorderParams {
    double width  = 0.0;
    double radius = 0.0;
};

// Bounds the operator's border width and corner radius against the tile they
// are drawn on. Both are reachable from the properties dialog and from a
// hand-edited scene file, and both invert or degenerate the tile if trusted:
// a width past half the shorter side leaves no interior, and a radius past the
// same bound is just a capsule with extra steps.
inline BorderParams clamp_border(double width, double radius,
                                 double tile_w, double tile_h)
{
    BorderParams out;
    const double limit = std::min(tile_w, tile_h) / 2.0;
    if (limit <= 0.0) return out;  // degenerate tile: no border at all
    out.width  = std::min(std::max(width,  0.0), limit);
    out.radius = std::min(std::max(radius, 0.0), limit);
    return out;
}
