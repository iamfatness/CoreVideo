// tests/tile-texture-test.cpp
// The upload-decision rules for tile textures. Getting these wrong is either a
// per-frame GPU upload of unchanged pixels (a performance bug that is invisible
// until a 9-tile 4K wall) or a frozen tile (a correctness bug that is obvious
// but hard to attribute).

#include "zoom-tile-texture.h"

#include <iostream>

static bool check(const char *name, bool got, bool want)
{
    if (got == want) return true;
    std::cerr << name << ": expected " << want << ", got " << got << "\n";
    return false;
}

int main()
{
    // Realloc: only when the dimensions actually differ.
    if (!check("same size needs no realloc",
               tile_texture_needs_realloc(640, 360, 640, 360), false)) return 1;
    if (!check("width change needs realloc",
               tile_texture_needs_realloc(640, 360, 1280, 360), true)) return 1;
    if (!check("height change needs realloc",
               tile_texture_needs_realloc(640, 360, 640, 720), true)) return 1;
    // A texture that does not exist yet reports 0x0 and must allocate.
    if (!check("unallocated needs realloc",
               tile_texture_needs_realloc(0, 0, 640, 360), true)) return 1;
    // A zero-sized request must not allocate.
    if (!check("zero request needs no realloc",
               tile_texture_needs_realloc(640, 360, 0, 0), false)) return 1;

    // Upload: only when the feed produced a newer frame than we uploaded.
    if (!check("new frame uploads", tile_texture_needs_upload(3, 4), true)) return 1;
    if (!check("same frame skips", tile_texture_needs_upload(4, 4), false)) return 1;
    // Generation 0 means "no frame has ever been stored" — nothing to upload.
    if (!check("no frame yet skips", tile_texture_needs_upload(0, 0), false)) return 1;
    // A feed rebuilt from scratch restarts its generation; a lower incoming
    // generation still means "different from what we uploaded", so upload it
    // rather than showing the previous assignee's last frame forever.
    if (!check("generation went backwards uploads",
               tile_texture_needs_upload(9, 2), true)) return 1;

    std::cout << "tile-texture: all tests passed\n";
    return 0;
}
