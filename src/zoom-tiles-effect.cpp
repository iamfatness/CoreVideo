// src/zoom-tiles-effect.cpp
#include "zoom-tiles-effect.h"

#include <obs-module.h>

bool tiles_effect_load(TilesEffect &out)
{
    tiles_effect_destroy(out);

    char *path = obs_module_file("effects/corevideo-tiles.effect");
    if (!path) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles effect not found: effects/corevideo-tiles.effect "
             "is missing from the plugin's data directory");
        return false;
    }

    char *error = nullptr;
    obs_enter_graphics();
    out.effect = gs_effect_create_from_file(path, &error);
    if (out.effect) {
        out.tech_i420 = gs_effect_get_technique(out.effect, "I420");
        out.param_y   = gs_effect_get_param_by_name(out.effect, "image");
        out.param_u   = gs_effect_get_param_by_name(out.effect, "tex_u");
        out.param_v   = gs_effect_get_param_by_name(out.effect, "tex_v");
        out.param_border_color =
            gs_effect_get_param_by_name(out.effect, "border_color");
        out.param_border_width =
            gs_effect_get_param_by_name(out.effect, "border_width");
        out.param_corner_radius =
            gs_effect_get_param_by_name(out.effect, "corner_radius");
        out.param_tile_size =
            gs_effect_get_param_by_name(out.effect, "tile_size");
        out.param_crop_uv =
            gs_effect_get_param_by_name(out.effect, "crop_uv");
        out.tech_solid  = gs_effect_get_technique(out.effect, "Solid");
        out.param_color = gs_effect_get_param_by_name(out.effect, "fill_color");
    }
    obs_leave_graphics();

    bfree(path);

    if (!out.effect) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Tiles effect failed to compile: %s",
             error ? error : "(no compiler message)");
        bfree(error);
        return false;
    }
    bfree(error);

    if (!out.valid() || !out.param_y || !out.param_u || !out.param_v ||
        !out.param_color || !out.param_border_color ||
        !out.param_border_width || !out.param_corner_radius ||
        !out.param_tile_size || !out.param_crop_uv) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles effect compiled but is missing its I420 "
             "or Solid technique, or a plane/color/border parameter");
        tiles_effect_destroy(out);
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Tiles effect loaded");
    return true;
}

void tiles_effect_destroy(TilesEffect &fx)
{
    if (fx.effect) {
        obs_enter_graphics();
        gs_effect_destroy(fx.effect);
        obs_leave_graphics();
    }
    fx = TilesEffect{};
}
