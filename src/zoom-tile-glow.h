// src/zoom-tile-glow.h
#pragma once

#include "zoom-tile-grid.h"  // SnappedTileRect

#include <algorithm>
#include <cmath>
#include <cstdint>

// The pure geometry behind the wall's outer glow: a tile rect plus a glow size
// becomes the quad the glow pass draws, expanded beyond the tile on all four
// sides and clamped to the canvas.
//
// The glow is a SEPARATE PASS DRAWN BEFORE THE TILES, not an extension of the
// tile draw, and this header exists because of that. A tile is drawn as a quad
// exactly its own size, so there is no canvas outside it for a halo to bleed
// into; the halo needs a bigger quad of its own. (The other half of the reason
// is that the tile pixel shader carries the parity-verified BT.709 conversion
// and the crop_uv correction, which are not worth disturbing for an effect that
// can live beside them. See the design doc:
// docs/superpowers/specs/2026-08-11-corevideo-tiles-gallery-styling-design.md.)

// The quad to draw, and where the *inner* tile sits inside it.
//
// center/half_size are not redundant with the quad: once the quad is clipped by
// the canvas the tile is no longer centred in it, and the shader evaluates its
// distance field from the quad's own 0..1 coordinates. Carrying the tile's
// position through explicitly is what keeps a clipped halo registered against
// the tile it belongs to — see the clamping cases in tests/tile-glow-test.cpp.
struct GlowQuad {
    // false means "draw nothing": no glow size, a degenerate tile or canvas, or
    // a quad that clamped away to nothing. The pass is skipped rather than
    // issuing a zero-sized sprite.
    bool visible = false;

    // The quad, in whole canvas pixels, inside the canvas.
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;

    // The tile's centre, in pixels from the quad's origin.
    double center_x = 0.0;
    double center_y = 0.0;
    // Half the tile's size — the tile's, never the quad's. This is the
    // half_size the shader passes to rounded_rect_sd(), so distance 0 is the
    // tile's own edge and the falloff starts there.
    double half_width  = 0.0;
    double half_height = 0.0;
};

// Expands `tile` by `glow_size` on all four sides and clamps the result to the
// canvas.
//
// Deliberately NOT done here: any clamp on `glow_size` itself. A glow wider
// than half the gutter merges neighbouring halos into a continuous wash, and
// one wider than the margin clips at the canvas edge. Both are legitimate small
// and obviously wrong large, and clamping would silently contradict the number
// the operator typed. The clipping below is a consequence of the canvas being
// finite, not a bound on the setting.
inline GlowQuad solve_glow_quad(const SnappedTileRect &tile, double glow_size,
                                uint32_t canvas_width, uint32_t canvas_height)
{
    GlowQuad q;

    // Written as !(x > 0.0) rather than (x <= 0.0) so NaN is caught too — a
    // hand-edited scene file can carry one and obs_data_get_* will hand it over
    // without comment. Zero is the default and the no-regression guarantee:
    // nothing is drawn, so the output is byte-identical to a wall with no glow.
    if (!(glow_size > 0.0)) return q;
    if (tile.width == 0 || tile.height == 0) return q;
    if (canvas_width == 0 || canvas_height == 0) return q;

    const double tile_left   = static_cast<double>(tile.x);
    const double tile_top    = static_cast<double>(tile.y);
    const double tile_right  = tile_left + static_cast<double>(tile.width);
    const double tile_bottom = tile_top + static_cast<double>(tile.height);

    // Expand, rounding OUTWARD. The quad is drawn in whole pixels, so rounding
    // inward would clip the last row of the falloff and leave a faint hard edge
    // exactly where the halo should have faded to nothing.
    //
    // std::floor/std::ceil of an enormous or infinite glow size stay enormous
    // or infinite, and the clamp below turns them into the canvas — no cast of
    // an unrepresentable value ever reaches the uint32_t members.
    double left   = std::floor(tile_left - glow_size);
    double top    = std::floor(tile_top - glow_size);
    double right  = std::ceil(tile_right + glow_size);
    double bottom = std::ceil(tile_bottom + glow_size);

    left   = std::max(left, 0.0);
    top    = std::max(top, 0.0);
    right  = std::min(right, static_cast<double>(canvas_width));
    bottom = std::min(bottom, static_cast<double>(canvas_height));

    // A tile entirely off the canvas clamps away to nothing. Only reachable
    // from a hand-built rect — snap_tile_grid_even() keeps its rects inside the
    // canvas — but the draw path must never be handed a zero-sized sprite.
    if (!(right > left) || !(bottom > top)) return q;

    q.visible = true;
    q.x      = static_cast<uint32_t>(left);
    q.y      = static_cast<uint32_t>(top);
    q.width  = static_cast<uint32_t>(right - left);
    q.height = static_cast<uint32_t>(bottom - top);

    q.half_width  = static_cast<double>(tile.width) * 0.5;
    q.half_height = static_cast<double>(tile.height) * 0.5;
    // Measured from the quad's origin, which is why the clamp above has to have
    // happened first: this is the term that absorbs whatever the canvas clipped.
    q.center_x = (tile_left + q.half_width) - left;
    q.center_y = (tile_top + q.half_height) - top;
    return q;
}
