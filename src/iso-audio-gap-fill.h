#pragma once

// How much silence to backfill into an ISO WAV before the next real audio
// buffer, so the file's own timeline tracks real elapsed time instead of
// shrinking by every silent stretch.
//
// THE DEFECT THIS EXISTS FOR (2026-08-21, design review, prompted by "how
// does ZoomISO handle a source's fps fluctuating 10-60fps and does audio
// stay in sync with video"). Zoom only calls back one-way audio for a
// participant currently producing sound (see src/audio-timeline.h) --
// ZoomIsoRecorder::record_audio_frame() appended PCM only when a callback
// arrived, with no accounting for the gaps between them. A session that
// opened at T0 and stayed quiet until T0+45s wrote 45s of real speech
// starting at WAV offset 0, not offset 45s: every silent stretch permanently
// shortened the file relative to wall clock and shifted everything after it
// earlier. Paired against the video side's own fix (real per-frame wallclock
// timestamps instead of an assumed-constant frame rate -- see the
// -use_wallclock_as_timestamps / -fps_mode vfr change in
// zoom-iso-recorder.cpp), an audio track built this way would drift out of
// sync with its own video from the very first silence.
//
// The fix mirrors what src/audio-timeline.h already does for the live OBS
// path, for the same underlying reason: treat a gap as silence of a KNOWN
// duration and account for it, never skip the accounting. The mechanism
// differs because the two paths write to different things -- the live path
// advances a virtual presentation clock, this one has to physically emit
// zero-valued PCM frames, since a WAV's only concept of time is its byte
// position. Both keep the same doctrine: gaps are accounted for, not
// silently dropped, and BOTH streams (video's real per-frame timestamps,
// audio's backfilled silence) are anchored to the same os_gettime_ns() clock
// the ISO recorder has always used -- see the comment on that choice at the
// existing ISO audio.timestamp call site in zoom-source.cpp.

#include <cstdint>

// Above this, a gap is treated as anomalous (a debugger pause, a system
// sleep/resume, a clock jump) rather than a real meeting silence: backfill
// is capped and the caller should treat it as a resync point instead of
// writing the difference. 10 minutes is generous for any ordinary meeting
// pause -- a participant who is only listening routinely goes quiet for
// minutes at a stretch -- while still bounding the worst case: an unbounded
// gap could otherwise turn one bad timestamp into a multi-hour, multi-GB
// silence file.
constexpr uint64_t kIsoAudioMaxBackfillNs = 600'000'000'000ULL;

// Below this, a gap is treated as ordinary dispatch/IPC arrival jitter, not
// real silence -- no backfill. Caller's reference point stays wherever
// content duration already put it (see the "content-driven playhead" note
// at the call site); when arrival is only jittered, not truly gapped, that
// playhead naturally reabsorbs the difference on its own without ever
// treating it as time to fill.
//
// THE DEFECT THIS THRESHOLD EXISTS FOR (2026-08-21, same live incident as
// the file-level doctrine comment below). Advancing the reference by
// content duration instead of snapping to arrival stops the *steady-state*
// version of the bug, but a zero-threshold gap check still back-fills the
// full jitter on every call whose arrival happens to land even 1ns after
// the reference -- each such call pushes the reference ahead by the real
// content duration PLUS that jitter, which is fine in isolation, but a
// synthetic zero-mean-jitter regression test (tests/iso-audio-gap-fill-
// test.cpp) showed the naive version still inserting nonzero silence for
// ordinary jitter with no real gaps at all. 50ms mirrors
// kAudioTimelineMaxDriftNs in src/audio-timeline.h -- this codebase's own
// precedent for "how much drift from arrival is normal jitter vs. a real
// gap" on the live audio path, reused here for the same distinction on a
// different clock.
constexpr uint64_t kIsoAudioJitterToleranceNs = 50'000'000ULL;

// Frames of silence to write before the next real buffer, given the real
// time elapsed since the last write (or session start, if nothing has been
// written yet) and this session's sample rate. Returns 0 if there is no
// gap, arrival went backwards (clock jitter -- never write negative time),
// the gap is within ordinary jitter tolerance, or the gap exceeds the cap
// (the caller resyncs instead of backfilling).
inline uint64_t iso_audio_silence_frames(uint64_t last_write_ns,
                                         uint64_t now_ns,
                                         uint32_t sample_rate)
{
    if (sample_rate == 0 || now_ns <= last_write_ns) return 0;
    const uint64_t gap_ns = now_ns - last_write_ns;
    if (gap_ns <= kIsoAudioJitterToleranceNs) return 0;
    if (gap_ns > kIsoAudioMaxBackfillNs) return 0;
    return (gap_ns * sample_rate) / 1'000'000'000ULL;
}
