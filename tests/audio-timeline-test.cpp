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
        //
        // arrival ADVANCES with each buffer, because that is what a real caller
        // does -- wall clock moves while it emits silence. Freezing arrival here
        // (as this fixture originally did) is a scenario no caller produces, and
        // it now reads as 5 seconds of drift, which correctly trips the resync.
        for (int i = 0; i < 500; ++i)
            audio_timeline_stamp(tl, kRate, kFrames,
                                 base + static_cast<uint64_t>(i + 1) * k10ms);
        const uint64_t after =
            audio_timeline_stamp(tl, kRate, kFrames, base + 501 * k10ms);
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
        // arrival advances in lockstep, as it does in a live stream, so the
        // drift guard never fires and this measures pure timeline arithmetic.
        for (uint64_t i = 0; i < buffers; ++i)
            last = audio_timeline_stamp(tl, kRate, kFrames,
                                        base + (i + 1) * k10ms);
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

    // --- THE DEFECT THIS FILE MISSED (2026-08-17, live meeting) ---
    // Zoom emits one-way audio ONLY while a participant is producing sound.
    // Every silence, no buffer arrives and the timeline does not advance -- so
    // the next buffer after a 5-second mute was stamped as if it followed the
    // previous one immediately, putting it 5 seconds behind wall clock. In a
    // normal conversation each person is quiet most of the time, so every
    // source walked progressively into the past until OBS gave up:
    //   "Source X audio is lagging (over by 102.56 ms) at max audio buffering.
    //    Restarting source audio."
    // ...repeating every ~1.3s on every source. That is the jitter the operator
    // heard, and it is WORSE than the arrival-stamped clock this replaced:
    // arrival stamping has jitter, this had unbounded drift.
    //
    // The spec required "underrun emits silence at the correct timestamp" to
    // hold the timeline continuous through a mute. That was never implemented,
    // and the "never re-anchor on a gap" rule is only safe WITH it. Bounding
    // the drift is the cheaper half: keep sample-accurate stamping while the
    // stream is continuous, and resync when it has fallen too far behind to be
    // explained by jitter.
    {
        AudioTimeline tl{};
        const uint64_t base = 100'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        // Participant goes quiet for 5 seconds: no callbacks at all, so nothing
        // advances the timeline. They speak again at base + 5s + 10ms.
        const uint64_t after_mute = base + 5'010'000'000ULL;
        const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, after_mute);
        check(ts > after_mute - kAudioTimelineMaxDriftNs,
              "a buffer arriving after a 5-second silence was stamped ~5 seconds "
              "in the past -- OBS reports the source lagging and restarts its "
              "audio, over and over, which is what the operator hears");
    }

    // --- Jitter within the threshold must still be absorbed. If this fails the
    // resync is too tight and we are back to arrival stamping ---
    {
        AudioTimeline tl{};
        const uint64_t base = 200'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        const uint64_t t1 = audio_timeline_stamp(tl, kRate, kFrames, base + 3'000'000ULL);
        const uint64_t t2 = audio_timeline_stamp(tl, kRate, kFrames, base + 24'000'000ULL);
        check(t1 == base + k10ms && t2 == base + 2 * k10ms,
              "ordinary arrival jitter triggered a resync -- the threshold is too "
              "tight and the clock has degenerated back into arrival stamping");
    }

    // --- Burst drains must not trip the backward clamp. Draining a backlog
    // stamps several slots against one frozen arrival; by slot 6 the derived
    // time reads 60ms "ahead" and the backward resync would hand OBS a
    // backwards timestamp jump mid-recovery. allow_backward_resync=false is
    // what the drain loops pass whenever a wakeup found more than one buffer.
    {
        AudioTimeline tl{};
        const uint64_t base = 300'000'000'000ULL;
        audio_timeline_stamp(tl, kRate, kFrames, base);
        uint64_t prev = base;
        bool monotonic = true;
        // The burst itself: 7 more slots against one frozen arrival.
        for (int i = 1; i <= 7; ++i) {
            const uint64_t ts = audio_timeline_stamp(tl, kRate, kFrames, base);
            if (ts < prev) monotonic = false;
            prev = ts;
        }
        check(monotonic && prev == base + 7 * k10ms,
              "a full-ring burst against a frozen arrival re-anchored "
              "backwards mid-burst");
        // THE CASE THE FIRST FIX MISSED (review-reproduced): the wakeup AFTER
        // the burst. The timeline still reads ~70ms ahead of wall clock; a
        // symmetric 50ms clamp re-anchored backwards right here, handing OBS
        // one reversed timestamp. The burst allowance in the backward limit is
        // what this pins.
        const uint64_t after = audio_timeline_stamp(tl, kRate, kFrames,
                                                    base + 8 * k10ms + 500'000ULL);
        check(after >= prev,
              "the first wakeup AFTER a burst re-anchored backwards -- the "
              "backward limit must absorb one full ring of drain-ahead");
        // And the allowance must be a BOUNDED grace, not a license to run
        // ahead forever: stamping indefinitely against a frozen arrival, the
        // derived time may lead arrival by at most forward-limit + one ring
        // (the clamp re-anchors mid-stream whenever it would exceed that).
        AudioTimeline tl2{};
        const uint64_t t2 = 400'000'000'000ULL;
        audio_timeline_stamp(tl2, kRate, kFrames, t2);
        bool bounded = true;
        for (int i = 0; i < 40; ++i) {                     // 400ms of content
            const uint64_t ts = audio_timeline_stamp(tl2, kRate, kFrames, t2);
            if (ts > t2 + kAudioTimelineMaxDriftNs +
                         kAudioTimelineBurstAllowanceNs)
                bounded = false;
        }
        check(bounded,
              "derived time ran ahead of a frozen arrival past "
              "forward-limit-plus-one-ring without re-anchoring -- the burst "
              "allowance must bound run-ahead, not license it");
    }

    if (failures == 0)
        std::cout << "audio-timeline: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
