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
//
// THE BOARD SHOWS SOURCES, NOT THE ROSTER. A row exists because an operator
// created a Participant-kind CoreVideo audio source and pointed it at
// somebody -- it is not a live view of who is in the meeting. Two
// consequences an operator will run into and may read as a bug: a panelist
// with no source pointed at them never appears at all, and a panelist who
// rejoins drops to "- unassigned -"/"no audio" until the operator re-points
// the source at them, because Zoom participant ids are meeting-scoped and do
// not survive a rejoin (the same fact documented for talkback nomination in
// CLAUDE.md). Both are correct behaviour, not defects, but they are the most
// likely way this board gets reported as broken.
//
// ActiveSpeaker- and Audience-kind sources are excluded from the board
// entirely (never a row, never a vote on the panel median) -- see
// src/loudness-board.h's loudness_panel_median()/loudness_board_build() for
// why.

void corevideo_loudness_meter_source_register();

// Compiles/releases the shared effect. Called from plugin-main.cpp alongside
// the Tiles equivalents; each call to gs_effect_create_from_file() allocates
// its own effect (libobs does NOT cache or dedupe effects created from a
// file, despite what an earlier version of this comment said) -- see the
// caution beside s_meter_effect in the .cpp for what believing otherwise
// costs.
void corevideo_loudness_meter_load_gfx();
void corevideo_loudness_meter_unload_gfx();
