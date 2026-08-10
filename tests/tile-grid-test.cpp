#include "zoom-tile-grid.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

static TileGridParams params_1080p()
{
    TileGridParams p;
    p.canvas_width  = 1920.0;
    p.canvas_height = 1080.0;
    p.tile_aspect   = 16.0 / 9.0;
    p.gutter        = 8.0;
    p.margin        = 8.0;
    return p;
}

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

static bool test_empty()
{
    if (!solve_tile_grid(0, params_1080p()).empty()) {
        std::cerr << "count 0 should produce no rects\n";
        return false;
    }
    return true;
}

static bool test_single_tile_is_centered()
{
    const TileGridParams p = params_1080p();
    const std::vector<TileRect> rects = solve_tile_grid(1, p);
    if (rects.size() != 1) return false;

    const TileRect &r = rects[0];
    if (!near(r.width / r.height, p.tile_aspect)) {
        std::cerr << "single tile aspect wrong: " << r.width / r.height << "\n";
        return false;
    }
    if (!near(r.x + r.width / 2.0, p.canvas_width / 2.0) ||
        !near(r.y + r.height / 2.0, p.canvas_height / 2.0)) {
        std::cerr << "single tile not centered\n";
        return false;
    }
    if (r.x < p.margin - 0.001 || r.y < p.margin - 0.001) {
        std::cerr << "single tile violates margin\n";
        return false;
    }
    return true;
}

static bool test_four_tiles_form_2x2()
{
    const std::vector<TileRect> rects = solve_tile_grid(4, params_1080p());
    if (rects.size() != 4) return false;

    // Two distinct rows, two distinct columns.
    if (!near(rects[0].y, rects[1].y) || !near(rects[2].y, rects[3].y)) {
        std::cerr << "4-up rows not aligned\n";
        return false;
    }
    if (near(rects[0].y, rects[2].y)) {
        std::cerr << "4-up collapsed to a single row\n";
        return false;
    }
    if (!near(rects[0].x, rects[2].x) || !near(rects[1].x, rects[3].x)) {
        std::cerr << "4-up columns not aligned\n";
        return false;
    }
    return true;
}

static bool test_five_tiles_center_short_row()
{
    const TileGridParams p = params_1080p();
    const std::vector<TileRect> rects = solve_tile_grid(5, p);
    if (rects.size() != 5) return false;

    // Expect 3 over 2 on a 16:9 canvas.
    if (!near(rects[0].y, rects[2].y) || near(rects[0].y, rects[3].y)) {
        std::cerr << "5-up did not split 3 over 2\n";
        return false;
    }

    // The short row's midpoint must sit on the canvas centre line.
    const double row_left  = rects[3].x;
    const double row_right = rects[4].x + rects[4].width;
    if (!near((row_left + row_right) / 2.0, p.canvas_width / 2.0)) {
        std::cerr << "short row not centered\n";
        return false;
    }

    // The two rows must be separated by exactly one gutter.
    const double v_gap = rects[3].y - (rects[0].y + rects[0].height);
    if (!near(v_gap, p.gutter)) {
        std::cerr << "5-up vertical gap " << v_gap << " != gutter\n";
        return false;
    }
    return true;
}

static bool test_all_tiles_identical_and_evenly_spaced()
{
    const TileGridParams p = params_1080p();
    for (std::size_t count = 1; count <= 16; ++count) {
        const std::vector<TileRect> rects = solve_tile_grid(count, p);
        if (rects.size() != count) {
            std::cerr << "count " << count << ": wrong rect count\n";
            return false;
        }

        for (const TileRect &r : rects) {
            // Same size.
            if (!near(r.width, rects[0].width) || !near(r.height, rects[0].height)) {
                std::cerr << "count " << count << ": tiles differ in size\n";
                return false;
            }
            // Correct aspect.
            if (!near(r.width / r.height, p.tile_aspect)) {
                std::cerr << "count " << count << ": tile aspect wrong\n";
                return false;
            }
            // Inside the canvas, respecting margins.
            if (r.x < p.margin - 0.001 || r.y < p.margin - 0.001 ||
                r.x + r.width  > p.canvas_width  - p.margin + 0.001 ||
                r.y + r.height > p.canvas_height - p.margin + 0.001) {
                std::cerr << "count " << count << ": tile outside canvas margins\n";
                return false;
            }
        }

        // Equally spaced: every horizontal neighbour gap equals the gutter.
        for (std::size_t i = 1; i < rects.size(); ++i) {
            if (!near(rects[i].y, rects[i - 1].y)) continue;  // row break
            const double gap = rects[i].x - (rects[i - 1].x + rects[i - 1].width);
            if (!near(gap, p.gutter)) {
                std::cerr << "count " << count << ": horizontal gap " << gap
                          << " != gutter " << p.gutter << "\n";
                return false;
            }
        }

        // Equally spaced vertically: every adjacent row gap equals the gutter.
        // Group tiles into rows by y; all tiles in a row share the same y.
        std::vector<double> row_ys;
        for (const TileRect &r : rects) {
            bool known = false;
            for (double y : row_ys)
                if (near(y, r.y)) { known = true; break; }
            if (!known) row_ys.push_back(r.y);
        }
        std::sort(row_ys.begin(), row_ys.end());
        for (std::size_t k = 1; k < row_ys.size(); ++k) {
            const double gap = row_ys[k] - (row_ys[k - 1] + rects[0].height);
            if (!near(gap, p.gutter)) {
                std::cerr << "count " << count << ": vertical row gap " << gap
                          << " != gutter " << p.gutter << "\n";
                return false;
            }
        }
    }
    return true;
}

static bool test_portrait_canvas_stacks_vertically()
{
    TileGridParams p = params_1080p();
    p.canvas_width  = 1080.0;
    p.canvas_height = 1920.0;

    const std::vector<TileRect> rects = solve_tile_grid(2, p);
    if (rects.size() != 2) return false;
    if (near(rects[0].y, rects[1].y)) {
        std::cerr << "portrait canvas should stack 2 tiles vertically\n";
        return false;
    }
    return true;
}

static bool test_cover_crop()
{
    const double tile_aspect = 16.0 / 9.0;

    // Matching aspect: no crop at all.
    CropRect c = solve_cover_crop(1920.0, 1080.0, tile_aspect);
    if (!near(c.x, 0.0) || !near(c.y, 0.0) ||
        !near(c.width, 1920.0) || !near(c.height, 1080.0)) {
        std::cerr << "16:9 source should not be cropped\n";
        return false;
    }

    // Portrait source: full width kept, top and bottom cropped evenly.
    c = solve_cover_crop(1080.0, 1920.0, tile_aspect);
    if (!near(c.width, 1080.0)) {
        std::cerr << "portrait source should keep full width\n";
        return false;
    }
    if (!near(c.height, 1080.0 / tile_aspect)) {
        std::cerr << "portrait crop height wrong\n";
        return false;
    }
    if (!near(c.y, (1920.0 - c.height) / 2.0) || !near(c.x, 0.0)) {
        std::cerr << "portrait crop not centered\n";
        return false;
    }

    // Ultra-wide source: full height kept, sides cropped evenly.
    c = solve_cover_crop(3840.0, 1080.0, tile_aspect);
    if (!near(c.height, 1080.0) || !near(c.width, 1080.0 * tile_aspect)) {
        std::cerr << "ultra-wide crop wrong\n";
        return false;
    }
    if (!near(c.x, (3840.0 - c.width) / 2.0)) {
        std::cerr << "ultra-wide crop not centered\n";
        return false;
    }

    // The result must always match the tile aspect and stay inside the source.
    if (!near(c.width / c.height, tile_aspect)) {
        std::cerr << "crop aspect does not match tile\n";
        return false;
    }
    return true;
}

// Groups snapped tiles into rows by shared y, preserving left-to-right order.
static std::vector<std::vector<SnappedTileRect>> rows_of(
    const std::vector<SnappedTileRect> &rects)
{
    std::vector<std::vector<SnappedTileRect>> rows;
    for (const SnappedTileRect &r : rects) {
        bool placed = false;
        for (auto &row : rows) {
            if (row[0].y == r.y) { row.push_back(r); placed = true; break; }
        }
        if (!placed) rows.push_back({r});
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return a[0].y < b[0].y; });
    return rows;
}

// The DoD requires identical, evenly spaced tiles. This pins that as a property
// of the snapped output — the form actually blitted — rather than of the
// floating-point solve, across every tile count and both target canvases.
static bool test_snapped_grid_is_uniform()
{
    struct Canvas { double w, h; const char *label; };
    const Canvas canvases[] = {
        {1920.0, 1080.0, "1080p"},
        {3840.0, 2160.0, "4K"},
    };

    for (const Canvas &canvas : canvases) {
        TileGridParams p;
        p.canvas_width  = canvas.w;
        p.canvas_height = canvas.h;
        p.tile_aspect   = 16.0 / 9.0;
        p.gutter        = canvas.h / 135.0;  // the shipping spacing rule
        p.margin        = p.gutter;

        for (std::size_t count = 1; count <= 16; ++count) {
            const std::vector<SnappedTileRect> rects =
                snap_tile_grid_even(solve_tile_grid(count, p), p);
            if (rects.size() != count) {
                std::cerr << canvas.label << " count " << count
                          << ": wrong snapped rect count\n";
                return false;
            }

            for (const SnappedTileRect &r : rects) {
                // Every edge even: I420 chroma has no half-pixel.
                if ((r.x | r.y | r.width | r.height) & 1u) {
                    std::cerr << canvas.label << " count " << count
                              << ": odd edge in snapped rect\n";
                    return false;
                }
                if (r.width != rects[0].width || r.height != rects[0].height) {
                    std::cerr << canvas.label << " count " << count
                              << ": snapped tiles differ in size\n";
                    return false;
                }
                if (r.width < 2 || r.height < 2) {
                    std::cerr << canvas.label << " count " << count
                              << ": degenerate snapped tile\n";
                    return false;
                }
                // Must stay inside the canvas: the blit writes these directly.
                if (r.x + r.width > static_cast<uint32_t>(canvas.w) ||
                    r.y + r.height > static_cast<uint32_t>(canvas.h)) {
                    std::cerr << canvas.label << " count " << count
                              << ": snapped tile outside canvas\n";
                    return false;
                }
            }

            // Every horizontal gap and every vertical gap must be exactly the
            // same integer — not merely close.
            const auto rows = rows_of(rects);
            bool have_gap = false;
            uint32_t gap = 0;
            for (const auto &row : rows) {
                for (std::size_t i = 1; i < row.size(); ++i) {
                    const uint32_t left = row[i - 1].x + row[i - 1].width;
                    if (row[i].x < left) {
                        std::cerr << canvas.label << " count " << count
                                  << ": snapped tiles overlap horizontally\n";
                        return false;
                    }
                    const uint32_t g = row[i].x - left;
                    if (have_gap && g != gap) {
                        std::cerr << canvas.label << " count " << count
                                  << ": horizontal gap " << g << " != " << gap
                                  << "\n";
                        return false;
                    }
                    have_gap = true;
                    gap = g;
                }
            }
            for (std::size_t r = 1; r < rows.size(); ++r) {
                const uint32_t top = rows[r - 1][0].y + rows[r - 1][0].height;
                if (rows[r][0].y < top) {
                    std::cerr << canvas.label << " count " << count
                              << ": snapped rows overlap\n";
                    return false;
                }
                const uint32_t g = rows[r][0].y - top;
                if (have_gap && g != gap) {
                    std::cerr << canvas.label << " count " << count
                              << ": vertical gap " << g << " != horizontal gap "
                              << gap << "\n";
                    return false;
                }
                have_gap = true;
                gap = g;
            }
        }
    }
    return true;
}

// Regression guard for the specific defect this replaced: independently
// rounding each edge produced adjacent gaps of 8 and 6 px in one row.
static bool test_snapped_six_up_gaps_are_equal()
{
    TileGridParams p = params_1080p();
    const std::vector<SnappedTileRect> rects =
        snap_tile_grid_even(solve_tile_grid(6, p), p);
    if (rects.size() != 6) return false;

    const auto rows = rows_of(rects);
    for (const auto &row : rows) {
        for (std::size_t i = 1; i < row.size(); ++i) {
            const uint32_t g = row[i].x - (row[i - 1].x + row[i - 1].width);
            if (g != 8) {
                std::cerr << "6-up snapped horizontal gap " << g << " != 8\n";
                return false;
            }
        }
    }
    return true;
}

static bool test_snapped_empty_grid()
{
    if (!snap_tile_grid_even({}, params_1080p()).empty()) {
        std::cerr << "empty grid should snap to nothing\n";
        return false;
    }
    return true;
}

int main()
{
    if (!test_empty()) return 1;
    if (!test_single_tile_is_centered()) return 1;
    if (!test_four_tiles_form_2x2()) return 1;
    if (!test_five_tiles_center_short_row()) return 1;
    if (!test_all_tiles_identical_and_evenly_spaced()) return 1;
    if (!test_portrait_canvas_stacks_vertically()) return 1;
    if (!test_cover_crop()) return 1;
    if (!test_snapped_empty_grid()) return 1;
    if (!test_snapped_six_up_gaps_are_equal()) return 1;
    if (!test_snapped_grid_is_uniform()) return 1;

    std::cout << "tile-grid: all tests passed\n";
    return 0;
}
