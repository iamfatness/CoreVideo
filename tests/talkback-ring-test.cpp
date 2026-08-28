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

// True when [p, p + len) overlaps [region_base, region_base + region_bytes).
// A deterministic, timing-independent proof that a payload pointer handed to
// a drain callback is NOT a view into shared memory: the review that found
// the seqlock defect this file guards against also noted that a torn-buffer
// race is a few instructions wide, and 5000 unthrottled writer iterations
// against 2000 drain passes may simply never land in that window -- a
// concurrency test that passes because the timing never collided is not
// evidence. This check needs no collision: it is true or false on every
// single call, always.
static bool aliases_region(const void *p, uint32_t len, const void *region_base,
                           size_t region_bytes)
{
    const auto *pb = static_cast<const uint8_t *>(p);
    const auto *rb = static_cast<const uint8_t *>(region_base);
    return pb < rb + region_bytes && rb < pb + len;
}

struct Collected {
    std::vector<std::vector<uint8_t>> buffers;
    const void *region_base = nullptr;
    size_t region_bytes = 0;
    bool aliased_shared_memory = false;
};

static void collect(const void *pcm, uint32_t byte_len, uint64_t, void *ctx)
{
    auto *c = static_cast<Collected *>(ctx);
    const auto *p = static_cast<const uint8_t *>(pcm);
    if (c->region_base != nullptr &&
        aliases_region(pcm, byte_len, c->region_base, c->region_bytes))
        c->aliased_shared_memory = true;
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
    got.region_base = region.data();
    got.region_bytes = region.size();
    const uint32_t n = talkback_ring_drain(region.data(), read_index, collect, &got);
    check(n == 2, "drain did not return 2 buffers");
    check(read_index == 2, "read_index did not advance to write_index");
    check(got.buffers.size() == 2, "drain did not deliver 2 buffers");
    check(got.buffers[0][0] == 0xAB, "first buffer was not the first published");
    check(got.buffers[1][0] == 0xCD, "second buffer was out of order");
    // ── Deterministic proof the callback got a COPY, not a shared pointer ──
    check(!got.aliased_shared_memory,
          "drain handed fn a pointer INTO the shared region -- the seqlock "
          "recheck must happen around a private copy, not around the "
          "caller's read of shared memory");

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

        // ...and DRAINING that lapped ring must ACCOUNT for what it steps
        // over. The skip-forward used to be silent: `*lost` counted only
        // seqlock give-ups, so the larger and likelier loss -- the writer
        // overwriting buffers before the reader arrived -- was the one kind
        // of dropped audio nothing reported, while the code's own comment
        // said the caller reports it. 3 published buffers were overwritten
        // (slot_count + 3 published, slot_count still intact).
        Collected got2;
        got2.region_base = r2.data();
        got2.region_bytes = r2.size();
        uint32_t lost2 = 0;
        uint32_t ri2 = 0;
        const uint32_t delivered2 =
            talkback_ring_drain(r2.data(), ri2, collect, &got2, &lost2);
        check(lost2 == 3,
              "draining a lapped ring did not count the buffers the skip stepped "
              "over -- audio the director spoke and nobody heard, reported as "
              "nothing at all");
        check(delivered2 == kAudioRingSlots,
              "a lapped drain did not deliver exactly the slots still intact");
    }

    // ── A slot that fails every seqlock attempt increments *lost, silently
    //    dropping a buffer must never be free ──────────────────────────────
    {
        std::vector<uint8_t> r4(shm_audio_region_bytes(kTalkbackSlotBytes), 0);
        auto *h4 = reinterpret_cast<ShmAudioHeader *>(r4.data());
        talkback_ring_init(h4, 48000, 1);
        check(talkback_ring_publish(r4.data(), a.data(), 960, 555) == true,
              "setup publish for the lost-slot test failed to notify");

        // No concurrent writer here -- this is deterministic, not a timing
        // race. Reach into the one published slot and leave its sequence
        // ODD, exactly what a writer caught mid-publish would leave behind.
        // Every one of the reader's 3 attempts must then see "write in
        // progress" and give up on this slot.
        auto *slot0 = reinterpret_cast<ShmAudioSlot *>(
            r4.data() + shm_audio_slot_offset(*h4, 0));
        slot0->sequence |= 1u;

        uint32_t ri4 = 0;
        uint32_t lost = 0;
        Collected got4;
        const uint32_t delivered =
            talkback_ring_drain(r4.data(), ri4, collect, &got4, &lost);
        check(delivered == 0, "a torn slot was reported as delivered");
        check(lost == 1, "a slot that failed every seqlock attempt did not "
                          "increment *lost -- that is the exact silent-loss "
                          "class this codebase has already been burned by");
        check(ri4 == 1, "read_index did not advance past the lost slot");
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

        struct VerifyCtx {
            bool torn = false;
            bool aliased = false;
            const void *region_base = nullptr;
            size_t region_bytes = 0;
        };
        VerifyCtx vctx;
        vctx.region_base = r3.data();
        vctx.region_bytes = r3.size();

        uint32_t ri = 0;
        auto verify = [](const void *pcm, uint32_t len, uint64_t, void *ctx) {
            auto *v = static_cast<VerifyCtx *>(ctx);
            if (aliases_region(pcm, len, v->region_base, v->region_bytes))
                v->aliased = true;
            const auto *p = static_cast<const uint8_t *>(pcm);
            for (uint32_t i = 1; i < len; ++i)
                if (p[i] != p[0]) { v->torn = true; return; }
        };
        for (int pass = 0; pass < 2000; ++pass)
            talkback_ring_drain(r3.data(), ri, verify, &vctx);
        stop.store(true);
        writer.join();
        talkback_ring_drain(r3.data(), ri, verify, &vctx);
        check(!vctx.torn, "a torn buffer escaped the seqlock under concurrency");
        // Timing-dependent (may or may not exercise the race window) but
        // cheap to keep: the deterministic check above (Collected's
        // aliased_shared_memory) is what actually proves the fix, but this
        // one still has value -- it is the only check here that runs the
        // publish/drain path under real concurrent contention.
        check(!vctx.aliased,
              "drain handed fn a pointer INTO the shared region under "
              "concurrency");
    }

    if (failures == 0)
        std::cout << "talkback-ring: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
