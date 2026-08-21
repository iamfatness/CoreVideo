#pragma once

// Paces ISO video frames to a fixed cadence before they reach ffmpeg's raw
// pipe, by computing how many frame-slots are due for each real arrival —
// the caller emits its newest frame that many times (1 in the ordinary
// case, >1 to backfill a stall, 0 to drop an excess frame from a burst).
//
// THE DEFECT THIS EXISTS FOR (2026-08-21, design review: "does ZoomISO
// handle a source's fps fluctuating 10-60fps"). record_video_frame() is
// called 1:1 with whatever Zoom's coalescing video dispatch lane actually
// delivers — fluctuating with network/encoder conditions outside our
// control, never padded to a fixed cadence, sometimes coalesced DOWN under
// load — while ffmpeg's rawvideo input was told a flat, declared "-r 30"
// with no other way to time each frame (rawvideo carries no per-frame
// timestamps). A source averaging 15fps recorded under that declared 30fps
// played back at ~2x speed and finished in roughly half the real meeting's
// duration; a sustained 60fps source would run at half speed. Measured
// live 2026-08-19: observed_fps of 16-18 on every participant during a
// normal verification meeting — every ISO file from that session was
// materially short.
//
// THE FIRST FIX ATTEMPT WAS WRONG (same review). -use_wallclock_as_timestamps
// looks like the textbook answer — stamp each frame with real read time
// instead of assuming CFR — but probed directly against this project's
// ffmpeg build (8.1.1) with `ffprobe -show_frames`, the rawvideo demuxer
// does not honor it at all: every frame's pts_time still came back as a
// pure frame_index/30, identical to the unpatched behavior, even fed a
// deliberate 1.5s real-time gap. Pacing before the pipe sidesteps that
// (undocumented, apparently silently ignored) demuxer behavior entirely:
// ffmpeg is never asked to infer timing it cannot actually derive from a
// raw byte stream — what it receives really is CFR, because we made it so.
//
// Silence-fill for the paired WAV (src/iso-audio-gap-fill.h) is the same
// doctrine applied to the other stream: a gap is accounted for by writing
// real time worth of *something*, never left to silently shrink the file.

#include <cstdint>

constexpr uint32_t kIsoVideoTargetFps = 30;
constexpr uint64_t kIsoVideoFramePeriodNs =
    1'000'000'000ULL / kIsoVideoTargetFps;

// Above this many held-frame duplicates in one catch-up, a stall is treated
// as anomalous (a debugger pause, a suspended/backgrounded process, a clock
// jump) rather than an source that is merely slow: the caller still gets a
// bounded, visible catch-up instead of none, but the schedule resyncs
// rather than queuing an unbounded burst of identical frames. 300 frames is
// 10s at the target rate — generous for any real network stall while still
// bounding the worst case. In practice other machinery (stale-video
// recovery, the 60s unresolved-session close) usually reacts to a stall
// this long before the pacer would ever need this cap.
constexpr uint32_t kIsoVideoMaxCatchUpFrames = 300;

// How many frame-slots are due for a real arrival at `now_ns`, given the
// next slot the caller has not yet filled (`next_due_ns`, advanced in
// place past every slot this call accounts for). The caller seeds
// next_due_ns to the session's start time on the first real frame — see
// ZoomIsoRecorder::ensure_session_locked's video_next_due_ns
// initialization, mirroring how started_ns itself is seeded.
//
// Returns 0 if no slot is due yet (this real frame arrived faster than the
// target rate — drop it, the next due slot will pick up the frame after
// it), 1 in the ordinary case, or more than 1 to backfill a stall. Never
// moves the schedule backwards: an out-of-order or repeated timestamp
// leaves next_due_ns untouched and returns 0.
inline uint32_t iso_video_frames_due(uint64_t &next_due_ns, uint64_t now_ns)
{
    if (now_ns < next_due_ns) return 0;
    uint32_t due = 0;
    while (next_due_ns <= now_ns && due < kIsoVideoMaxCatchUpFrames) {
        next_due_ns += kIsoVideoFramePeriodNs;
        ++due;
    }
    if (next_due_ns <= now_ns) {
        // Backlog exceeds the cap: resync from here instead of leaving an
        // ever-growing count of slots permanently pending.
        next_due_ns = now_ns + kIsoVideoFramePeriodNs;
    }
    return due;
}
