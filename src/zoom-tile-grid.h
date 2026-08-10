#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A single tile's placement, in canvas pixels.
struct TileRect {
    double x      = 0.0;
    double y      = 0.0;
    double width  = 0.0;
    double height = 0.0;
};

struct TileGridParams {
    double canvas_width  = 1920.0;
    double canvas_height = 1080.0;
    double tile_aspect   = 16.0 / 9.0;  // width / height
    double gutter        = 8.0;         // space between adjacent tiles
    double margin        = 8.0;         // space around the whole wall
};

// The sub-rectangle of a source frame to sample so that it fills a tile of
// `dst_aspect` completely, cropping the overflow evenly on both sides.
// Never letterboxes: the returned rect always has aspect == dst_aspect and
// fits inside the source frame.
// Members default to zero so the invalid-input early return below yields a
// defined (empty) rect rather than uninitialized doubles, matching TileRect.
struct CropRect {
    double x      = 0.0;
    double y      = 0.0;
    double width  = 0.0;
    double height = 0.0;
};

CropRect solve_cover_crop(double src_width, double src_height, double dst_aspect);

// Lays out `count` identical tiles, choosing the row/column arrangement that
// maximizes tile area. Every returned rect has the same width and height and
// the same gap from its neighbours. A short final row is centered.
// Returns an empty vector when count == 0 or the canvas cannot fit the margins.
std::vector<TileRect> solve_tile_grid(std::size_t count, const TileGridParams &params);

// A tile placed on integer pixel boundaries, every edge even.
struct SnappedTileRect {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

// Snaps a solved grid onto even pixel boundaries — required because I420 chroma
// is subsampled 2x2, so a blit edge on an odd pixel has no valid chroma sample.
//
// Rounding each rect's edges independently would destroy the uniform spacing the
// solver produced: the gap between two tiles becomes
// round(x[i+1]) - round(x[i]) - round(w), which varies by up to 2 px from one
// pair to the next. Instead the tile size and the gutter are each snapped once,
// and every tile is then placed at a whole multiple of (size + gutter) from its
// row origin, so every gap is identical by construction.
//
// Returns an empty vector when the grid cannot be represented (no rects, or a
// tile that snaps below 2 px). Every returned rect lies inside the canvas.
std::vector<SnappedTileRect> snap_tile_grid_even(const std::vector<TileRect> &rects,
                                                 const TileGridParams &params);
