// tests/tiles-effect-policy-test.cpp
// Which missing effect handles are fatal and which only cost a feature
// (src/zoom-tiles-effect-policy.h).
//
// The load-bearing assertion in here is ASYMMETRY, and it is a live-broadcast
// safety property, not a graphics one. The DLL and
// data/effects/corevideo-tiles.effect install independently, and this project
// has already had one install put a new DLL in place without syncing data/.
// When that happens the old effect still compiles; the handles the new DLL
// expects and the old file does not declare come back null.
//
// If those are all treated the same, a stale effect file costs the operator the
// ENTIRE WALL — the source renders nothing — instead of the one feature the old
// file predates. So:
//
//   * missing tile or fill handles  -> fatal    (there is no wall without them)
//   * missing glow handles          -> degrade  (the wall draws, glow skipped)
//
// Every case below is asserted in BOTH directions: a fatal handle must not
// merely fail, it must not take the glow flag with it in a way that hides the
// real fault, and a glow handle must not merely degrade, it must leave
// wall_drawable TRUE. A policy that returned false for everything, or true for
// everything, fails here.

#include "zoom-tiles-effect-policy.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

// Everything resolved: the state after a successful load against a matching
// effect file.
static TilesEffectHandles all_present()
{
    TilesEffectHandles h;
    h.effect                   = true;
    h.tech_i420                = true;
    h.param_y                  = true;
    h.param_u                  = true;
    h.param_v                  = true;
    h.param_border_color       = true;
    h.param_border_width       = true;
    h.param_corner_radius      = true;
    h.param_tile_size          = true;
    h.param_crop_uv            = true;
    h.tech_solid               = true;
    h.param_color              = true;
    h.tech_glow                = true;
    h.param_glow_color         = true;
    h.param_glow_quad_size     = true;
    h.param_glow_tile_center   = true;
    h.param_glow_tile_half     = true;
    h.param_glow_corner_radius = true;
    h.param_glow_size          = true;
    h.param_glow_intensity     = true;
    return h;
}

struct Handle {
    const char *label;         // what the log should name
    bool TilesEffectHandles::*member;
};

// The handles the wall cannot be drawn without. Listed here rather than derived
// from the policy header so the test states the expected classification
// independently of the code under test.
static std::vector<Handle> fatal_handles()
{
    return {
        {"technique I420",   &TilesEffectHandles::tech_i420},
        {"image",            &TilesEffectHandles::param_y},
        {"tex_u",            &TilesEffectHandles::param_u},
        {"tex_v",            &TilesEffectHandles::param_v},
        {"border_color",     &TilesEffectHandles::param_border_color},
        {"border_width",     &TilesEffectHandles::param_border_width},
        {"corner_radius",    &TilesEffectHandles::param_corner_radius},
        {"tile_size",        &TilesEffectHandles::param_tile_size},
        {"crop_uv",          &TilesEffectHandles::param_crop_uv},
        {"technique Solid",  &TilesEffectHandles::tech_solid},
        {"fill_color",       &TilesEffectHandles::param_color},
    };
}

// The handles whose absence must cost the glow and nothing else.
static std::vector<Handle> glow_handles()
{
    return {
        {"technique Glow",     &TilesEffectHandles::tech_glow},
        {"glow_color",         &TilesEffectHandles::param_glow_color},
        {"glow_quad_size",     &TilesEffectHandles::param_glow_quad_size},
        {"glow_tile_center",   &TilesEffectHandles::param_glow_tile_center},
        {"glow_tile_half",     &TilesEffectHandles::param_glow_tile_half},
        {"glow_corner_radius", &TilesEffectHandles::param_glow_corner_radius},
        {"glow_size",          &TilesEffectHandles::param_glow_size},
        {"glow_intensity",     &TilesEffectHandles::param_glow_intensity},
    };
}

static bool mentions(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

int main()
{
    // ── A matching effect file: everything available, nothing to report ──────
    {
        const TilesEffectStatus st = classify_tiles_effect(all_present());
        if (!st.wall_drawable || !st.glow_drawable) {
            std::cerr << "a fully resolved effect must be fully drawable\n";
            return 1;
        }
        if (!st.missing_required.empty() || !st.missing_glow.empty()) {
            std::cerr << "nothing is missing, so both reports must be empty; got '"
                      << st.missing_required << "' / '" << st.missing_glow << "'\n";
            return 1;
        }
    }

    // ── No compiled effect at all: nothing is drawable ───────────────────────
    {
        TilesEffectHandles h;  // every member default-false
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (st.wall_drawable || st.glow_drawable) {
            std::cerr << "with no effect, neither the wall nor the glow is drawable\n";
            return 1;
        }
        if (!mentions(st.missing_required, "the effect itself")) {
            std::cerr << "a missing effect must be named: '"
                      << st.missing_required << "'\n";
            return 1;
        }
    }

    // ── Each fatal handle, alone: refuse the load ────────────────────────────
    //
    // One at a time, from an otherwise complete set, so the verdict can only
    // come from the handle under test.
    for (const Handle &f : fatal_handles()) {
        TilesEffectHandles h = all_present();
        h.*(f.member) = false;
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (st.wall_drawable) {
            std::cerr << "missing " << f.label
                      << " must be fatal: the wall cannot be drawn without it\n";
            return 1;
        }
        if (st.glow_drawable) {
            std::cerr << "missing " << f.label
                      << " must not leave the glow reported as drawable\n";
            return 1;
        }
        if (!mentions(st.missing_required, f.label)) {
            std::cerr << "the fatal report must name " << f.label << ", got '"
                      << st.missing_required << "'\n";
            return 1;
        }
        // The operator's diagnosis depends on the two reports not being mixed:
        // a tile handle named in the glow report would send them looking at the
        // wrong control.
        if (!st.missing_glow.empty()) {
            std::cerr << "missing " << f.label
                      << " is not a glow problem, but the glow report says '"
                      << st.missing_glow << "'\n";
            return 1;
        }
    }

    // ── Each glow handle, alone: keep the wall, lose the glow ────────────────
    //
    // This is the case the whole header exists for. A stale effect file predates
    // the glow, so its technique and uniforms are exactly what comes back null.
    for (const Handle &g : glow_handles()) {
        TilesEffectHandles h = all_present();
        h.*(g.member) = false;
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (!st.wall_drawable) {
            std::cerr << "missing " << g.label
                      << " must NOT black out the wall — tiles, background, "
                         "borders and crop are all still drawable\n";
            return 1;
        }
        if (st.glow_drawable) {
            std::cerr << "missing " << g.label
                      << " must switch the glow off; a pass opened without it "
                         "would draw that uniform from a stale register\n";
            return 1;
        }
        if (!mentions(st.missing_glow, g.label)) {
            std::cerr << "the glow report must name " << g.label << ", got '"
                      << st.missing_glow << "'\n";
            return 1;
        }
        if (!st.missing_required.empty()) {
            std::cerr << "missing " << g.label
                      << " is not fatal, so the fatal report must stay empty; got '"
                      << st.missing_required << "'\n";
            return 1;
        }
    }

    // ── The whole glow group gone at once: the stale-file case verbatim ──────
    //
    // An effect file from before the glow existed has none of these, not one.
    {
        TilesEffectHandles h = all_present();
        for (const Handle &g : glow_handles()) h.*(g.member) = false;
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (!st.wall_drawable) {
            std::cerr << "an effect file predating the glow entirely must still "
                         "draw the wall\n";
            return 1;
        }
        if (st.glow_drawable) {
            std::cerr << "an effect file predating the glow cannot draw one\n";
            return 1;
        }
        // Every one of them named, so the log says what to reinstall rather
        // than stopping at the first.
        for (const Handle &g : glow_handles()) {
            if (!mentions(st.missing_glow, g.label)) {
                std::cerr << "the glow report stopped short of " << g.label
                          << ": '" << st.missing_glow << "'\n";
                return 1;
            }
        }
    }

    // ── Both groups damaged: fatal wins, and says so ─────────────────────────
    //
    // wall_drawable false must never coexist with glow_drawable true, however
    // the handles fell: the draw path reads glow_valid() on its own.
    {
        TilesEffectHandles h = all_present();
        h.tech_i420 = false;
        h.tech_glow = false;
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (st.wall_drawable || st.glow_drawable) {
            std::cerr << "with the tile technique gone nothing is drawable\n";
            return 1;
        }
        if (!mentions(st.missing_required, "technique I420") ||
            !mentions(st.missing_glow, "technique Glow")) {
            std::cerr << "both reports must still name their own losses\n";
            return 1;
        }
    }

    // ── Multiple names in one report are separated, not run together ─────────
    {
        TilesEffectHandles h = all_present();
        h.param_glow_size      = false;
        h.param_glow_intensity = false;
        const TilesEffectStatus st = classify_tiles_effect(h);
        if (!mentions(st.missing_glow, "glow_size, glow_intensity")) {
            std::cerr << "missing names should read as a list, got '"
                      << st.missing_glow << "'\n";
            return 1;
        }
    }

    std::cout << "tiles-effect-policy: all tests passed\n";
    return 0;
}
