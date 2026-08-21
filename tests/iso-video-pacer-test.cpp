// tests/iso-video-pacer-test.cpp
// Pacing ISO video frames to a fixed cadence before they reach ffmpeg's raw
// pipe, instead of trusting ffmpeg to derive timing it cannot actually get
// from a raw byte stream. See src/iso-video-pacer.h for the full account,
// including why -use_wallclock_as_timestamps (the first fix attempt) does
// not work with this project's ffmpeg build's rawvideo demuxer, confirmed
// live with ffprobe -show_frames on 2026-08-21.
#include "iso-video-pacer.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // --- The very first real frame lands exactly on its own due slot ---
    {
        uint64_t next_due = 0; // session start, per ensure_session_locked
        const uint32_t due = iso_video_frames_due(next_due, 0);
        check(due == 1,
              "the first frame at the session's own start time was not "
              "counted as due -- every ISO recording would start one "
              "frame short");
        check(next_due == kIsoVideoFramePeriodNs,
              "next_due_ns did not advance by exactly one period after the "
              "first frame");
    }

    // --- Ordinary cadence: a frame landing near its slot emits once ---
    {
        uint64_t next_due = kIsoVideoFramePeriodNs;
        const uint32_t due =
            iso_video_frames_due(next_due, kIsoVideoFramePeriodNs + 500'000);
        check(due == 1,
              "a frame arriving just after its own due slot was not "
              "counted as exactly one due frame");
    }

    // --- Fast burst (e.g. 60fps against a 30fps target): frames landing
    // before their slot is due are dropped, not duplicated ---
    {
        uint64_t next_due = kIsoVideoFramePeriodNs; // one period from start
        const uint64_t half_period = kIsoVideoFramePeriodNs / 2;
        const uint32_t due = iso_video_frames_due(next_due, half_period);
        check(due == 0,
              "a frame arriving before its slot was due was still counted "
              "-- a 60fps source against a 30fps target would double the "
              "output rate instead of being downsampled to it");
        check(next_due == kIsoVideoFramePeriodNs,
              "a dropped (too-early) frame advanced the schedule anyway");
    }

    // --- Stalled source (e.g. 10fps against a 30fps target): a late frame
    // backfills every slot that elapsed while nothing arrived ---
    {
        uint64_t next_due = 0;
        // First frame establishes the schedule and consumes slot 0.
        iso_video_frames_due(next_due, 0);
        // Next real frame doesn't arrive until slot 4's own due time --
        // slots 1, 2, 3 elapsed with nothing arriving, and slot 4 is due
        // too (landing exactly on a boundary is inclusive), so 4 frames
        // backfill this one arrival.
        const uint64_t stall_arrival = 4 * kIsoVideoFramePeriodNs;
        const uint32_t due = iso_video_frames_due(next_due, stall_arrival);
        check(due == 4,
              "a stall spanning slots 1-4 did not backfill exactly 4 "
              "duplicate frames -- the recording would run short by the "
              "stall's duration, the exact defect this pacer exists to fix");
        check(next_due == 5 * kIsoVideoFramePeriodNs,
              "the schedule did not land back on the correct absolute "
              "slot after backfilling a stall");
    }

    // --- Anomalous stall past the cap: bounded catch-up, then resync ---
    {
        uint64_t next_due = 0;
        iso_video_frames_due(next_due, 0);
        const uint64_t huge_gap =
            (kIsoVideoMaxCatchUpFrames + 50) * kIsoVideoFramePeriodNs;
        const uint32_t due = iso_video_frames_due(next_due, huge_gap);
        check(due == kIsoVideoMaxCatchUpFrames,
              "a gap far past the cap was not clamped to "
              "kIsoVideoMaxCatchUpFrames -- one bad timestamp could queue "
              "an unbounded burst of duplicate frames");
        check(next_due > huge_gap,
              "the schedule was not resynced past the anomalous gap -- the "
              "very next ordinary frame would immediately trigger another "
              "capped catch-up burst");
    }

    // --- Clock went backwards: never rewind the schedule, never emit ---
    {
        uint64_t next_due = 10 * kIsoVideoFramePeriodNs;
        const uint32_t due = iso_video_frames_due(next_due, 0);
        check(due == 0,
              "a timestamp earlier than the last scheduled slot produced "
              "nonzero due frames");
        check(next_due == 10 * kIsoVideoFramePeriodNs,
              "a backwards timestamp moved the schedule");
    }

    // --- Long-run accuracy: total emitted frames over many arrivals tracks
    // real elapsed time within one frame period, regardless of the
    // arrival pattern's own rate ---
    {
        uint64_t next_due = 0;
        uint64_t now = 0;
        uint64_t total_due = 0;
        // 200 arrivals at an irregular ~16fps-equivalent mean interval,
        // deliberately not a multiple of the 30fps period.
        const uint64_t interval_ns = 62'500'000ULL; // 1/16 s
        for (int i = 0; i < 200; ++i) {
            now += interval_ns;
            total_due += iso_video_frames_due(next_due, now);
        }
        const uint64_t expected_frames =
            now / kIsoVideoFramePeriodNs; // within rounding
        const uint64_t diff =
            total_due > expected_frames ? total_due - expected_frames
                                        : expected_frames - total_due;
        check(diff <= 1,
              "total paced frame count drifted from real elapsed time by "
              "more than one frame period over a long irregular run -- "
              "the whole point of pacing is a duration that matches wall "
              "clock");
    }

    if (failures == 0)
        std::cout << "iso-video-pacer: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
