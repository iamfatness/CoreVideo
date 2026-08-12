// src/zoom-tiles-audio.h
// The libobs half of per-participant Tiles audio: reads what exists, applies
// what the planner decided. Every scene-collection mutation in this feature
// happens here and nowhere else.

#pragma once

#include "zoom-tiles-audio-plan.h"

#include <string>
#include <vector>

// Written into every source this feature creates, holding the creating Tiles
// source's obs_source_get_uuid(). Ownership is decided by this marker and
// never by name: the operator can rename anything at any time, and a name
// match would eventually let the plugin adopt — and mute — a source it did
// not create.
#define CV_TILES_AUDIO_OWNER_KEY "cv_tiles_audio_owner"

// The scan and apply halves of a reconcile are deliberately NOT declared here.
// They are internal to zoom-tiles-audio.cpp, because running either without
// the other — or either of them outside the process-wide lock
// tiles_audio_reconcile takes — reintroduces exactly the cross-source race
// that lock exists to close. One entry point, one lock, no way to bypass it.
//
// Runs scan → plan → apply as one atomic step, serialised across every Tiles
// source in the process. Nothing else serialises two Tiles sources against
// each other — the per-ctx engine_mutex is not held across this call, and the
// per-ctx coalescing flag only collapses one source's own burst — so without
// this two of them could both call tiles_audio_scan(), both see nothing
// for a participant one is about to gain, and both plan a Create — the loser
// would then hit its own name clash and uniquify into a genuine duplicate.
// Holding one process-wide lock across the whole scan-and-apply pair for
// every caller closes that: whichever Tiles source reconciles first sees its
// own Create (or someone else's prior one) before the next reconcile's scan
// runs. No-op if params.enabled is false.
//
// An EMPTY group_name is not a no-op, and this is the one asymmetry worth
// knowing about: it means the operator cleared the field, which is them
// turning the feature off. Off has to actually be off, so this runs one
// reconcile with no assignments at all — muting every source this Tiles
// source owns, creating nothing, and touching nothing owned by anyone else.
// The mute is reversible: naming a group again unmutes whoever is back on the
// wall. Calling it repeatedly with an empty group is harmless — the second
// call finds everything already muted and plans nothing.
void tiles_audio_reconcile(const std::vector<uint32_t>        &assignments,
                           const std::vector<ParticipantInfo> &roster,
                           const TilesAudioPlanParams          &params,
                           const std::string                   &group_name);
