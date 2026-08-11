// src/zoom-tiles-effect.h
#pragma once

#include "zoom-tiles-effect-policy.h"

#include <graphics/graphics.h>

// The plugin-supplied effect used to draw tiles. Loaded once at module load
// and shared by every Tiles source: effects are immutable once compiled, and
// one wall per OBS instance is already the recommended configuration.
struct TilesEffect {
    gs_effect_t    *effect     = nullptr;
    gs_technique_t *tech_i420  = nullptr;
    gs_eparam_t    *param_y    = nullptr;
    gs_eparam_t    *param_u    = nullptr;
    gs_eparam_t    *param_v    = nullptr;
    // Per-tile border geometry, consumed by the I420 technique. Every one of
    // these has to be set before the tile's gs_technique_begin_pass(), which is
    // where libobs uploads a pass's parameters — see tiles_begin_pass().
    gs_eparam_t    *param_border_color  = nullptr;
    gs_eparam_t    *param_border_width  = nullptr;
    gs_eparam_t    *param_corner_radius = nullptr;
    gs_eparam_t    *param_tile_size     = nullptr;
    gs_eparam_t    *param_crop_uv       = nullptr;
    gs_technique_t *tech_solid  = nullptr;
    gs_eparam_t    *param_color = nullptr;
    // The outer glow: a separate technique drawn before the tiles, on a quad
    // expanded beyond the tile rect. Its uniforms are per-tile for exactly the
    // same reason the border's are, and have to be set before the tile's
    // gs_technique_begin_pass() — see glow_begin_pass().
    gs_technique_t *tech_glow                = nullptr;
    gs_eparam_t    *param_glow_color         = nullptr;
    gs_eparam_t    *param_glow_quad_size     = nullptr;
    gs_eparam_t    *param_glow_tile_center   = nullptr;
    gs_eparam_t    *param_glow_tile_half     = nullptr;
    gs_eparam_t    *param_glow_corner_radius = nullptr;
    gs_eparam_t    *param_glow_size          = nullptr;
    gs_eparam_t    *param_glow_intensity     = nullptr;
    gs_eparam_t    *param_glow_falloff       = nullptr;

    // Decided once at load by classify_tiles_effect() and cached, rather than
    // re-derived per frame: valid() is on the 60 Hz draw path.
    bool wall_drawable = false;
    bool glow_drawable = false;

    // Which handles resolved, in the form the policy header classifies. Kept
    // beside the members it reads so the two cannot drift apart unnoticed.
    TilesEffectHandles handles() const {
        TilesEffectHandles h;
        h.effect                  = effect != nullptr;
        h.tech_i420               = tech_i420 != nullptr;
        h.param_y                 = param_y != nullptr;
        h.param_u                 = param_u != nullptr;
        h.param_v                 = param_v != nullptr;
        h.param_border_color      = param_border_color != nullptr;
        h.param_border_width      = param_border_width != nullptr;
        h.param_corner_radius     = param_corner_radius != nullptr;
        h.param_tile_size         = param_tile_size != nullptr;
        h.param_crop_uv           = param_crop_uv != nullptr;
        h.tech_solid              = tech_solid != nullptr;
        h.param_color             = param_color != nullptr;
        h.tech_glow               = tech_glow != nullptr;
        h.param_glow_color        = param_glow_color != nullptr;
        h.param_glow_quad_size    = param_glow_quad_size != nullptr;
        h.param_glow_tile_center  = param_glow_tile_center != nullptr;
        h.param_glow_tile_half    = param_glow_tile_half != nullptr;
        h.param_glow_corner_radius = param_glow_corner_radius != nullptr;
        h.param_glow_size         = param_glow_size != nullptr;
        h.param_glow_intensity    = param_glow_intensity != nullptr;
        h.param_glow_falloff      = param_glow_falloff != nullptr;
        return h;
    }

    // "The wall can be drawn." NOT "everything resolved" — see
    // zoom-tiles-effect-policy.h. A stale effect file beside a new DLL must
    // cost the operator the feature it predates, never the whole wall.
    bool valid() const { return effect != nullptr && wall_drawable; }

    // "The outer glow pass can be drawn." False leaves the wall intact with the
    // glow skipped; the draw path must check this before touching any
    // param_glow_* handle, because a partially-set glow pass would inherit
    // whatever the previous pass left in the unset register.
    bool glow_valid() const { return effect != nullptr && glow_drawable; }
};

// Compiles data/effects/corevideo-tiles.effect and resolves its technique and
// parameters. Enters and leaves the graphics context itself. Returns false and
// logs loudly on failure — a silently missing effect renders an invisible
// source with no explanation, which is the worst possible symptom.
bool tiles_effect_load(TilesEffect &out);

// Drops our reference to the effect and resets the struct. Safe to call on
// an unloaded/failed TilesEffect. Note: libobs caches effects created from a
// file, so gs_effect_destroy() here is effectively a no-op until graphics
// shutdown rather than an immediate release — not a leak, just not the
// literal "release" the name suggests.
void tiles_effect_destroy(TilesEffect &fx);
