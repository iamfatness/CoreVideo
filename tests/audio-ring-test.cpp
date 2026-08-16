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

    // --- The indices wrap. A reader near the top of the ring and a writer that
    // has wrapped past zero is the normal steady state, not an error ---
    check(audio_ring_slots_behind(1, kAudioRingSlots - 1, kAudioRingSlots) == 2,
          "wrapped indices were miscounted -- this is the steady state, not an "
          "edge case");

    // --- Exactly full: the writer has lapped the reader by the whole ring.
    // Every slot is unread and none is lost YET ---
    check(audio_ring_slots_behind(0, 0, kAudioRingSlots) == 0,
          "an equal pair must read as caught up, not as a full lap");

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

    if (failures == 0)
        std::cout << "audio-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
