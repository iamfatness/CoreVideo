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

// How far the derived timeline may fall from actual arrival before it resyncs.
//
// THE DEFECT THIS EXISTS FOR (2026-08-17, live meeting). Zoom emits one-way
// audio ONLY while a participant is producing sound. Every silence, no callback
// arrives and nothing advances the timeline -- so the buffer after a 5-second
// mute was stamped as if it followed the previous one immediately, 5 seconds
// behind wall clock, and it never caught up. In a normal conversation everyone
// is quiet most of the time, so every source walked steadily into the past
// until OBS gave up:
//
//   "Source X audio is lagging (over by 102.56 ms) at max audio buffering.
//    Restarting source audio."
//
// ...every ~1.3 seconds, on every source. Audible as continuous jitter, and
// strictly worse than the arrival stamping this replaced: that had jitter, this
// had unbounded drift.
//
// The design called for underrun to emit silence at the correct timestamp,
// which would hold the timeline continuous through a mute; that was never
// built, and "never re-anchor on a gap" is only safe WITH it. Bounding the
// drift is the cheaper half of the same guarantee: stay sample-accurate while
// the stream is continuous, and resync once the gap is too large to be jitter.
//
// 50 ms is chosen to sit well inside the ~100 ms at which OBS starts reporting
// a lagging source, and far outside the few milliseconds of IPC jitter this
// clock exists to absorb.
constexpr uint64_t kAudioTimelineMaxDriftNs = 50'000'000ULL;

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

// Advances the timeline by `frames` without publishing anything -- for audio
// that existed but was never read (a ring slot the writer lapped before the
// reader got to it, or one that could not be verified un-torn). Same doctrine
// as the comment above: a lost slot is silence of a KNOWN duration, and
// skipping the accounting shifts this source permanently earlier relative to
// everything else on the timeline, cumulatively, with no re-anchor to undo it.
// A no-op before the timeline has an anchor -- there is nothing yet to shift.
inline void audio_timeline_skip(AudioTimeline &tl, uint32_t frames)
{
    if (!tl.started) return;
    tl.samples += frames;
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
    uint64_t ts = tl.anchor_ns
                + whole * 1'000'000'000ULL
                + (rem * 1'000'000'000ULL) / tl.sample_rate;

    // Resync if the derived time has drifted too far from arrival to be
    // explained by jitter. The dominant cause is a participant going silent:
    // Zoom stops delivering entirely, the timeline stops advancing, and the
    // stream resumes stamped by however long they were quiet -- see
    // kAudioTimelineMaxDriftNs. Compared signed, because a timeline running
    // AHEAD of arrival is equally wrong and equally worth correcting.
    const int64_t drift = static_cast<int64_t>(arrival_ns) -
                          static_cast<int64_t>(ts);
    const int64_t limit = static_cast<int64_t>(kAudioTimelineMaxDriftNs);
    if (drift > limit || drift < -limit) {
        tl.anchor_ns = arrival_ns;
        tl.samples   = 0;
        ts           = arrival_ns;
    }

    tl.samples += frames;
    return ts;
}
