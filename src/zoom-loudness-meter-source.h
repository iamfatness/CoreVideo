#pragma once

// The CoreVideo Loudness Meter: a preshow readiness board, drawn as an OBS
// source so it can sit on a multiview, a projector or a producer's monitor
// without a dock being open.
//
// One row per live CoreVideo audio source: the panelist's name, their
// deviation in LU from the panel reference, and a pass/fail verdict. Bars are
// drawn with the Solid technique already in data/effects/corevideo-tiles.effect
// -- there is no new effect file, because a solid quad is all a bar is and a
// second .effect is a second thing that can go missing beside a new DLL.
// Labels are private child text sources, so a Norwegian display name renders
// correctly instead of through a hand-rolled ASCII font.

void corevideo_loudness_meter_source_register();

// Compiles/releases the shared effect. Called from plugin-main.cpp alongside
// the Tiles equivalents; libobs caches effects created from a file, so this
// costs nothing beyond the Tiles source's own load.
void corevideo_loudness_meter_load_gfx();
void corevideo_loudness_meter_unload_gfx();
