#pragma once
//
// talkback-ring.h — the talkback ring's writer and reader halves.
//
// Talkback is the FIRST path in this codebase that moves media plugin ->
// engine. Every other one runs engine -> plugin. Rather than invent a second
// transport, this reuses ShmAudioHeader unchanged and swaps the roles: the
// plugin creates and writes, the engine opens and reads.
//
// That works because the ring's helpers in engine-ipc.h
// (audio_ring_notify_after_publish / audio_ring_reader_done /
// audio_ring_reader_abandon) are free functions over a header pointer with no
// baked-in direction, and shm_region_open_readwrite() already exists precisely
// because a READER must be able to clear the notify flag.
//
// The publish sequence below mirrors engine/src/engine-audio.cpp's, fence for
// fence. It is extracted here rather than retyped so the protocol has one
// implementation per direction instead of two that can drift.
//
// WHY NOT THE PIPE. Talking produces ~100 buffers/sec. This codebase has
// already measured what that shape does to the P2E/E2P pipes: engine->plugin
// latency of 58-90ms under full gallery load versus 41-161us idle, with ring
// overruns at zero throughout -- the ring never fell behind, the wakeups did.
// That incident is why the ring exists. Talkback is the one feature where
// that latency is heard directly, as a stutter in the director's voice.
//
#include "engine-ipc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

using TalkbackRingSlotFn = void (*)(const void *pcm, uint32_t byte_len,
                                    uint64_t capture_ns, void *ctx);

// Lay out a freshly created region. Writer side only, before any publish.
inline bool talkback_ring_init(ShmAudioHeader *hdr, uint32_t sample_rate,
                               uint16_t channels)
{
    if (hdr == nullptr) return false;
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->write_index = 0;
    hdr->slot_count  = kAudioRingSlots;
    hdr->slot_bytes  = kTalkbackSlotBytes;
    hdr->sample_rate = sample_rate;
    hdr->channels    = channels;
    hdr->notify      = 0;
    return true;
}

// Publish one buffer. Returns true exactly when this publish crossed the
// empty->non-empty edge and the caller must send ONE notify event.
//
// An oversized buffer is REFUSED (returns false, write_index untouched) rather
// than truncated: half a buffer is heard as a click, and a click in a
// director's ear reads as a fault in the transport.
inline bool talkback_ring_publish(void *region_base, const void *pcm,
                                  uint32_t byte_len, uint64_t capture_ns)
{
    if (region_base == nullptr || pcm == nullptr || byte_len == 0) return false;
    auto *hdr = static_cast<ShmAudioHeader *>(region_base);
    if (byte_len > hdr->slot_bytes) return false;

    const uint32_t index = hdr->write_index % hdr->slot_count;
    auto *slot = reinterpret_cast<ShmAudioSlot *>(
        static_cast<char *>(region_base) + shm_audio_slot_offset(*hdr, index));

    uint32_t seq = slot->sequence + 1;
    if ((seq & 1u) == 0) ++seq;
    slot->sequence = seq;                        // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    slot->capture_ns     = capture_ns;
    slot->byte_len       = byte_len;
    slot->participant_id = 0;                    // talkback has no single owner
    std::memcpy(reinterpret_cast<char *>(slot) + sizeof(ShmAudioSlot),
                pcm, byte_len);
    std::atomic_thread_fence(std::memory_order_release);
    slot->sequence = seq + 1;                    // even: readable

    std::atomic_thread_fence(std::memory_order_release);
    // FREE-RUNNING -- never % slot_count here. `index` above already applied
    // the modulo to pick the physical slot; write_index must keep counting so
    // the reader can tell "caught up" (0 behind) from "lapped by exactly one
    // ring" (slot_count behind). Collapsing those was the original defect.
    hdr->write_index = hdr->write_index + 1;

    return audio_ring_notify_after_publish(hdr);
}

// Drain everything published since `read_index`, calling `fn` per buffer in
// order. Advances `read_index`. Returns the number of buffers delivered.
// `lost`, when non-null, is incremented once per slot that failed every
// seqlock attempt -- see the retry loop below for why that must never be
// silent.
//
// EVENTS ARE PROMPTS, NOT PAYLOADS: one notify can cover many slots, so the
// reader must drain fully on any wakeup rather than consuming one buffer per
// event.
inline uint32_t talkback_ring_drain(void *region_base, uint32_t &read_index,
                                    TalkbackRingSlotFn fn, void *ctx,
                                    uint32_t *lost = nullptr)
{
    if (region_base == nullptr || fn == nullptr) return 0;
    auto *hdr = static_cast<ShmAudioHeader *>(region_base);

    uint32_t delivered = 0;
    for (;;) {
        const uint32_t write_index = hdr->write_index;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (read_index == write_index) break;

        // If the writer lapped us, skip forward to the oldest slot still
        // intact. Silent loss is the thing the free-running index exists to
        // make visible; the caller reports it.
        const uint32_t behind = audio_ring_slots_behind(write_index, read_index,
                                                        hdr->slot_count);
        if (behind > hdr->slot_count)
            read_index = write_index - hdr->slot_count;

        const uint32_t index = read_index % hdr->slot_count;
        auto *slot = reinterpret_cast<const ShmAudioSlot *>(
            static_cast<const char *>(region_base) +
            shm_audio_slot_offset(*hdr, index));

        // Per-slot seqlock, mirroring the read in src/zoom-source.cpp
        // (~line 1965-1990): the payload COPY happens INSIDE the
        // [s1, s2] window, and `fn` is only ever handed the COPY, never a
        // pointer into shared memory. A recheck that only re-validates
        // already-copied locals (byte_len/capture_ns) and then lets the
        // caller read the payload afterward protects nothing -- the actual
        // shared-memory read (fn's access) still happens outside the
        // validated window and can race a concurrent write, handing back a
        // torn buffer. `scratch` is sized for the largest possible slot;
        // `len` is clamped against both `hdr->slot_bytes` and
        // `kTalkbackSlotBytes` before the copy so a corrupted header can
        // never drive a stack overflow here.
        alignas(alignof(std::max_align_t)) uint8_t scratch[kTalkbackSlotBytes];
        uint32_t len = 0;
        uint64_t ns  = 0;
        bool copied = false;
        for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
            const uint32_t s1 = slot->sequence;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s1 & 1u) continue;
            len = slot->byte_len;
            ns  = slot->capture_ns;
            if (len == 0 || len > hdr->slot_bytes || len > kTalkbackSlotBytes)
                break;
            std::memcpy(scratch,
                        reinterpret_cast<const char *>(slot) + sizeof(ShmAudioSlot),
                        len);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t s2 = slot->sequence;
            if (s1 == s2 && (s2 & 1u) == 0) copied = true;
        }
        if (copied) {
            fn(scratch, len, ns, ctx);
            ++delivered;
        } else if (lost != nullptr) {
            // Three attempts, then give up on this slot rather than
            // spinning on the audio path -- but giving up is not silent:
            // the caller must be able to account for every lost slot, the
            // same discipline the free-running write_index exists to
            // enforce (see ShmAudioHeader::write_index in engine-ipc.h).
            ++*lost;
        }
        ++read_index;
    }
    return delivered;
}
