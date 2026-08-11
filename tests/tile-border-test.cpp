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

    // A degenerate tile must not produce a negative clamp.
    p = clamp_border(6.0, 6.0, 0.0, 0.0);
    if (!check("degenerate tile width", p.width, 0.0)) return 1;
    if (!check("degenerate tile radius", p.radius, 0.0)) return 1;

    std::cout << "tile-border: all tests passed\n";
    return 0;
}
