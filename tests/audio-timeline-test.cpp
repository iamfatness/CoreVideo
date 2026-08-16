// tests/audio-timeline-test.cpp
// The master clock every CoreVideo audio buffer is stamped from.
//
// The defect this exists for (2026-08-16, live show): every audio publish site
// stamped its buffer with os_gettime_ns() -- the wall-clock instant the plugin
// happened to read it. Zoom's buffers are exactly 10 ms apart, but they cross
// an IPC pipe and a shared-memory hop, so ARRIVAL is jittery. OBS was handed a
// stream whose timestamps advanced 8 ms, then 14 ms, then 3 ms, and reconciled
// it against its own audio clock by stretching, dropping and resampling --
// continuously. The operator heard it as "audio is very bad".
//
// The fix is to stop consulting arrival at all. Timestamps come from a
// cumulative sample count, so output advances by exactly one sample period per
// sample no matter when the buffer turned up.

#include "audio-timeline.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static constexpr uint32_t kRate   = 48000;   // broadcast standard
static constexpr uint32_t kFrames = 480;     // Zoom's 10 ms buffer
static constexpr uint64_t k10ms   = 10'000'000ULL;

int main()
{
    // --- The first buffer anchors the timeline to its arrival ---
    {
        AudioTimeline tl{};
        const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        check(ts == 1'000'000'000ULL,
              "the first buffer must publish at its own arrival time -- there is "
              "nothing else to anchor to");
    }

    // --- Jittery arrival must NOT reach the output ---
    {
        AudioTimeline tl{};
        const uint64_t base = 5'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        // Arrivals wander badly: +3 ms, then +21 ms, then +7 ms.
        const uint64_t t1 = audio_timeline_stamp(tl, kRate, kFrames, base + 3'000'000ULL);
        const uint64_t t2 = audio_timeline_stamp(tl, kRate, kFrames, base + 24'000'000ULL);
        const uint64_t t3 = audio_timeline_stamp(tl, kRate, kFrames, base + 31'000'000ULL);
        check(t1 == base + k10ms,
              "second buffer drifted with arrival instead of advancing one "
              "sample period -- this is the jitter reaching OBS");
        check(t2 == base + 2 * k10ms, "third buffer drifted with arrival");
        check(t3 == base + 3 * k10ms, "fourth buffer drifted with arrival");
    }

    // --- A mute is silence with a duration, not a new timeline ---
    {
        AudioTimeline tl{};
        const uint64_t base = 9'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        // Caller keeps the timeline moving through the mute by stamping the
        // silence it emits: 5 seconds is 500 buffers of 10 ms.
        for (int i = 0; i < 500; ++i)
            audio_timeline_stamp(tl, kRate, kFrames, base + 999'999'999ULL);
        const uint64_t after = audio_timeline_stamp(tl, kRate, kFrames, base);
        check(after == base + 501 * k10ms,
              "a 5-second mute did not produce 5 seconds of timeline -- "
              "re-anchoring on gaps is the drift EBU R37 exists to prevent");
    }

    // --- No drift over a long show. One million samples is ~20.8 seconds;
    // any per-buffer rounding error would have accumulated visibly by here ---
    {
        AudioTimeline tl{};
        const uint64_t base = 0;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        const uint64_t buffers = 1'000'000ULL / kFrames;   // 2083
        uint64_t last = 0;
        for (uint64_t i = 0; i < buffers; ++i)
            last = audio_timeline_stamp(tl, kRate, kFrames, base);
        check(last == buffers * k10ms,
              "timestamps accumulated rounding error over 2083 buffers");
    }

    // --- An explicit reset starts a new timeline: subscribe, engine restart ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        audio_timeline_stamp(tl, kRate, kFrames, 1'000'000'000ULL);
        audio_timeline_reset(tl);
        const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, 77'000'000'000ULL);
        check(ts == 77'000'000'000ULL,
              "an explicit reset did not re-anchor -- a re-subscribe or engine "
              "restart is a genuinely new timeline");
    }

    // --- A sample-rate change re-anchors on its own: the old sample count
    // means nothing at the new rate ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 2'000'000'000ULL);
        const uint64_t ts = audio_timeline_stamp(tl, 16000, 160, 3'000'000'000ULL);
        check(ts == 3'000'000'000ULL,
              "a sample-rate change did not re-anchor -- the accumulated sample "
              "count is meaningless at the new rate");
    }

    // --- A zero rate or zero frame count must not divide by zero or advance ---
    {
        AudioTimeline tl{};
        audio_timeline_stamp(tl, kRate, kFrames, 4'000'000'000ULL);
        const uint64_t ts = audio_timeline_stamp(tl, 0, kFrames, 4'500'000'000ULL);
        check(ts == 4'500'000'000ULL,
              "a zero sample rate must fall back to arrival rather than divide "
              "by zero");
    }

    if (failures == 0)
        std::cout << "audio-timeline: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
