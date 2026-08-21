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
    // --- No gap: back-to-back buffers at 10ms need no silence ---
    check(iso_audio_silence_frames(0, 10'000'000ULL, 48000) == 480,
          "the very first buffer (last_write_ns=0, i.e. session start) did "
          "not backfill from session start to its own arrival");

    check(iso_audio_silence_frames(1000 * kSec, 1000 * kSec + 10'000'000ULL,
                                   48000) == 480,
          "a normal 10ms gap between consecutive real buffers, far from "
          "session start, was not sized to exactly 10ms of frames");

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

    if (failures == 0)
        std::cout << "iso-audio-gap-fill: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
