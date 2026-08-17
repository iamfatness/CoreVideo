// tests/audio-ring-test.cpp
// The index arithmetic of the audio shared-memory ring.
//
// The defect this exists for (2026-08-16, live show): the audio region was a
// SINGLE slot. engine-audio.cpp memcpy'd every Zoom buffer over the previous
// one, guarded only by a seqlock. A seqlock stops the reader seeing a TORN
// buffer; it does nothing about LOSS. Zoom delivers ~100 buffers/second, so any
// reader stall destroyed 10 ms of audio permanently and silently -- on a box
// already at 70% CPU with 10 sources, which is to say routinely.
//
// A ring lets the writer run ahead without destroying unread audio, and makes
// the loss that does happen countable instead of invisible.

#include "engine-ipc.h"
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

int main()
{
    // --- A reader level with the writer has nothing to do ---
    check(audio_ring_slots_behind(5, 5, kAudioRingSlots) == 0,
          "a caught-up reader was told it had slots pending");

    // --- Ordinary case: three buffers written since the last drain ---
    check(audio_ring_slots_behind(5, 2, kAudioRingSlots) == 3,
          "a reader three slots behind was miscounted");

    // --- write_index and read_index are FREE-RUNNING counters: they never
    // wrap at slot_count, only at 2^32 (~1.4 years at Zoom's ~100 buffers/sec).
    // Unsigned subtraction stays correct across that wrap with no special
    // casing -- this is the case that actually exercises it ---
    check(audio_ring_slots_behind(2, 0xFFFFFFFEu, kAudioRingSlots) == 4,
          "free-running counter wraparound at 2^32 was miscounted");

    // --- Exactly full: the writer has lapped the reader by the whole ring.
    // Every slot is STILL INTACT (none overwritten yet), so this must read as
    // slot_count pending, not as caught up. Collapsing the two was the actual
    // defect: under the old slot_count-modulo arithmetic, a reader stalled
    // for exactly one full lap (8 buffers, 80ms) computed the same value as a
    // reader that was perfectly caught up, published nothing, and reported no
    // loss ---
    check(audio_ring_slots_behind(kAudioRingSlots, 0, kAudioRingSlots) ==
              kAudioRingSlots,
          "an exactly-full ring must read as slot_count pending, not as "
          "caught up -- collapsing the two hides a full lap of silent loss");

    // --- Region sizing must account for header + every slot ---
    {
        const uint32_t slot_bytes = 1920;   // 480 frames, 16-bit stereo
        const size_t total = shm_audio_region_bytes(slot_bytes);
        const size_t expected = sizeof(ShmAudioHeader) +
            static_cast<size_t>(kAudioRingSlots) *
            (sizeof(ShmAudioSlot) + slot_bytes);
        check(total == expected,
              "region sizing does not cover header plus every slot -- a short "
              "region means the last slot writes out of bounds");
    }

    // --- Slot offsets must be distinct, ordered, and inside the region ---
    {
        ShmAudioHeader h{};
        h.slot_count = kAudioRingSlots;
        h.slot_bytes = 1920;
        const size_t total = shm_audio_region_bytes(h.slot_bytes);
        size_t previous = 0;
        for (uint32_t i = 0; i < kAudioRingSlots; ++i) {
            const size_t off = shm_audio_slot_offset(h, i);
            check(off >= sizeof(ShmAudioHeader),
                  "a slot offset landed inside the header");
            check(off + sizeof(ShmAudioSlot) + h.slot_bytes <= total,
                  "a slot ran past the end of the region");
            if (i > 0)
                check(off > previous, "slot offsets are not strictly ordered");
            previous = off;
        }
    }

    // --- The wire format must not have grown: participant_id reuses the old
    // reserved word, and the notify flag reuses the header's. A size change
    // here is a silent engine/plugin mismatch (see the version guard) ---
    {
        // 24, not 20: capture_ns forces 8-byte alignment, so there have
        // always been 4 padding bytes after sequence. What matters is that
        // the size is UNCHANGED from the v0.1.40 wire format -- renaming
        // reserved to participant_id must not move anything.
        check(sizeof(ShmAudioSlot) == 24,
              "ShmAudioSlot changed size -- participant_id was meant to reuse "
              "the reserved word, not extend the struct");
        check(sizeof(ShmAudioHeader) == 4 * 4 + 2 + 2,
              "ShmAudioHeader changed size -- notify was meant to reuse the "
              "reserved u16, not extend the struct");
        ShmAudioSlot slot{};
        slot.participant_id = 16788480u;
        check(slot.participant_id == 16788480u,
              "slot participant_id does not round-trip");
    }

    // --- Notify-protocol liveness. The first shipped version of the edge
    // trigger cleared the flag only at the drain-loop exit; the first-event
    // leveling path returned early, left the flag set, and every source went
    // silent after ONE buffer -- caught in review, not by these tests,
    // because no test exercised the protocol at all. These do. (Single
    // threaded, so they pin the STATE MACHINE, not the memory ordering; the
    // fence-pair proof lives with ShmAudioHeader::notify.)
    {
        ShmAudioHeader h{};
        h.slot_count = kAudioRingSlots;
        uint32_t read_index = 0;

        // First publish crosses the empty->non-empty edge: one event.
        h.write_index = 1;
        check(audio_ring_notify_after_publish(&h),
              "the first publish after idle must send an event");
        // Further publishes while the reader has not drained: silent.
        h.write_index = 2;
        check(!audio_ring_notify_after_publish(&h),
              "a second publish before the reader drained must NOT send -- "
              "this is the whole point of edge triggering");

        // THE WEDGE REGRESSION: reader wakes, levels (or drains), and runs
        // reader_done. If it were to skip that (the old early return), the
        // flag would stay set and the writer above would never notify again.
        read_index = 2;                     // drained/levelled to the writer
        check(audio_ring_reader_done(&h, read_index),
              "a reader level with the writer must be told it is safe to sleep");
        h.write_index = 3;
        check(audio_ring_notify_after_publish(&h),
              "after the reader completed, the next publish must send a fresh "
              "event -- if this fails, sources go silent after one buffer");

        // Race window: data lands between the drain and the clear. reader_done
        // must demand another pass and suppress redundant events meanwhile.
        h.write_index = 5;
        check(!audio_ring_reader_done(&h, 3),
              "reader_done must demand another pass when data landed in the "
              "race window");
        check(!audio_ring_notify_after_publish(&h),
              "no event needed while the reader has reclaimed the flag");
        check(audio_ring_reader_done(&h, 5),
              "the second pass reaches empty and may sleep");

        // Abandon: any consumed wakeup that cannot drain must hand the flag
        // back so the next publish re-notifies.
        h.write_index = 6;
        check(audio_ring_notify_after_publish(&h), "edge after sleep");
        audio_ring_reader_abandon(&h);
        h.write_index = 7;
        check(audio_ring_notify_after_publish(&h),
              "after an abandon the next publish must re-notify -- a wakeup "
              "consumed without clearing the flag is a permanent mute");
    }

    // --- The timeline's burst allowance must track the ring's capacity. It is
    // a literal in audio-timeline.h (to keep that header platform-free), so
    // this is the assertion that keeps the two in step if kAudioRingSlots ever
    // changes ---
    check(kAudioTimelineBurstAllowanceNs ==
              static_cast<uint64_t>(kAudioRingSlots) * 10'000'000ULL,
          "kAudioTimelineBurstAllowanceNs no longer equals kAudioRingSlots x "
          "10ms -- the backward drift clamp will misfire (too tight) or mask "
          "real errors (too loose) after a ring-depth change");

    // --- Ghost-writer detection: a "create" that lands on a name some other
    // handle still holds must say so. This is the only signal the engine has
    // that an orphaned predecessor is sharing (and possibly still writing) its
    // ring -- the 2026-08-17 live defect where a ghost's notify=1 with a dead
    // pipe suppressed every edge event and audio degraded to the 2.5s
    // keepalive. Two regions in one process stand in for two processes here:
    // the named section/shm object is process-agnostic either way ---
    {
        ShmRegion first{}, second{};
        const size_t bytes = shm_audio_region_bytes(64);
        check(shm_region_create(first, "cv_ring_test_collision", bytes),
              "fresh create failed");
        check(!first.already_existed,
              "a fresh create must not report a collision");
        check(shm_region_create(second, "cv_ring_test_collision", bytes),
              "create over a held name failed outright -- it must open and "
              "flag, not fail");
        check(second.already_existed,
              "a create that opened a section another handle still holds must "
              "set already_existed -- silent sharing is the ghost-writer bug");
        shm_region_destroy(second);
        check(!second.already_existed,
              "destroy must reset already_existed");
        shm_region_destroy(first);
    }

    if (failures == 0)
        std::cout << "audio-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
