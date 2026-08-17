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

    if (failures == 0)
        std::cout << "audio-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
