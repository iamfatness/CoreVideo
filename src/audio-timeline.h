#pragma once

// The master clock every CoreVideo audio buffer is stamped from.
//
// Extracted so it can be tested without OBS, Qt or a live engine -- the same
// treatment audio-subscription-state.h and director-handover.h get, and for the
// same reason: it is arithmetic whose only failure symptom is bad audio on air.
//
// THE DEFECT THIS EXISTS FOR (2026-08-16, live show). Every audio publish site
// stamped its buffer os_gettime_ns() -- the wall-clock instant the plugin
// happened to read it. Zoom's buffers are exactly 10 ms apart, but they cross
// an IPC pipe and a shared-memory hop, so ARRIVAL is jittery. OBS received a
// stream whose timestamps advanced 8 ms, then 14 ms, then 3 ms, and reconciled
// that against its own audio clock by stretching, dropping and resampling,
// continuously. Both vMix and Viz Engine run a master clock; CoreVideo had
// none.
//
// The rule: never consult arrival except to anchor. Output advances by exactly
// one sample period per sample.

#include <cstdint>

struct AudioTimeline {
    // Wall-clock instant the current timeline began.
    uint64_t anchor_ns   = 0;
    // Samples published since the anchor. Monotonic within a timeline.
    uint64_t samples     = 0;
    // The rate the accumulated sample count is denominated in.
    uint32_t sample_rate = 0;
    bool     started     = false;
};

// Begins a new timeline. Call ONLY where the old one is genuinely meaningless:
// a participant re-subscribe, a new engine process, a sample-rate change.
//
// Deliberately NOT called on a gap. A mute is silence with a duration, and the
// timeline has to honour that duration or audio walks out of sync across a long
// show -- the drift EBU R37 exists to prevent.
inline void audio_timeline_reset(AudioTimeline &tl)
{
    tl = AudioTimeline{};
}

// The timestamp this buffer publishes at, advancing the timeline by `frames`.
//
// `arrival_ns` is consulted only to anchor a new timeline; once running it is
// ignored entirely, which is the whole point.
inline uint64_t audio_timeline_stamp(AudioTimeline &tl,
                                     uint32_t sample_rate,
                                     uint32_t frames,
                                     uint64_t arrival_ns)
{
    // A rate change invalidates the accumulated sample count: N samples at
    // 16 kHz is not N samples at 48 kHz. Re-anchor rather than mis-scale.
    if (!tl.started || sample_rate == 0 || sample_rate != tl.sample_rate) {
        tl.anchor_ns   = arrival_ns;
        tl.samples     = 0;
        tl.sample_rate = sample_rate;
        tl.started     = true;
        // A zero rate cannot advance a timeline; publish at arrival and leave
        // the counter alone so the next valid buffer re-anchors cleanly.
        if (sample_rate == 0) {
            tl.started = false;
            return arrival_ns;
        }
    }

    // Split the division so neither term can overflow and no rounding error
    // accumulates: `rem` is always < sample_rate, so rem * 1e9 stays far inside
    // uint64 even at 48 kHz, and the seconds term is exact.
    const uint64_t whole = tl.samples / tl.sample_rate;
    const uint64_t rem   = tl.samples % tl.sample_rate;
    const uint64_t ts = tl.anchor_ns
                      + whole * 1'000'000'000ULL
                      + (rem * 1'000'000'000ULL) / tl.sample_rate;

    tl.samples += frames;
    return ts;
}
