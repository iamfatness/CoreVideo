#include "zoom-tile-grid.h"

#include <algorithm>
#include <cmath>

namespace {

// Nearest even value at or below `v`. Used for sizes, so a snapped row can
// never be wider than the row the solver laid out.
uint32_t even_floor(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(v) & ~1u;
}

// Nearest even value to `v`. Used for origins, where staying closest to the
// solver's centred position matters more than rounding direction.
uint32_t even_round(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(std::lround(v / 2.0)) * 2u;
}

// Places a run of `n` equal cells of `size` separated by `gutter`, starting as
// near `desired_start` as possible while keeping the whole run inside `extent`.
// Shifting the entire run (rather than clipping one cell) is what preserves
// uniform spacing when the snapped run would otherwise overhang.
uint32_t run_origin(double desired_start, std::size_t n, uint32_t size,
                    uint32_t gutter, uint32_t extent)
{
    const uint32_t run = size * static_cast<uint32_t>(n) +
        gutter * static_cast<uint32_t>(n - 1);
    if (run >= extent) return 0;
    uint32_t start = even_round(desired_start);
    if (start + run > extent) start = even_floor(extent - run);
    return start;
}

}  // namespace

std::vector<SnappedTileRect> snap_tile_grid_even(const std::vector<TileRect> &rects,
                                                 const TileGridParams &params)
{
    std::vector<SnappedTileRect> snapped;
    if (rects.empty()) return snapped;

    const uint32_t tile_w = even_floor(rects[0].width);
    const uint32_t tile_h = even_floor(rects[0].height);
    if (tile_w < 2 || tile_h < 2) return snapped;

    const uint32_t canvas_w = even_floor(params.canvas_width);
    const uint32_t canvas_h = even_floor(params.canvas_height);
    if (canvas_w < tile_w || canvas_h < tile_h) return snapped;

    const uint32_t gutter = even_floor(params.gutter);

    // The solver emits row-major with every tile in a row sharing a y, so a
    // change in y starts a new row.
    std::vector<std::size_t> row_starts;
    for (std::size_t i = 0; i < rects.size(); ++i)
        if (i == 0 || rects[i].y != rects[i - 1].y) row_starts.push_back(i);

    const std::size_t rows = row_starts.size();
    const uint32_t grid_y = run_origin(rects[0].y, rows, tile_h, gutter, canvas_h);

    snapped.reserve(rects.size());
    for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t first = row_starts[r];
        const std::size_t last =
            (r + 1 < rows) ? row_starts[r + 1] : rects.size();
        const std::size_t in_row = last - first;

        const uint32_t row_x =
            run_origin(rects[first].x, in_row, tile_w, gutter, canvas_w);
        const uint32_t row_y = grid_y + static_cast<uint32_t>(r) * (tile_h + gutter);

        for (std::size_t c = 0; c < in_row; ++c) {
            SnappedTileRect s;
            s.x      = row_x + static_cast<uint32_t>(c) * (tile_w + gutter);
            s.y      = row_y;
            s.width  = tile_w;
            s.height = tile_h;
            snapped.push_back(s);
        }
    }
    return snapped;
}

CropRect solve_cover_crop(double src_width, double src_height, double dst_aspect)
{
    CropRect crop;
    if (src_width <= 0.0 || src_height <= 0.0 || dst_aspect <= 0.0) return crop;

    const double src_aspect = src_width / src_height;
    if (src_aspect > dst_aspect) {
        // Source is wider than the tile: keep full height, crop the sides.
        crop.height = src_height;
        crop.width  = src_height * dst_aspect;
    } else {
        // Source is taller than the tile: keep full width, crop top and bottom.
        crop.width  = src_width;
        crop.height = src_width / dst_aspect;
    }
    crop.x = (src_width  - crop.width)  / 2.0;
    crop.y = (src_height - crop.height) / 2.0;
    return crop;
}

std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params)
{
    std::vector<TileRect> rects;
    if (count == 0) return rects;
    if (params.tile_aspect <= 0.0) return rects;

    const double usable_w = params.canvas_width  - 2.0 * params.margin;
    const double usable_h = params.canvas_height - 2.0 * params.margin;
    if (usable_w <= 0.0 || usable_h <= 0.0) return rects;

    std::size_t best_rows = 0;
    std::size_t best_cols = 0;
    double      best_tile_w = 0.0;

    for (std::size_t rows = 1; rows <= count; ++rows) {
        const std::size_t cols = (count + rows - 1) / rows;

        const double avail_w = usable_w - params.gutter * static_cast<double>(cols - 1);
        const double avail_h = usable_h - params.gutter * static_cast<double>(rows - 1);
        if (avail_w <= 0.0 || avail_h <= 0.0) continue;

        // Largest tile of the required aspect that fits both dimensions.
        const double tile_w = std::min(avail_w / static_cast<double>(cols),
                                       (avail_h / static_cast<double>(rows)) * params.tile_aspect);
        if (tile_w <= 0.0) continue;

        if (tile_w > best_tile_w) {
            best_tile_w = tile_w;
            best_rows   = rows;
            best_cols   = cols;
        }
    }

    if (best_tile_w <= 0.0) return rects;

    const double tile_h = best_tile_w / params.tile_aspect;
    const double grid_h = tile_h * static_cast<double>(best_rows) +
                          params.gutter * static_cast<double>(best_rows - 1);
    const double start_y = (params.canvas_height - grid_h) / 2.0;

    rects.reserve(count);
    for (std::size_t row = 0; row < best_rows; ++row) {
        const std::size_t placed = row * best_cols;
        // Defensive: `count - placed` is unsigned, so a row selection that
        // over-committed would underflow to a huge value and std::min would
        // then happily return best_cols, placing phantom tiles. The row
        // heuristic above cannot currently produce that, but the guard costs
        // one line and the failure mode is silent.
        if (placed >= count) break;
        const std::size_t in_row = std::min(best_cols, count - placed);
        if (in_row == 0) break;

        // Center each row independently so a short final row sits centered.
        const double row_w = best_tile_w * static_cast<double>(in_row) +
                             params.gutter * static_cast<double>(in_row - 1);
        const double start_x = (params.canvas_width - row_w) / 2.0;

        for (std::size_t col = 0; col < in_row; ++col) {
            TileRect r;
            r.x      = start_x + static_cast<double>(col) * (best_tile_w + params.gutter);
            r.y      = start_y + static_cast<double>(row) * (tile_h + params.gutter);
            r.width  = best_tile_w;
            r.height = tile_h;
            rects.push_back(r);
        }
    }

    return rects;
}
