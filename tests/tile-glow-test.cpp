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

    // Final review, Minor 7: the halo must follow a MOVING tile's true
    // fractional rect, not the 2px-quantised one the direct blit uses. The
    // renderer was solving the quad from `rects[i]` while drawing the tile at
    // `moving[i]`, so while a tile glided its halo stepped on the 2px grid —
    // the exact artefact the sub-pixel path exists to remove from the tile's
    // own trailing edge.
    //
    // The quad stays whole-pixel, because it is drawn as a sprite and rounds
    // outward to avoid clipping the falloff. What has to move continuously is
    // the centre and half-size the shader measures distance from.
    {
        constexpr double g = 24.0;
        double last_center = -1.0;
        int distinct = 0;
        for (int step = 0; step <= 8; ++step) {
            const double x = 500.0 + step * 0.25;     // sub-2px motion
            const GlowQuad q = solve_glow_quad(x, 300.0, 628.0, 354.0, g, 1920, 1080);
            if (!q.visible) {
                std::cerr << "a moving tile's glow quad was not visible\n";
                return 1;
            }
            // Absolute centre on the canvas, reconstructed from the quad.
            const double abs_center = static_cast<double>(q.x) + q.center_x;
            if (!near(abs_center, x + 314.0)) {
                std::cerr << "the glow centre did not follow the tile's fractional "
                             "position: wanted " << (x + 314.0) << " got "
                          << abs_center << "\n";
                return 1;
            }
            if (last_center < 0.0 || !near(abs_center, last_center)) ++distinct;
            last_center = abs_center;
        }
        // Nine quarter-pixel steps must give nine distinct centres. Quantised
        // to the 2px grid they would collapse to two.
        if (distinct != 9) {
            std::cerr << "the glow centre is quantised, not continuous: "
                      << distinct << " distinct positions across 9 quarter-pixel "
                         "steps\n";
            return 1;
        }

        // A fractional SIZE is carried through too — even_floor_px() would
        // have lost up to 1.95px of it.
        const GlowQuad qf = solve_glow_quad(100.0, 100.0, 628.75, 354.5, g, 1920, 1080);
        if (!near(qf.half_width, 628.75 * 0.5) ||
            !near(qf.half_height, 354.5 * 0.5)) {
            std::cerr << "the glow quad rounded the tile's fractional size away\n";
            return 1;
        }

        // The whole-pixel overload must agree with the fractional one, or the
        // settled and moving paths would draw different halos.
        SnappedTileRect s;
        s.x = 500; s.y = 300; s.width = 628; s.height = 354;
        const GlowQuad a = solve_glow_quad(s, g, 1920, 1080);
        const GlowQuad b = solve_glow_quad(500.0, 300.0, 628.0, 354.0, g, 1920, 1080);
        if (a.visible != b.visible || a.x != b.x || a.y != b.y ||
            a.width != b.width || a.height != b.height ||
            !near(a.center_x, b.center_x) || !near(a.center_y, b.center_y) ||
            !near(a.half_width, b.half_width) || !near(a.half_height, b.half_height)) {
            std::cerr << "the whole-pixel and fractional overloads disagree\n";
            return 1;
        }

        // A degenerate fractional size draws nothing, as the integer one does.
        if (solve_glow_quad(10.0, 10.0, 0.0, 100.0, g, 1920, 1080).visible ||
            solve_glow_quad(10.0, 10.0, 100.0, 0.0, g, 1920, 1080).visible) {
            std::cerr << "a zero-sized fractional tile still produced a halo\n";
            return 1;
        }
    }

    // Re-review N2: on the composite-failure fallback the halo must follow the
    // rect the tile ACTUALLY drew at, not the one it was going to draw at.
    //
    // The glow pass runs before the tile pass, so it has to be told whether
    // the composite will succeed. When it does not — no default effect, no
    // render target, or one that could not be created at this size — the tile
    // falls back to the snapped blit, and a halo solved from the fractional
    // rect sits up to 1px out in position and ~1.95px in size against it.
    {
        constexpr double g = 24.0;
        SnappedTileRect snapped;
        snapped.x = 500; snapped.y = 300; snapped.width = 628; snapped.height = 354;
        // The fractional rect the tile would have used, deliberately well off
        // the snapped one so a mix-up cannot hide inside rounding.
        const double fx = 501.75, fy = 301.25, fw = 629.5, fh = 355.5;

        const GlowQuad fell_back =
            solve_glow_quad_for_tile(snapped, false, fx, fy, fw, fh, g, 1920, 1080);
        const GlowQuad want_snapped = solve_glow_quad(snapped, g, 1920, 1080);
        if (fell_back.x != want_snapped.x || fell_back.y != want_snapped.y ||
            fell_back.width != want_snapped.width ||
            fell_back.height != want_snapped.height ||
            !near(fell_back.center_x, want_snapped.center_x) ||
            !near(fell_back.center_y, want_snapped.center_y) ||
            !near(fell_back.half_width, want_snapped.half_width) ||
            !near(fell_back.half_height, want_snapped.half_height)) {
            std::cerr << "the halo followed the fractional rect on a tile that "
                         "fell back to the snapped blit\n";
            return 1;
        }

        const GlowQuad composited =
            solve_glow_quad_for_tile(snapped, true, fx, fy, fw, fh, g, 1920, 1080);
        const GlowQuad want_fractional =
            solve_glow_quad(fx, fy, fw, fh, g, 1920, 1080);
        if (!near(composited.center_x, want_fractional.center_x) ||
            !near(composited.half_width, want_fractional.half_width)) {
            std::cerr << "the halo did not follow the fractional rect on a tile "
                         "that was composited\n";
            return 1;
        }
        // And the two answers must genuinely differ, or this proves nothing.
        if (near(composited.half_width, fell_back.half_width) &&
            near(composited.center_x, fell_back.center_x)) {
            std::cerr << "test setup: the two rects produce the same halo\n";
            return 1;
        }
    }

    std::cout << "tile-glow: all tests passed\n";
    return 0;
}
