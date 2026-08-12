// tests/tile-glow-test.cpp
// The pure geometry behind the outer glow (src/zoom-tile-glow.h): a tile rect
// plus a glow size becomes the expanded, canvas-clamped quad the glow pass
// draws, together with where the *inner* tile sits inside that quad.
//
// The load-bearing assertion in here is the CLAMPING. A tile against the canvas
// edge produces an expanded rect that runs off the canvas, and the shader
// evaluates its distance field from the quad's own 0..1 coordinates — so if the
// quad is clamped but the tile's position within it is not recomputed, the halo
// slides off the tile by exactly the amount that was clipped, on every tile in
// the outer row and column. Clamping and re-centring are one operation and the
// tests below pin them together rather than separately.
//
// Deliberately NOT tested, because it is deliberately not implemented: any
// clamp on the glow size itself. A glow wider than half the gutter merges
// neighbouring halos, and one wider than the margin clips at the canvas edge.
// Both are the operator's number rendered honestly. See the design doc,
// docs/superpowers/specs/2026-08-11-corevideo-tiles-gallery-styling-design.md.

#include "zoom-tile-glow.h"
#include "zoom-tile-grid.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

static bool near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

static SnappedTileRect rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    SnappedTileRect r;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

// The quad must contain every part of the tile that is on the canvas, or the
// halo would be drawn on a surface that does not reach the tile edge it is
// supposed to hug.
static bool quad_contains_tile(const GlowQuad &q, const SnappedTileRect &t,
                               uint32_t canvas_w, uint32_t canvas_h)
{
    const double t_left   = static_cast<double>(t.x);
    const double t_top    = static_cast<double>(t.y);
    const double t_right  = std::min<double>(t_left + t.width, canvas_w);
    const double t_bottom = std::min<double>(t_top + t.height, canvas_h);
    return static_cast<double>(q.x) <= t_left &&
           static_cast<double>(q.y) <= t_top &&
           static_cast<double>(q.x) + q.width >= t_right &&
           static_cast<double>(q.y) + q.height >= t_bottom;
}

int main()
{
    // ── The unclamped case: the quad is the tile grown by the glow size ──────
    {
        const SnappedTileRect t = rect(400, 300, 200, 100);
        const GlowQuad q = solve_glow_quad(t, 20.0, 1920, 1080);
        if (!q.visible) {
            std::cerr << "a tile well inside the canvas must produce a quad\n";
            return 1;
        }
        if (q.x != 380 || q.y != 280 || q.width != 240 || q.height != 140) {
            std::cerr << "unclamped quad should be (380,280) 240x140, got ("
                      << q.x << "," << q.y << ") " << q.width << "x" << q.height
                      << "\n";
            return 1;
        }
        // The tile's half-size is the tile's, never the quad's: the distance
        // field is evaluated against the INNER rect.
        if (!near(q.half_width, 100.0) || !near(q.half_height, 50.0)) {
            std::cerr << "half size should be the tile's (100, 50), got ("
                      << q.half_width << ", " << q.half_height << ")\n";
            return 1;
        }
        // Nothing was clipped, so the tile is centred in the quad.
        if (!near(q.center_x, 120.0) || !near(q.center_y, 70.0)) {
            std::cerr << "unclamped centre should be (120, 70), got ("
                      << q.center_x << ", " << q.center_y << ")\n";
            return 1;
        }
    }

    // ── Glow size 0: no quad at all ─────────────────────────────────────────
    // This is the no-regression guarantee. The draw path skips the whole pass
    // on a zero size, and this is the unit that says so, so a future caller
    // that forgets the guard still draws nothing rather than a zero-width
    // sliver of glow colour along every tile edge.
    {
        const GlowQuad q = solve_glow_quad(rect(400, 300, 200, 100), 0.0, 1920, 1080);
        if (q.visible) {
            std::cerr << "glow size 0 must produce no quad\n";
            return 1;
        }
    }
    // Negative and NaN reach here only from a hand-edited scene file, and both
    // would otherwise expand the rect inwards or poison every comparison.
    {
        const double bad[] = {-1.0, -64.0,
                              std::numeric_limits<double>::quiet_NaN()};
        for (double g : bad) {
            if (solve_glow_quad(rect(400, 300, 200, 100), g, 1920, 1080).visible) {
                std::cerr << "glow size " << g << " must produce no quad\n";
                return 1;
            }
        }
    }

    // ── Clamping: a tile hard against the top-left corner ────────────────────
    // The quad cannot start at -12, so it starts at 0 — and the tile is then no
    // longer centred in it. center_x/center_y are what carry that asymmetry to
    // the shader; if they were left at half the quad the halo would sit 4 px
    // right and down of the tile it belongs to.
    {
        const SnappedTileRect t = rect(8, 8, 200, 100);
        const GlowQuad q = solve_glow_quad(t, 20.0, 1920, 1080);
        if (!q.visible) {
            std::cerr << "a tile near the top-left corner must still draw\n";
            return 1;
        }
        if (q.x != 0 || q.y != 0) {
            std::cerr << "quad should clamp to the canvas origin, got (" << q.x
                      << "," << q.y << ")\n";
            return 1;
        }
        // 8 (the margin) + 200 (the tile) + 20 (the glow) = 228 wide.
        if (q.width != 228 || q.height != 128) {
            std::cerr << "clamped quad should be 228x128, got " << q.width << "x"
                      << q.height << "\n";
            return 1;
        }
        // The tile's centre in quad-local pixels: 8 + 100 = 108, 8 + 50 = 58.
        // NOT half the quad (114, 64), which is what an unclamped centre would
        // give — that difference is the whole point of this case.
        if (!near(q.center_x, 108.0) || !near(q.center_y, 58.0)) {
            std::cerr << "clamped centre should be (108, 58), got ("
                      << q.center_x << ", " << q.center_y << ")\n";
            return 1;
        }
        if (near(q.center_x, q.width * 0.5) || near(q.center_y, q.height * 0.5)) {
            std::cerr << "a clipped quad must NOT be centred on its tile\n";
            return 1;
        }
        // The quad-local tile edges, which is what the shader's distance field
        // actually resolves to. The left edge must land on the tile's real
        // offset from the quad origin, i.e. 8 px in.
        if (!near(q.center_x - q.half_width, 8.0) ||
            !near(q.center_y - q.half_height, 8.0)) {
            std::cerr << "the tile's left/top edge is misplaced inside the quad\n";
            return 1;
        }
    }

    // ── Clamping: a tile hard against the bottom-right corner ────────────────
    {
        const SnappedTileRect t = rect(1712, 972, 200, 100);  // 8 px margin
        const GlowQuad q = solve_glow_quad(t, 20.0, 1920, 1080);
        if (!q.visible) {
            std::cerr << "a tile near the bottom-right corner must still draw\n";
            return 1;
        }
        if (q.x != 1692 || q.y != 952) {
            std::cerr << "quad origin should be (1692,952), got (" << q.x << ","
                      << q.y << ")\n";
            return 1;
        }
        // Right edge clamps at 1920, bottom at 1080.
        if (q.x + q.width != 1920 || q.y + q.height != 1080) {
            std::cerr << "quad should clamp to the canvas edge, got right="
                      << (q.x + q.width) << " bottom=" << (q.y + q.height) << "\n";
            return 1;
        }
        if (q.width != 228 || q.height != 128) {
            std::cerr << "clamped quad should be 228x128, got " << q.width << "x"
                      << q.height << "\n";
            return 1;
        }
        // 1712 + 100 - 1692 = 120; 972 + 50 - 952 = 70. Unlike the top-left
        // case the clip is on the far side, so the centre keeps its full glow
        // margin on the near side — which is exactly the asymmetry a single
        // "half the quad" value could not express.
        if (!near(q.center_x, 120.0) || !near(q.center_y, 70.0)) {
            std::cerr << "bottom-right centre should be (120, 70), got ("
                      << q.center_x << ", " << q.center_y << ")\n";
            return 1;
        }
    }

    // ── A glow larger than the whole canvas ──────────────────────────────────
    // Legitimate (and obviously wrong to look at) rather than an error: it
    // clamps to the canvas and the tile keeps its true position inside it.
    {
        const SnappedTileRect t = rect(800, 400, 320, 240);
        const GlowQuad q = solve_glow_quad(t, 100000.0, 1920, 1080);
        if (!q.visible || q.x != 0 || q.y != 0 || q.width != 1920 ||
            q.height != 1080) {
            std::cerr << "an enormous glow should clamp to the whole canvas, got ("
                      << q.x << "," << q.y << ") " << q.width << "x" << q.height
                      << "\n";
            return 1;
        }
        if (!near(q.center_x, 960.0) || !near(q.center_y, 520.0)) {
            std::cerr << "the tile centre must survive a full-canvas clamp, got ("
                      << q.center_x << ", " << q.center_y << ")\n";
            return 1;
        }
    }

    // ── Fractional glow sizes round OUTWARD ──────────────────────────────────
    // The quad is drawn in whole pixels. Rounding inward would clip the last
    // row of the falloff and leave a faint hard edge where the halo should have
    // faded to nothing.
    {
        const GlowQuad q = solve_glow_quad(rect(400, 300, 200, 100), 0.5, 1920, 1080);
        if (!q.visible || q.x != 399 || q.y != 299 || q.width != 202 ||
            q.height != 102) {
            std::cerr << "a 0.5 px glow should still cover a whole pixel: got ("
                      << q.x << "," << q.y << ") " << q.width << "x" << q.height
                      << "\n";
            return 1;
        }
        const GlowQuad f = solve_glow_quad(rect(400, 300, 200, 100), 7.25, 1920, 1080);
        if (!f.visible || f.x != 392 || f.y != 292 || f.width != 216 ||
            f.height != 116) {
            std::cerr << "a 7.25 px glow should expand to 8 px each side, got ("
                      << f.x << "," << f.y << ") " << f.width << "x" << f.height
                      << "\n";
            return 1;
        }
    }

    // ── Degenerate inputs are skipped, not drawn ─────────────────────────────
    {
        if (solve_glow_quad(rect(400, 300, 0, 100), 20.0, 1920, 1080).visible ||
            solve_glow_quad(rect(400, 300, 200, 0), 20.0, 1920, 1080).visible) {
            std::cerr << "a zero-sized tile has no glow to draw\n";
            return 1;
        }
        if (solve_glow_quad(rect(0, 0, 200, 100), 20.0, 0, 1080).visible ||
            solve_glow_quad(rect(0, 0, 200, 100), 20.0, 1920, 0).visible) {
            std::cerr << "a zero-sized canvas has nowhere to draw\n";
            return 1;
        }
        // Entirely off the canvas: the expanded rect clamps to nothing. Only
        // reachable from a hand-built rect, but the draw path must not be
        // handed a zero-width sprite.
        if (solve_glow_quad(rect(4000, 300, 200, 100), 20.0, 1920, 1080).visible) {
            std::cerr << "a tile off the right of the canvas must be skipped\n";
            return 1;
        }
    }

    // ── Invariants over a sweep, including real solved walls ─────────────────
    // Every quad is inside the canvas, contains its tile, and places the tile's
    // edges where the tile actually is. Run over the grids the wall really
    // produces (1 to 9 tiles at 1080p) at glow sizes either side of the 8 px
    // margin, so the clamped and unclamped cases are both exercised on rects
    // nobody hand-picked.
    {
        TileGridParams params;
        params.canvas_width  = 1920.0;
        params.canvas_height = 1080.0;
        params.tile_aspect   = 16.0 / 9.0;
        params.gutter        = 8.0;
        params.margin        = 8.0;
        const double glows[] = {1.0, 4.0, 8.0, 9.0, 40.0, 200.0, 2000.0};
        for (std::size_t count = 1; count <= 9; ++count) {
            const std::vector<SnappedTileRect> rects =
                snap_tile_grid_even(solve_tile_grid(count, params), params);
            for (const SnappedTileRect &t : rects) {
                for (double g : glows) {
                    const GlowQuad q = solve_glow_quad(t, g, 1920, 1080);
                    if (!q.visible) {
                        std::cerr << "a solved tile produced no glow quad at size "
                                  << g << "\n";
                        return 1;
                    }
                    if (q.x + q.width > 1920 || q.y + q.height > 1080) {
                        std::cerr << "glow quad escaped the canvas at size " << g
                                  << ": (" << q.x << "," << q.y << ") " << q.width
                                  << "x" << q.height << "\n";
                        return 1;
                    }
                    if (!quad_contains_tile(q, t, 1920, 1080)) {
                        std::cerr << "glow quad does not contain its tile at size "
                                  << g << "\n";
                        return 1;
                    }
                    // The shader's view: quad-local tile edges must be the real
                    // ones. This is the check that fails the moment clamping
                    // and re-centring come apart.
                    if (!near(q.center_x - q.half_width,
                              static_cast<double>(t.x) - q.x) ||
                        !near(q.center_y - q.half_height,
                              static_cast<double>(t.y) - q.y)) {
                        std::cerr << "the tile is misplaced inside its glow quad at size "
                                  << g << "\n";
                        return 1;
                    }
                    if (!near(q.half_width, t.width * 0.5) ||
                        !near(q.half_height, t.height * 0.5)) {
                        std::cerr << "the glow quad forgot the tile's own size at size "
                                  << g << "\n";
                        return 1;
                    }
                    // Where the falloff is not clipped by the canvas it must fit
                    // inside the quad: the halo reaches zero at half_size + g
                    // from the centre, and drawing less than that would cut it.
                    const double want_right = q.center_x + q.half_width + g;
                    if (q.x + q.width < 1920 && want_right > q.width + 1e-9) {
                        std::cerr << "an unclipped quad is too narrow for its falloff at size "
                                  << g << "\n";
                        return 1;
                    }
                }
            }
        }
    }

    std::cout << "tile-glow: all tests passed\n";
    return 0;
}
