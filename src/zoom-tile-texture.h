// src/zoom-tile-texture.h
#pragma once

#include <cstdint>

// Whether a plane texture must be recreated before the next upload. Textures
// are sized to the participant's stream, which changes when the engine raises
// or lowers that participant's resolution. Kept pure so the rules are testable
// without a graphics device.
inline bool tile_texture_needs_realloc(uint32_t have_w, uint32_t have_h,
                                       uint32_t want_w, uint32_t want_h)
{
    if (want_w == 0 || want_h == 0) return false;  // nothing to allocate for
    return have_w != want_w || have_h != want_h;
}

// Whether the feed has pixels we have not uploaded yet. Generation 0 means no
// frame has ever been stored for this feed. Any difference counts, not just an
// increase: a rebuilt slot restarts its generation, and treating a lower value
// as "already uploaded" would pin the previous assignee's last frame on screen.
inline bool tile_texture_needs_upload(uint64_t uploaded_generation,
                                      uint64_t frame_generation)
{
    if (frame_generation == 0) return false;
    return uploaded_generation != frame_generation;
}
