// tests/iso-audio-gap-fill-test.cpp
// Silence backfill that keeps an ISO WAV's own timeline tracking real
// elapsed time instead of shrinking by every silent stretch.
//
// The defect this guards (2026-08-21 design review): Zoom only calls back
// one-way audio for a currently-talking participant, so a session quiet for
// the first 45s of its life wrote real speech starting at WAV byte 0, not
// the 45s mark -- the file drifted out of sync with its own paired video
// (and with real elapsed time) by exactly the total silent duration. See
// src/iso-audio-gap-fill.h for the full account.
#include "iso-audio-gap-fill.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static constexpr uint64_t kSec = 1'000'000'000ULL;

int main()
{
    // --- Within jitter tolerance: no backfill (this is the gate, not a
    // real gap -- see kIsoAudioJitterToleranceNs) ---
    check(iso_audio_silence_frames(0, 10'000'000ULL, 48000) == 0,
          "a 10ms gap -- well inside ordinary dispatch jitter -- was "
          "backfilled anyway; every buffer this small would falsely "
          "register as silence");
    check(iso_audio_silence_frames(1000 * kSec, 1000 * kSec + 10'000'000ULL,
                                   48000) == 0,
          "a normal 10ms gap between consecutive real buffers, far from "
          "session start, was backfilled despite being within tolerance");

    // --- Just past the tolerance boundary: backfilled, sized correctly ---
    {
        const uint64_t gap_ns = kIsoAudioJitterToleranceNs + 20'000'000ULL; // +20ms
        const uint64_t got = iso_audio_silence_frames(0, gap_ns, 48000);
        check(got == (gap_ns * 48000ULL) / 1'000'000'000ULL,
              "a 70ms gap, clearly past the 50ms jitter tolerance, was not "
              "sized to the full real gap once it did trigger backfill");
    }

    // --- Exactly at the tolerance boundary: NOT backfilled (inclusive
    // tolerance -- the boundary itself still reads as ordinary jitter) ---
    check(iso_audio_silence_frames(0, kIsoAudioJitterToleranceNs, 48000) == 0,
          "a gap exactly at the jitter tolerance was backfilled -- the "
          "boundary itself should still read as ordinary jitter");

    // --- An ordinary meeting silence: real duration is restored exactly ---
    {
        const uint64_t got = iso_audio_silence_frames(0, 45 * kSec, 48000);
        check(got == 45ULL * 48000ULL,
              "a 45s silent stretch was not backfilled to exactly 45s of "
              "frames at 48kHz -- the file would still drift from real time");
    }

    // --- Sample-rate correctness: the same gap at a different rate scales ---
    {
        const uint64_t got = iso_audio_silence_frames(0, 45 * kSec, 16000);
        check(got == 45ULL * 16000ULL,
              "backfill did not scale with sample_rate -- would over- or "
              "under-fill at any rate other than the one it was tuned for");
    }

    // --- Anomalous gap: capped, not backfilled (a debugger pause / sleep
    // must not turn into an hours-long silence file) ---
    {
        const uint64_t got =
            iso_audio_silence_frames(0, kIsoAudioMaxBackfillNs + kSec, 48000);
        check(got == 0,
              "a gap past the cap was backfilled anyway -- one bad "
              "timestamp could balloon into a multi-hour silence file");
    }

    // --- Exactly at the cap: still allowed (the cap is inclusive) ---
    {
        const uint64_t got =
            iso_audio_silence_frames(0, kIsoAudioMaxBackfillNs, 48000);
        check(got == (kIsoAudioMaxBackfillNs / 1'000'000'000ULL) * 48000ULL,
              "a gap exactly at the cap was rejected -- the cap should be "
              "inclusive of genuinely long but real meeting silences");
    }

    // --- Clock went backwards: never write negative time ---
    check(iso_audio_silence_frames(100 * kSec, 50 * kSec, 48000) == 0,
          "a backwards timestamp produced nonzero silence -- frames must "
          "never be computed from negative elapsed time");
    check(iso_audio_silence_frames(100 * kSec, 100 * kSec, 48000) == 0,
          "an exactly-zero gap produced nonzero silence");

    // --- Degenerate sample rate never divides by zero ---
    check(iso_audio_silence_frames(0, 10 * kSec, 0) == 0,
          "sample_rate=0 was not treated as zero silence (would divide by "
          "zero without this guard)");

    // --- Regression: the reference point MUST advance by content duration,
    // not snap to arrival time. Live defect (2026-08-21, not a flaw in
    // this pure function itself, but in how a caller could misuse it): a
    // caller that resets its reference to the raw arrival timestamp after
    // every write cannot "bank" being ahead of schedule -- every delayed
    // wakeup gets backfilled in full even when a burst right after it
    // (this codebase's own documented coalescing dispatch-lane behavior,
    // CLAUDE.md's "media events are prompts, not payloads") delivers all
    // the real content that delay was covering. Modeled here as a delay
    // clearly past the jitter tolerance (150ms, so this exercises the two
    // real strategies, not the tolerance gate itself) immediately followed
    // by a matching catch-up call that brings arrival back to the nominal
    // schedule -- net zero drift over each pair, the same as a delayed
    // wakeup whose backlog arrives in the next one. Content-driven
    // advancement backfills the first delay once, then -- having banked
    // that content time -- needs nothing further from the catch-up calls;
    // arrival-snapping re-backfills the same delay on every single cycle,
    // because snapping to arrival throws the bank away each time. Over a
    // whole session that is exactly how a live 152s video paired a 305s
    // WAV, almost exactly double.
    {
        const uint32_t sample_rate = 48000;
        const uint32_t frames_per_buffer = 480; // 10ms at 48kHz
        const uint64_t nominal_interval_ns = 10'000'000ULL; // 10ms
        const int64_t delay_ns = 150'000'000LL;             // 150ms, > 50ms
        const uint64_t buffer_duration_ns =
            (static_cast<uint64_t>(frames_per_buffer) * 1'000'000'000ULL) /
            sample_rate;
        uint64_t arrival_ns = 0;
        uint64_t ref_wrong = 0; // snaps to arrival (the live bug)
        uint64_t ref_right = 0; // advances by content (the fix)
        uint64_t total_silence_wrong = 0;
        uint64_t total_silence_right = 0;
        const int kPairs = 250; // 500 calls total
        for (int i = 0; i < kPairs; ++i) {
            // Delayed wakeup.
            arrival_ns = static_cast<uint64_t>(
                static_cast<int64_t>(arrival_ns + nominal_interval_ns) +
                delay_ns);
            total_silence_wrong +=
                iso_audio_silence_frames(ref_wrong, arrival_ns, sample_rate);
            ref_wrong = arrival_ns; // THE BUG: snap to raw arrival time
            uint64_t silence_frames =
                iso_audio_silence_frames(ref_right, arrival_ns, sample_rate);
            total_silence_right += silence_frames;
            ref_right += (silence_frames * 1'000'000'000ULL) / sample_rate +
                        buffer_duration_ns;

            // The catch-up: the delay's backlog arrives right after, back
            // on the nominal schedule -- net zero drift for the pair.
            arrival_ns = static_cast<uint64_t>(
                static_cast<int64_t>(arrival_ns + nominal_interval_ns) -
                delay_ns);
            total_silence_wrong +=
                iso_audio_silence_frames(ref_wrong, arrival_ns, sample_rate);
            ref_wrong = arrival_ns;
            silence_frames =
                iso_audio_silence_frames(ref_right, arrival_ns, sample_rate);
            total_silence_right += silence_frames;
            ref_right += (silence_frames * 1'000'000'000ULL) / sample_rate +
                        buffer_duration_ns;
        }
        check(total_silence_wrong > static_cast<uint64_t>(kPairs) *
                                        (delay_ns * sample_rate /
                                         1'000'000'000ULL) / 2,
              "the arrival-snapping pattern was expected to reproduce the "
              "live defect (re-backfilling the same delay every cycle) -- "
              "if this no longer holds, the reproduction itself is stale, "
              "not proof the bug is fixed");
        check(total_silence_right < total_silence_wrong / 20,
              "content-driven advancement backfilled nearly as much as "
              "naive arrival-snapping for a delay immediately followed by "
              "its own catch-up burst -- this is the exact live defect (a "
              "152s video paired a 305s WAV) happening again");
    }

    if (failures == 0)
        std::cout << "iso-audio-gap-fill: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
