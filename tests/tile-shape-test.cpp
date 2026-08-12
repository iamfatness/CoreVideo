// tests/tile-shape-test.cpp
// The two pure resolvers behind the operator's tile-shape and spacing controls
// (src/zoom-tile-shape.h), plus the one interaction between them and the draw
// path that cannot be seen from either in isolation.
//
// The load-bearing assertion in here is the spacing EXACTNESS check. The wall
// shipped with a hard-coded gutter and margin of canvas_height / 135.0; the
// whole no-regression argument for turning that into a percentage control is
// that the default percentage reproduces that number bit-for-bit, not merely
// closely. A value that is close but not equal can still snap to a different
// even pixel (snap_tile_grid_even truncates, so 7.999 becomes a 6 px gutter,
// not an 8 px one) and move every tile on the wall.

#include "zoom-tile-crop.h"
#include "zoom-tile-shape.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

static bool near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) < eps;
}

static long long id(TileAspectPreset p) { return static_cast<long long>(p); }

int main()
{
    // ── Presets ──────────────────────────────────────────────────────────────
    // Exact equality, not `near`: the 16:9 entry is the shape every existing
    // scene renders at, and it has to be the identical double the wall was
    // hard-coded to or the grid solve moves.
    struct PresetCase {
        TileAspectPreset preset;
        double           want;
        const char      *name;
    };
    const PresetCase cases[] = {
        {TileAspectPreset::Wide16x9,    16.0 / 9.0, "16:9"},
        {TileAspectPreset::Standard4x3, 4.0 / 3.0,  "4:3"},
        {TileAspectPreset::Photo5x4,    5.0 / 4.0,  "5:4"},
        {TileAspectPreset::Square1x1,   1.0,        "1:1"},
        {TileAspectPreset::Portrait3x4, 3.0 / 4.0,  "3:4"},
        {TileAspectPreset::Tall9x16,    9.0 / 16.0, "9:16"},
    };
    for (const PresetCase &c : cases) {
        // A junk custom ratio is passed deliberately: a preset must ignore the
        // Custom control entirely, so an operator who typed a ratio, switched
        // to a preset and left the ratio behind gets the preset they chose.
        const double got = resolve_tile_aspect(id(c.preset), 3.9);
        if (got != c.want) {
            std::cerr << "preset " << c.name << " should resolve to " << c.want
                      << ", got " << got << "\n";
            return 1;
        }
    }

    // The default preset is the shipped constant, bit-for-bit.
    if (resolve_tile_aspect(id(TileAspectPreset::Wide16x9), 0.0) != kDefaultTileAspect ||
        kDefaultTileAspect != 16.0 / 9.0) {
        std::cerr << "the 16:9 preset is no longer the shipped tile aspect\n";
        return 1;
    }

    // The reference gallery the shapes were added for measures ~1.27:1, which
    // is between 5:4 and 4:3 — so the two must not be the same number.
    if (resolve_tile_aspect(id(TileAspectPreset::Photo5x4), 0.0) ==
        resolve_tile_aspect(id(TileAspectPreset::Standard4x3), 0.0)) {
        std::cerr << "5:4 and 4:3 must be distinct shapes\n";
        return 1;
    }

    // ── Custom ───────────────────────────────────────────────────────────────
    if (resolve_tile_aspect(id(TileAspectPreset::Custom), 1.27) != 1.27) {
        std::cerr << "Custom must return the operator's ratio unchanged\n";
        return 1;
    }
    // Zero, negative and NaN all divide by zero or poison the grid solve, so
    // they fall back to 16:9 rather than reaching it. NaN is reachable: a
    // hand-edited scene file can carry one, and obs_data_get_double will hand
    // it over without comment.
    const double bad_ratios[] = {0.0, -1.0, -16.0 / 9.0,
                                 std::numeric_limits<double>::quiet_NaN()};
    for (double bad : bad_ratios) {
        const double got = resolve_tile_aspect(id(TileAspectPreset::Custom), bad);
        if (got != kDefaultTileAspect) {
            std::cerr << "Custom ratio " << bad << " should fall back to 16:9, got "
                      << got << "\n";
            return 1;
        }
    }

    // A preset id from a newer scene file degrades to the shape that existed
    // first, exactly as the border-shape setting does.
    if (resolve_tile_aspect(4242, 0.0) != kDefaultTileAspect ||
        resolve_tile_aspect(-7, 0.0) != kDefaultTileAspect) {
        std::cerr << "an unknown preset id should fall back to 16:9\n";
        return 1;
    }

    // ── Spacing: the no-regression equivalence ───────────────────────────────
    // 1/135 of the canvas height, as a percentage.
    if (!near(kDefaultSpacingPct, 0.7407407407, 1e-9)) {
        std::cerr << "the default spacing percentage moved: " << kDefaultSpacingPct
                  << "\n";
        return 1;
    }

    // 8 px at 1080p — the number the wall has always used.
    if (resolve_spacing_px(kDefaultSpacingPct, 1080.0) != 8.0) {
        std::cerr << "default spacing at 1080p must be exactly 8 px, got "
                  << resolve_spacing_px(kDefaultSpacingPct, 1080.0) << "\n";
        return 1;
    }

    // And bit-for-bit equal to canvas_height / 135.0 at every canvas height the
    // source will accept. Exact `!=` on purpose: this is the entire argument
    // that an untouched scene renders as it did, so an epsilon here would
    // quietly concede the thing being claimed. A naive
    // `canvas_height * pct / 100.0` fails this at hundreds of heights.
    for (double h = 16.0; h <= 4320.0; h += 2.0) {
        const double got = resolve_spacing_px(kDefaultSpacingPct, h);
        if (got != h / 135.0) {
            std::cerr << "default spacing at canvas height " << h
                      << " is not the shipped canvas_height/135: got " << got
                      << ", want " << (h / 135.0) << "\n";
            return 1;
        }
    }
    // Spelled out at the two heights that matter most, so a failure above is
    // not the only place a reader can see what the rule is.
    if (resolve_spacing_px(kDefaultSpacingPct, 720.0)  != 720.0 / 135.0 ||
        resolve_spacing_px(kDefaultSpacingPct, 2160.0) != 2160.0 / 135.0) {
        std::cerr << "default spacing broke at 720p or 2160p\n";
        return 1;
    }

    // ── Spacing: the control's own behaviour ─────────────────────────────────
    // Linear in the percentage, which is the only thing an operator dragging
    // the slider is entitled to assume.
    if (!near(resolve_spacing_px(2.0 * kDefaultSpacingPct, 1080.0), 16.0, 1e-9)) {
        std::cerr << "doubling the percentage should double the pixels, got "
                  << resolve_spacing_px(2.0 * kDefaultSpacingPct, 1080.0) << "\n";
        return 1;
    }
    if (!near(resolve_spacing_px(10.0, 1080.0), 108.0, 1e-9)) {
        std::cerr << "10% of a 1080 canvas should be 108 px, got "
                  << resolve_spacing_px(10.0, 1080.0) << "\n";
        return 1;
    }

    // Zero is a legitimate setting — tiles flush against each other — and must
    // be exactly zero rather than a sub-pixel sliver.
    if (resolve_spacing_px(0.0, 1080.0) != 0.0) {
        std::cerr << "0% must be 0 px\n";
        return 1;
    }
    // Negative and NaN are only reachable from a hand-edited scene file. A
    // negative gutter would place tiles on top of one another and a NaN one is
    // undefined behaviour the moment snap_tile_grid_even casts it to uint32_t,
    // so both are floored to zero here rather than passed on.
    const double bad_pcts[] = {-1.0, -0.0001,
                               std::numeric_limits<double>::quiet_NaN()};
    for (double bad : bad_pcts) {
        if (resolve_spacing_px(bad, 1080.0) != 0.0) {
            std::cerr << "spacing percentage " << bad << " should floor to 0 px, got "
                      << resolve_spacing_px(bad, 1080.0) << "\n";
            return 1;
        }
    }
    // An absurd percentage is bounded by the canvas rather than handed to the
    // snap pass as something its uint32_t cast cannot represent. The layout
    // then comes back empty, which renders the background alone — a legitimate
    // outcome, unlike undefined behaviour.
    if (resolve_spacing_px(1e9, 1080.0) != 1080.0 ||
        resolve_spacing_px(std::numeric_limits<double>::infinity(), 1080.0) != 1080.0) {
        std::cerr << "an absurd spacing percentage must be bounded by the canvas\n";
        return 1;
    }
    // A degenerate canvas has no spacing to give.
    if (resolve_spacing_px(kDefaultSpacingPct, 0.0) != 0.0 ||
        resolve_spacing_px(kDefaultSpacingPct, -1080.0) != 0.0) {
        std::cerr << "a zero or negative canvas height must yield 0 px\n";
        return 1;
    }

    // ── Shape x crop: the border must stay in register ───────────────────────
    // Changing the tile aspect changes the cover-crop rectangle, which changes
    // the sub-region handed to gs_draw_sprite_subregion() — and crop_uv, which
    // the shader divides by to recover a 0..1 tile coordinate for its distance
    // field, is derived from that same rectangle. This reproduces the draw
    // path's arithmetic for a 4:3 tile fed by a 16:9 camera with a non-zero
    // slot crop (the case where every term is non-trivial) and checks that the
    // shader's `(uv - crop_uv.xy) / crop_uv.zw` still lands on 0 at the tile's
    // left edge and 1 at its right. If it did not, the border would sit off
    // the tile edge on every tile.
    {
        const double aspect =
            resolve_tile_aspect(id(TileAspectPreset::Standard4x3), 0.0);
        const double tex_w = 1920.0, tex_h = 1080.0;
        const CropRect crop = solve_slot_crop(tex_w, tex_h, aspect, 10.0, 0.0);

        if (!near(crop.width / crop.height, aspect, 1e-9)) {
            std::cerr << "a 4:3 tile must sample a 4:3 rect, got "
                      << (crop.width / crop.height) << "\n";
            return 1;
        }
        if (crop.x < 0.0 || crop.y < 0.0 ||
            crop.x + crop.width > tex_w || crop.y + crop.height > tex_h) {
            std::cerr << "the 4:3 crop escaped the source frame\n";
            return 1;
        }

        // The integers actually passed to gs_draw_sprite_subregion(), and the
        // float maths the draw path uses to turn them into crop_uv.
        const uint32_t cx = static_cast<uint32_t>(crop.x);
        const uint32_t cy = static_cast<uint32_t>(crop.y);
        const uint32_t cw = static_cast<uint32_t>(crop.width);
        const uint32_t ch = static_cast<uint32_t>(crop.height);
        if (cw == 0 || ch == 0) {
            std::cerr << "the 4:3 crop truncated to nothing\n";
            return 1;
        }
        const float crop_u  = static_cast<float>(cx) / static_cast<float>(tex_w);
        const float crop_v  = static_cast<float>(cy) / static_cast<float>(tex_h);
        const float crop_cu = static_cast<float>(cw) / static_cast<float>(tex_w);
        const float crop_cv = static_cast<float>(ch) / static_cast<float>(tex_h);

        // build_subsprite_norm() (libobs/graphics/graphics.c) builds the quad's
        // UVs from exactly these integers over the texture size.
        const float u0 = static_cast<float>(cx) / static_cast<float>(tex_w);
        const float u1 = static_cast<float>(cx + cw) / static_cast<float>(tex_w);
        const float v0 = static_cast<float>(cy) / static_cast<float>(tex_h);
        const float v1 = static_cast<float>(cy + ch) / static_cast<float>(tex_h);

        // The shader's undo, in the shader's order.
        const double tile_u0 = (u0 - crop_u) / crop_cu;
        const double tile_u1 = (u1 - crop_u) / crop_cu;
        const double tile_v0 = (v0 - crop_v) / crop_cv;
        const double tile_v1 = (v1 - crop_v) / crop_cv;
        if (!near(tile_u0, 0.0, 1e-5) || !near(tile_u1, 1.0, 1e-5) ||
            !near(tile_v0, 0.0, 1e-5) || !near(tile_v1, 1.0, 1e-5)) {
            std::cerr << "crop_uv is out of register with the drawn sub-region: "
                      << "u " << tile_u0 << ".." << tile_u1 << ", v " << tile_v0
                      << ".." << tile_v1 << " (want 0..1)\n";
            return 1;
        }
    }

    std::cout << "tile-shape: all tests passed\n";
    return 0;
}
