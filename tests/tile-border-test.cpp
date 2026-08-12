// tests/tile-border-test.cpp
// Border clamping. A border wider than half the tile, or a radius larger than
// half the shorter side, would invert the tile or produce a degenerate shape.
// These are operator-reachable via the properties dialog, so they are clamped
// rather than trusted.

#include "zoom-tile-border.h"

#include <cmath>
#include <iostream>

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

static bool check(const char *name, double got, double want)
{
    if (near(got, want)) return true;
    std::cerr << name << ": expected " << want << ", got " << got << "\n";
    return false;
}

int main()
{
    // Ordinary values pass through untouched.
    BorderParams p = clamp_border(6.0, 12.0, 620.0, 348.0);
    if (!check("width passthrough", p.width, 6.0)) return 1;
    if (!check("radius passthrough", p.radius, 12.0)) return 1;

    // Width is capped at half the SHORTER side, so the tile cannot invert.
    p = clamp_border(400.0, 0.0, 620.0, 348.0);
    if (!check("width clamped to half the short side", p.width, 174.0)) return 1;

    // Radius is capped at half the shorter side too: a larger value has no
    // additional meaning, it is just a capsule.
    p = clamp_border(0.0, 900.0, 620.0, 348.0);
    if (!check("radius clamped", p.radius, 174.0)) return 1;

    // Negative values are meaningless; treat as zero rather than inverting.
    p = clamp_border(-5.0, -5.0, 620.0, 348.0);
    if (!check("negative width floors at 0", p.width, 0.0)) return 1;
    if (!check("negative radius floors at 0", p.radius, 0.0)) return 1;

    // A degenerate tile must not produce a negative clamp. Note: this case does
    // not by itself discriminate whether the `limit <= 0.0` guard exists, since
    // min(max(6,0), 0) is 0 either way. Kept because it documents intent.
    p = clamp_border(6.0, 6.0, 0.0, 0.0);
    if (!check("degenerate tile width", p.width, 0.0)) return 1;
    if (!check("degenerate tile radius", p.radius, 0.0)) return 1;

    // Negative tile dimensions are what the guard actually exists for: without
    // it, limit goes negative (min(-10,-10)/2 == -5) and
    // min(max(width,0), limit) collapses to that negative limit instead of 0.
    p = clamp_border(6.0, 6.0, -10.0, -10.0);
    if (!check("negative tile width floors at 0", p.width, 0.0)) return 1;
    if (!check("negative tile radius floors at 0", p.radius, 0.0)) return 1;

    std::cout << "tile-border: all tests passed\n";
    return 0;
}
