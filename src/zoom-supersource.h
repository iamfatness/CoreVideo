#pragma once

// Registers the "CoreVideo Tiles" OBS source (id: corevideo_tiles_source),
// which renders assigned Zoom participants as identical, evenly-spaced tiles.
void zoom_supersource_register();

// Loads/destroys the shared I420 render effect used by every Tiles source.
// Call zoom_supersource_load_gfx() once from obs_module_load (after
// zoom_supersource_register()) and zoom_supersource_unload_gfx() from
// obs_module_unload. See src/zoom-tiles-effect.h for details.
void zoom_supersource_load_gfx();
void zoom_supersource_unload_gfx();
