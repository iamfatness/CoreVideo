// src/zoom-tiles-effect-policy.h
#pragma once

#include <string>

// WHICH MISSING EFFECT HANDLES ARE FATAL, AND WHICH ONLY COST A FEATURE.
//
// This is policy, not graphics, so it lives in its own libobs-free header and
// is unit-tested (tests/tiles-effect-policy-test.cpp). zoom-tiles-effect.cpp
// resolves the real gs_* handles and hands their null-ness here as plain bools.
//
// The problem it exists to solve: the DLL and data/effects/corevideo-tiles.effect
// are two files that can be installed independently. We have already had one
// install put a new DLL in place without syncing data/ (caught only by a log
// check). When that happens the effect still compiles — it is simply an older
// one — and every technique or uniform the new DLL expects but the old file
// does not declare resolves to nullptr.
//
// Treating every one of those as fatal means a stale effect file loses the
// operator the ENTIRE WALL rather than the one feature the old file predates.
// For a live broadcast that is the difference between "the glow is missing" and
// "we are off air". So the split below is deliberate:
//
//   FATAL — the wall cannot be drawn at all without these.
//     * the compiled effect itself
//     * the I420 technique and its parameters. Every tile AND every
//       no-frame placeholder is drawn through it (tiles_begin_pass /
//       tiles_draw_neutral in zoom-supersource.cpp); without it the wall has no
//       tiles in it, which is not a degraded wall, it is no wall.
//     * the Solid technique and fill_color. This paints the whole canvas before
//       anything else. Without it nothing clears the frame, so the wall would
//       composite over undefined content — worse than a black frame, because it
//       is whatever happened to be in that target.
//
//   DEGRADING — the wall draws; one optional pass is skipped.
//     * the Glow technique and its parameters. The glow is a separate pass
//       drawn before the tiles, gated behind a size setting that DEFAULTS TO
//       OFF. Skipping it returns the wall to exactly what it rendered before
//       the control existed.
//
// The same reasoning applies to PARAMETERS, not just techniques — which is why
// the glow's uniforms are in the degrading set. But note the direction it runs:
// a missing glow uniform degrades the WHOLE GLOW, it is not individually
// ignored. A null handle here means the uniform is not declared in
// data/effects/corevideo-tiles.effect AT ALL — no shader in the file
// references that constant, so there is no register for a partially-set pass
// to inherit from (libobs/graphics/effect.c: gs_effect_get_param_by_name()
// returns null exactly when the name is absent from the parsed file). That is
// a different situation from a DECLARED-but-never-set uniform, where the
// stale-register risk is real — which is why every one of these uniforms has
// to be set before gs_technique_begin_pass() on the path that does set them
// (see zoom-tiles-effect.h).
//
// The gate exists for two reasons instead:
//   * a stale effect's Glow technique would silently ignore a parameter the
//     operator set, or mis-place the halo — the DLL cannot know what an old
//     technique does with a value it was never written to consume; and
//   * gs_effect_set_*() on a null parameter is null-safe but logs LOG_ERROR
//     per call (libobs/graphics/effect.c: effect_setval_inline()) — roughly
//     8 uniforms x 9 tiles x 60 fps ~= 4,000 log lines per second without
//     this gate.
// Skipping the pass avoids both.
//
// The fatal set's own parameters (border_color, border_width, corner_radius,
// tile_size, crop_uv) are NOT spared by that same argument, and that is not
// a mirror image of the glow's case — it is a stricter policy choice. An
// effect predating tile borders would, in the same sense as a pre-glow
// effect, very likely draw something with those uniforms left stale; nothing
// about the I420 technique makes that structurally impossible the way it is
// impossible to "skip" the technique itself. It is fatal anyway because these
// parameters govern the framing of the operator's own video — crop, size,
// corner shape — not a decorative layer over it, and the DLL has no way to
// tell "drew a plain square tile" from "drew the wrong crop of the source"
// without them. Blacking out the wall on a loud, logged error is safer than
// guessing that an old technique still frames the video correctly.

// One bool per handle tiles_effect_load() resolves: true = resolved.
// Deliberately mirrors the members of TilesEffect one-for-one — but nothing
// compile-time enforces that mirroring. A handle added to TilesEffect and
// left unclassified here compiles cleanly, defaults to false (unresolved),
// and is caught only if someone notices the two struct definitions have
// drifted apart, not by any mechanism in this file.
struct TilesEffectHandles {
    bool effect = false;

    // Fatal group: the tiles themselves.
    bool tech_i420           = false;
    bool param_y             = false;
    bool param_u             = false;
    bool param_v             = false;
    bool param_border_color  = false;
    bool param_border_width  = false;
    bool param_corner_radius = false;
    bool param_tile_size     = false;
    bool param_crop_uv       = false;

    // Fatal group: the background fill.
    bool tech_solid  = false;
    bool param_color = false;

    // Degrading group: the outer glow.
    bool tech_glow                = false;
    bool param_glow_color         = false;
    bool param_glow_quad_size     = false;
    bool param_glow_tile_center   = false;
    bool param_glow_tile_half     = false;
    bool param_glow_corner_radius = false;
    bool param_glow_size          = false;
    bool param_glow_intensity     = false;
    bool param_glow_falloff       = false;
};

struct TilesEffectStatus {
    // false: refuse to load. The caller destroys the effect and returns false.
    bool wall_drawable = false;
    // false: load anyway, but skip the glow pass for the rest of the session.
    bool glow_drawable = false;

    // Effect-file names of what did not resolve, comma-separated, for the log.
    // Empty exactly when the corresponding flag above is true.
    std::string missing_required;
    std::string missing_glow;
};

namespace tiles_effect_policy_detail {

inline void note(std::string &into, bool resolved, const char *name)
{
    if (resolved) return;
    if (!into.empty()) into += ", ";
    into += name;
}

}  // namespace tiles_effect_policy_detail

// Classifies a set of resolved/unresolved handles. Pure: no libobs, no logging,
// no side effects.
inline TilesEffectStatus classify_tiles_effect(const TilesEffectHandles &h)
{
    using tiles_effect_policy_detail::note;

    TilesEffectStatus st;

    // Named with the identifiers that appear in corevideo-tiles.effect, so the
    // log line points straight at what a stale file is missing.
    note(st.missing_required, h.effect, "the effect itself");
    note(st.missing_required, h.tech_i420, "technique I420");
    note(st.missing_required, h.param_y, "image");
    note(st.missing_required, h.param_u, "tex_u");
    note(st.missing_required, h.param_v, "tex_v");
    note(st.missing_required, h.param_border_color, "border_color");
    note(st.missing_required, h.param_border_width, "border_width");
    note(st.missing_required, h.param_corner_radius, "corner_radius");
    note(st.missing_required, h.param_tile_size, "tile_size");
    note(st.missing_required, h.param_crop_uv, "crop_uv");
    note(st.missing_required, h.tech_solid, "technique Solid");
    note(st.missing_required, h.param_color, "fill_color");

    note(st.missing_glow, h.tech_glow, "technique Glow");
    note(st.missing_glow, h.param_glow_color, "glow_color");
    note(st.missing_glow, h.param_glow_quad_size, "glow_quad_size");
    note(st.missing_glow, h.param_glow_tile_center, "glow_tile_center");
    note(st.missing_glow, h.param_glow_tile_half, "glow_tile_half");
    note(st.missing_glow, h.param_glow_corner_radius, "glow_corner_radius");
    note(st.missing_glow, h.param_glow_size, "glow_size");
    note(st.missing_glow, h.param_glow_intensity, "glow_intensity");
    note(st.missing_glow, h.param_glow_falloff, "glow_falloff");

    st.wall_drawable = st.missing_required.empty();
    // Gated on the wall as well: with no compiled effect there is nothing to
    // draw the glow with either, and a caller that reads glow_drawable without
    // first checking wall_drawable must not be told the glow is available.
    st.glow_drawable = st.wall_drawable && st.missing_glow.empty();

    return st;
}
