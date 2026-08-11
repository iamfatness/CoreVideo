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

// Snapshots every marked source in the scene collection. Takes no arguments on
// purpose: ownership is read off each source's own marker, never inferred from
// who is asking. A marker naming a uuid with no live source is reported with an
// empty owner_uuid, which the planner reads as an adoptable orphan.
std::vector<TilesAudioSourceState> tiles_audio_scan();

// Applies the plan. Creates into group_name, which must already exist —
// creating the group is the operator's act, and is also how they opt in.
void tiles_audio_apply(const TilesAudioPlan &plan, const std::string &group_name,
                       const std::string &self_uuid);
