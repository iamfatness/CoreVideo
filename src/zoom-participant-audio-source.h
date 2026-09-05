#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "loudness-board.h"

void zoom_participant_audio_source_register();

// One live CoreVideoAudioSource, flattened for the control API and any other
// read-only consumer.
//
// These are the DEDICATED per-participant audio sources -- a different OBS
// source type from ZoomSource, with a different transport (the 8-slot ring in
// src/engine-ipc.h) and a different clock (src/audio-timeline.h). They are not
// registered with ZoomOutputManager and never appear in list_outputs, so
// before this existed there was no way at all to read the loss counter the
// ring was built to produce, nor the latency the ring is stamped against.
struct CoreVideoAudioSourceInfo {
    std::string source_name;      // the OBS source name the operator sees
    std::string source_uuid;      // engine-side subscription uuid
    const char *kind = "participant"; // participant | active_speaker | audience
    // Participant this source is actually subscribed to right now. For the
    // active-speaker and audience kinds this is resolved by the engine/director
    // rather than configured, so it can differ from what any dialog shows.
    uint32_t participant_id = 0;
    bool     subscribed = false;
    uint32_t audio_delay_ms = 0;
    // Engine capture to OBS publish, microseconds. 0 = NOT YET MEASURED, which
    // is not the same as zero latency -- consumers must distinguish the two.
    uint64_t audio_latency_us = 0;
    // Ring slots the writer lapped before this source drained them: audio that
    // was lost. The spec's acceptance criterion is measured against this.
    uint64_t overrun_slots = 0;
    uint64_t frame_count = 0;
};

// Snapshot of every live CoreVideoAudioSource. Safe to call from any thread.
std::vector<CoreVideoAudioSourceInfo> corevideo_audio_source_infos();

// Pushes a new global audio delay (already clamped to 0-500) to every live
// CoreVideoAudioSource, including ones already created. Returns the value
// actually applied after clamping.
uint32_t corevideo_set_global_audio_delay_ms(uint32_t delay_ms);

// One BS.1770-4 reading per live CoreVideoAudioSource, for the readiness
// board. Safe to call from any thread; takes g_sources_mtx and then each
// source's own mutex, in that order and never the reverse.
//
// The display name here is a CACHED copy, refreshed on the engine's roster
// callback. ZoomEngineClient::roster() deep-copies every ParticipantInfo --
// strings included -- under the client's hot mutex, so resolving a name on
// the audio path (about a hundred buffers a second, per source) would put a
// full roster copy on the media path.
std::vector<LoudnessReading> corevideo_loudness_readings();

// Starts every live source's mic-check window over. Integrated loudness is
// scoped to ONE panelist's check, not the session: without this the number
// is polluted by whoever spoke before them on the same source.
void corevideo_reset_loudness_windows();
