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

// Gates per-participant audio reconciliation on whether a scene collection is
// currently being torn down or loaded. Call with true on
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING *and* on
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP, and false on both
// OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED and
// OBS_FRONTEND_EVENT_FINISHED_LOADING.
//
// CLEANUP is not optional: two frontend paths (duplicate and rename a scene
// collection) run the full ClearSceneData + obs_load_sources sequence without
// ever firing CHANGING, and CLEANUP is emitted from inside ClearSceneData on
// every path, exempt from the disableSaving filter that drops other events
// mid-load. Needed at all because obs_queue_task(OBS_TASK_UI, ...) runs its
// task inline, not deferred, when called from a thread that is already the UI
// thread — which a scene-collection load, running on the UI thread, always
// is. Turning the gate off sweeps every live Tiles source and requests a
// reconcile on each, so nothing suppressed while it was on is lost. See
// src/zoom-supersource.cpp's request_audio_reconcile for the full reasoning.
void zoom_supersource_set_collection_loading(bool loading);
