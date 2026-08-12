// src/zoom-tile-shape.h
#pragma once

#include <algorithm>

// The two pure resolvers behind the wall's shape and spacing controls: a tile
// shape preset becomes an aspect ratio, and a spacing percentage becomes
// pixels for a given canvas. Both were constants compiled into the draw path
// before they were operator controls, so both defaults are pinned to those
// constants exactly — see tests/tile-shape-test.cpp.

// Tile shape, stored as an int in the scene file so the dropdown's order can
// change without invalidating saved scenes. Custom is numbered well clear of
// the presets so more shapes can be added without renumbering it.
enum class TileAspectPreset : long long {
    Wide16x9    = 0,
    Standard4x3 = 1,
    Photo5x4    = 2,
    Square1x1   = 3,
    Portrait3x4 = 4,
    Tall9x16    = 5,
    Custom      = 100,
};

// The shape the wall shipped with, and the fallback for anything unusable.
constexpr double kDefaultTileAspect = 16.0 / 9.0;

// The wall's shipped spacing rule: gutter = margin = canvas_height / 135, i.e.
// 8 px at 1080p. Kept as the divisor it has always been, with the percentage
// derived from it rather than the other way round.
constexpr double kSpacingDivisor   = 135.0;
constexpr double kDefaultSpacingPct = 100.0 / kSpacingDivisor;  // ~0.741%

// Preset id (as stored in the scene file) plus the Custom control's ratio ->
// the tile aspect, width / height.
//
// The custom ratio is consulted only when Custom is selected, so switching to
// a preset uses the preset even if a ratio is still sitting in the settings.
// A ratio of zero or less would divide by zero in the grid solve and in
// solve_cover_crop; NaN would survive every comparison in both and end up cast
// to uint32_t by the snap pass. Neither is worth propagating, so both fall
// back to the shipped shape.
inline double resolve_tile_aspect(long long preset, double custom_ratio)
{
    switch (static_cast<TileAspectPreset>(preset)) {
    case TileAspectPreset::Wide16x9:    return 16.0 / 9.0;
    case TileAspectPreset::Standard4x3: return 4.0 / 3.0;
    case TileAspectPreset::Photo5x4:    return 5.0 / 4.0;
    case TileAspectPreset::Square1x1:   return 1.0;
    case TileAspectPreset::Portrait3x4: return 3.0 / 4.0;
    case TileAspectPreset::Tall9x16:    return 9.0 / 16.0;
    case TileAspectPreset::Custom:
        // Written as !(x > 0.0) rather than (x <= 0.0) so NaN is caught too.
        if (!(custom_ratio > 0.0)) return kDefaultTileAspect;
        return custom_ratio;
    }
    // A preset id from a newer scene file degrades to the shape that existed
    // first, the same way an unknown border shape degrades to Square.
    return kDefaultTileAspect;
}

// A spacing percentage of the canvas height -> pixels.
//
// The division by (100 / pct) is deliberate and is NOT a roundabout way of
// writing canvas_height * pct / 100.0. The whole no-regression argument for
// this control is that the default reproduces the wall's hard-coded
// canvas_height / 135.0 bit-for-bit; the multiplicative form does not, because
// 100.0 / 135.0 does not round-trip through a multiply, and it disagrees in
// the last bit at hundreds of canvas heights. The divisor form does round-trip
// (100.0 / kDefaultSpacingPct is exactly 135.0), so at the default the
// expression reduces to the original literally. The equivalence is pinned at
// every even canvas height the source accepts in tests/tile-shape-test.cpp —
// if this is ever "simplified", that test is what will notice.
//
// Bounds, both reachable only from a hand-edited scene file since the sliders
// cannot produce them: a negative or NaN percentage floors to zero, and an
// absurd one is capped at the canvas height. The cap is not cosmetic —
// snap_tile_grid_even() casts the gutter to uint32_t, which is undefined for a
// value it cannot represent. A capped gutter simply leaves no room for tiles,
// and the solver already returns an empty layout for that, which renders the
// background alone.
inline double resolve_spacing_px(double pct_of_height, double canvas_height)
{
    if (!(canvas_height > 0.0)) return 0.0;
    if (!(pct_of_height > 0.0)) return 0.0;
    const double px = canvas_height / (100.0 / pct_of_height);
    if (!(px > 0.0)) return 0.0;  // underflowed to zero, or NaN via inf/inf
    return std::min(px, canvas_height);
}
