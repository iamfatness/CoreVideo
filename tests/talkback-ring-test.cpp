// tests/talkback-ring-test.cpp
// The talkback ring, driven in REVERSE: the plugin writes, the engine reads.
//
// Every media path in this codebase runs engine -> plugin. Talkback is the
// first that runs the other way, so the ring's invariants are exercised here
// with the roles swapped: free-running indices, per-slot seqlock, and the
// edge-triggered notify protocol. The invariants are direction-agnostic by
// construction (the helpers are free functions over a header pointer) and
// this test is what keeps that true.
#include "engine-ipc.h"
#include "talkback-ring.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Collected {
    std::vector<std::vector<uint8_t>> buffers;
};

static void collect(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<Collected *>(ctx);
    const auto *p = static_cast<const uint8_t *>(pcm);
    c->buffers.emplace_back(p, p + byte_len);
}

int main()
{
    // A plain heap buffer stands in for the mapped region: the ring logic
    // does not care how the memory was obtained, and this keeps the test
    // free of platform shared-memory calls.
    std::vector<uint8_t> region(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
    auto *hdr = reinterpret_cast<ShmAudioHeader *>(region.data());
    check(talkback_ring_init(hdr, 48000, 1), "talkback_ring_init failed");
    check(hdr->slot_count == kAudioRingSlots, "slot_count was not kAudioRingSlots");
    check(hdr->slot_bytes == kTalkbackSlotBytes, "slot_bytes was not kTalkbackSlotBytes");
    check(hdr->sample_rate == 48000, "sample_rate was not stored");
    check(hdr->channels == 1, "channels was not stored");
    check(hdr->write_index == 0, "write_index did not start at 0");

    // ── First publish crosses the empty -> non-empty edge ──────────────────
    std::vector<uint8_t> a(960, 0xAB);
    check(talkback_ring_publish(region.data(), a.data(), 960, 111) == true,
          "the first publish did not request a notify");
    check(hdr->write_index == 1, "write_index did not advance");

    // ── A second publish with the flag still set must NOT re-notify ────────
    std::vector<uint8_t> b(960, 0xCD);
    check(talkback_ring_publish(region.data(), b.data(), 960, 222) == false,
          "a publish re-notified while the flag was already set");
    check(hdr->write_index == 2, "write_index did not advance on the second publish");

    // ── The reader drains BOTH, in order ───────────────────────────────────
    uint32_t read_index = 0;
    Collected got;
    const uint32_t n = talkback_ring_drain(region.data(), read_index, collect, &got);
    check(n == 2, "drain did not return 2 buffers");
    check(read_index == 2, "read_index did not advance to write_index");
    check(got.buffers.size() == 2, "drain did not deliver 2 buffers");
    check(got.buffers[0][0] == 0xAB, "first buffer was not the first published");
    check(got.buffers[1][0] == 0xCD, "second buffer was out of order");

    // ── After a full drain the reader may sleep ────────────────────────────
    check(audio_ring_reader_done(hdr, read_index) == true,
          "reader_done said not-empty after a full drain");
    check(hdr->notify == 0, "notify was left set after a clean drain");

    // ── ...and the next publish notifies again ─────────────────────────────
    check(talkback_ring_publish(region.data(), a.data(), 960, 333) == true,
          "publish after a clean drain did not re-notify");

    // ── Oversized payloads are refused, not truncated ──────────────────────
    std::vector<uint8_t> huge(kTalkbackSlotBytes + 1, 0xEE);
    const uint32_t before = hdr->write_index;
    check(talkback_ring_publish(region.data(), huge.data(),
                                kTalkbackSlotBytes + 1, 444) == false,
          "an oversized publish claimed it notified");
    check(hdr->write_index == before,
          "an oversized publish advanced write_index -- it must be refused, "
          "never truncated: a half buffer is heard as a click");

    // ── Overrun: the writer lapping the reader is DETECTED, not silent ─────
    {
        std::vector<uint8_t> r2(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
        auto *h2 = reinterpret_cast<ShmAudioHeader *>(r2.data());
        talkback_ring_init(h2, 48000, 1);
        for (uint32_t i = 0; i < kAudioRingSlots + 3; ++i)
            talkback_ring_publish(r2.data(), a.data(), 960, i);
        // Reader never drained: it is exactly slot_count+3 behind.
        check(audio_ring_slots_behind(h2->write_index, 0, h2->slot_count) ==
                  kAudioRingSlots + 3,
              "slots_behind did not report the true overrun depth -- collapsing "
              "'caught up' and 'lapped by one ring' was the original defect");
    }

    // ── Concurrent writer + reader: no torn buffer ever escapes ────────────
    {
        std::vector<uint8_t> r3(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
        auto *h3 = reinterpret_cast<ShmAudioHeader *>(r3.data());
        talkback_ring_init(h3, 48000, 1);

        std::atomic<bool> stop{false};
        constexpr uint32_t kLen = 960;
        std::thread writer([&]() {
            for (uint32_t i = 0; i < 5000 && !stop.load(); ++i) {
                std::vector<uint8_t> buf(kLen, static_cast<uint8_t>(i & 0xFF));
                talkback_ring_publish(r3.data(), buf.data(), kLen, i);
            }
        });

        uint32_t ri = 0;
        bool torn = false;
        auto verify = [](const void *pcm, uint32_t len, uint64_t, void *ctx) {
            const auto *p = static_cast<const uint8_t *>(pcm);
            bool *bad = static_cast<bool *>(ctx);
            for (uint32_t i = 1; i < len; ++i)
                if (p[i] != p[0]) { *bad = true; return; }
        };
        for (int pass = 0; pass < 2000; ++pass)
            talkback_ring_drain(r3.data(), ri, verify, &torn);
        stop.store(true);
        writer.join();
        talkback_ring_drain(r3.data(), ri, verify, &torn);
        check(!torn, "a torn buffer escaped the seqlock under concurrency");
    }

    if (failures == 0)
        std::cout << "talkback-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
