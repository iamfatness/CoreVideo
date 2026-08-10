// src/zoom-tiles-effect.h
#pragma once

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

    bool valid() const { return effect != nullptr && tech_i420 != nullptr; }
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
