// Pins the release-before-resubscribe rule in src/shm-resubscribe.h.
//
// This exists because of a live failure (2026-08-10): during a meeting,
// participants flashed bright garbage whenever the active speaker changed. The
// active-speaker "clean cut" re-pointed an already-subscribed source_uuid at a
// new participant while the plugin still held its read mapping of that uuid's
// SHM region. The engine destroys and rebuilds the SourceTarget on such a
// re-subscribe, and at the time the generation counter was a member of that
// struct, so it restarted at 1 — the legacy unsuffixed name, the very name we
// were still mapping.
//
// The engine no longer restarts the counter (src/shm-generation.h keeps it
// process-wide, per region base name; tests/shm-generation-test.cpp pins that).
// These tests deliberately keep exercising the same-name rebuild anyway: a
// plugin runs against whatever engine binary is installed beside it, including
// older ones, and on POSIX the engine grows a region in place under one name.
// The release is what makes the read side correct in all of those shapes.
//
// The tests below use real shared memory (same approach as
// shm-frame-reader-test.cpp) because the rule under test is a property of the
// operating system, not of our arithmetic: the reason the release is mandatory
// cannot be demonstrated with a mock.
//
// What is NOT covered here, stated plainly so nobody reads these as more than
// they are: nothing in this file touches the engine process, OBS, or
// ZoomSource. These tests prove the platform rule and the helper's
// postcondition. They do NOT pin the wiring — deleting a release call from any
// of its call sites in src/zoom-source.cpp would fail nothing here, because no
// test constructs a ZoomSource. Proving the wiring needs a live engine and a
// running OBS.

#include "shm-resubscribe.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kW      = 64;
constexpr uint32_t kH      = 32;
constexpr uint32_t kYLen   = kW * kH;
constexpr size_t   kBytes  = sizeof(ShmFrameHeader) + kYLen + kYLen / 2;

// The replacement participant streams at a higher resolution — the case the
// operating system will not let the engine satisfy while we hold the old
// mapping.
constexpr uint32_t kBigW     = kW * 2;
constexpr uint32_t kBigH     = kH * 2;
constexpr uint32_t kBigYLen  = kBigW * kBigH;
constexpr size_t   kBigBytes = sizeof(ShmFrameHeader) + kBigYLen + kBigYLen / 2;

// Generation 1 maps to the legacy unsuffixed region name, and it is what an
// engine that restarts its counter on every re-subscribe publishes forever.
// Holding the generation fixed at 1 across the rebuild below is what makes
// these tests model that engine: the read side is handed the same generation it
// already had, which is exactly why generation comparison alone cannot rescue
// us and the release has to be explicit.
constexpr uint32_t kGen1 = 1;

std::string unique_region_name()
{
    static int counter = 0;
    return std::string(IPC_SHM_PREFIX) + "resub_test_" + std::to_string(++counter);
}

void publish(ShmRegion &writer, uint32_t w, uint32_t h, uint32_t y_len,
             uint8_t fill)
{
    auto *hdr = static_cast<ShmFrameHeader *>(writer.ptr);
    hdr->sequence += 1;  // odd: writing
    hdr->width  = w;
    hdr->height = h;
    hdr->y_len  = y_len;
    std::memset(static_cast<uint8_t *>(writer.ptr) + sizeof(ShmFrameHeader),
                fill, y_len + y_len / 2);
    hdr->sequence += 1;  // even: complete
}

// The helper's postcondition, stated directly: no mapping, no remembered
// generation. The generation reset is what keeps "mapped_gen == 0 means nothing
// is mapped" true for the reopen logging in zoom-source.cpp.
bool test_release_clears_mapping_and_generation()
{
    const std::string name = unique_region_name();
    ShmRegion writer;
    if (!shm_region_create(writer, name, kBytes)) {
        std::cerr << "could not create region\n";
        return false;
    }
    publish(writer, kW, kH, kYLen, 0xAA);

    ShmRegion reader;
    uint32_t mapped_gen = 0;
    std::vector<uint8_t> dst;
    uint32_t w = 0, h = 0, y_len = 0;
    bool ok = true;
    if (shm_read_i420_frame(reader, name, kW, kH, kGen1, mapped_gen, dst, w, h,
                            y_len) != ShmFrameRead::Ok) {
        std::cerr << "setup read failed\n";
        ok = false;
    }
    if (ok && (reader.ptr == nullptr || mapped_gen != kGen1)) {
        std::cerr << "setup did not leave a live mapping\n";
        ok = false;
    }

    if (ok) {
        // Returns true because a mapping really was dropped. Call sites log on
        // that, so a release that reported false here would go unrecorded and
        // the next recurrence would have no trail.
        if (!shm_release_for_resubscribe(reader, mapped_gen)) {
            std::cerr << "release of a live mapping did not report it\n";
            ok = false;
        }
        if (ok && reader.ptr != nullptr) {
            std::cerr << "release left the mapping in place\n";
            ok = false;
        }
        if (ok && reader.size != 0) {
            std::cerr << "release left a non-zero mapped size\n";
            ok = false;
        }
        if (ok && mapped_gen != 0) {
            std::cerr << "release did not reset the recorded generation\n";
            ok = false;
        }
    }

    // Releasing again has nothing to drop and must say so, or every no-op call
    // would emit a log line.
    if (ok && shm_release_for_resubscribe(reader, mapped_gen)) {
        std::cerr << "release with nothing mapped reported a drop\n";
        ok = false;
    }

    shm_region_destroy(writer);
    return ok;
}

// The whole reason the release is mandatory. A Windows named section cannot be
// recreated at a larger size while any process still maps it, so a reader that
// holds on across a re-subscribe stops the engine from ever publishing the new
// participant's larger frames.
bool test_live_mapping_blocks_the_engines_recreate()
{
#if defined(WIN32)
    const std::string name = unique_region_name();
    ShmRegion writer;
    if (!shm_region_create(writer, name, kBytes)) {
        std::cerr << "could not create the generation-1 region\n";
        return false;
    }
    publish(writer, kW, kH, kYLen, 0xAA);

    ShmRegion reader;
    uint32_t mapped_gen = 0;
    std::vector<uint8_t> dst;
    uint32_t w = 0, h = 0, y_len = 0;
    bool ok = true;
    if (shm_read_i420_frame(reader, name, kW, kH, kGen1, mapped_gen, dst, w, h,
                            y_len) != ShmFrameRead::Ok) {
        std::cerr << "setup read failed\n";
        ok = false;
    }

    // The re-subscribe: the engine drops the old SourceTarget and its region,
    // then the replacement target recreates the region — same legacy name,
    // bigger, because the new participant streams larger frames.
    shm_region_destroy(writer);

    ShmRegion recreated;
    if (ok && shm_region_create(recreated, name, kBigBytes)) {
        std::cerr << "recreate at a larger size unexpectedly SUCCEEDED while a "
                     "reader still mapped the region -- the premise of "
                     "shm-resubscribe.h no longer holds on this platform\n";
        shm_region_destroy(recreated);
        ok = false;
    }

    // Same call, after the release. This is the fix, and it must work.
    if (ok) {
        shm_release_for_resubscribe(reader, mapped_gen);
        if (!shm_region_create(recreated, name, kBigBytes)) {
            std::cerr << "recreate still failed after the reader released\n";
            ok = false;
        } else if (recreated.size != kBigBytes) {
            std::cerr << "recreated region did not get the requested size\n";
            ok = false;
        }
    }

    shm_release_for_resubscribe(reader, mapped_gen);
    shm_region_destroy(recreated);
    return ok;
#else
    // Deliberately not asserted off Windows: shm_open + ftruncate grows a
    // region in place, so there is no blocked-recreate to observe. Reported
    // rather than silently passing, so this does not read as covered here.
    std::cout << "  test_live_mapping_blocks_the_engines_recreate: SKIPPED "
                 "(Windows named-section rule; POSIX can grow in place)\n";
    return true;
#endif
}

// End to end at the read side, in the shape the on-air path actually takes:
// release, engine rebuilds the region under the SAME generation, next frame
// event must land on the new region rather than the orphaned old one.
bool test_reader_follows_the_rebuilt_region_after_release()
{
    const std::string name = unique_region_name();
    ShmRegion writer;
    if (!shm_region_create(writer, name, kBytes)) {
        std::cerr << "could not create the generation-1 region\n";
        return false;
    }
    publish(writer, kW, kH, kYLen, 0xAA);

    ShmRegion reader;
    uint32_t mapped_gen = 0;
    std::vector<uint8_t> dst;
    uint32_t w = 0, h = 0, y_len = 0;
    bool ok = true;
    if (shm_read_i420_frame(reader, name, kW, kH, kGen1, mapped_gen, dst, w, h,
                            y_len) != ShmFrameRead::Ok || dst[0] != 0xAA) {
        std::cerr << "setup read failed\n";
        ok = false;
    }

    // The fix: release before the subscribe reaches the engine.
    shm_release_for_resubscribe(reader, mapped_gen);

    // The replacement SourceTarget of an engine that restarts its counter: back
    // to generation 1 and the legacy unsuffixed name, at the new participant's
    // larger size.
    shm_region_destroy(writer);
    ShmRegion rebuilt;
    if (ok && !shm_region_create(rebuilt, name, kBigBytes)) {
        std::cerr << "engine could not rebuild the region after the release\n";
        ok = false;
    }
    if (ok) publish(rebuilt, kBigW, kBigH, kBigYLen, 0xBB);

    if (ok) {
        const ShmFrameRead status =
            shm_read_i420_frame(reader, name, kBigW, kBigH, kGen1, mapped_gen,
                                dst, w, h, y_len);
        if (status != ShmFrameRead::Ok) {
            std::cerr << "read after rebuild failed: "
                      << static_cast<int>(status) << "\n";
            ok = false;
        } else if (dst[0] != 0xBB || w != kBigW || h != kBigH) {
            std::cerr << "reader stayed on the orphaned pre-resubscribe region\n";
            ok = false;
        } else if (mapped_gen != kGen1) {
            std::cerr << "recorded generation not restored on reopen\n";
            ok = false;
        }
    }

    shm_release_for_resubscribe(reader, mapped_gen);
    shm_region_destroy(rebuilt);
    shm_region_destroy(writer);
    return ok;
}

}  // namespace

int main()
{
    if (!test_release_clears_mapping_and_generation()) return 1;
    if (!test_live_mapping_blocks_the_engines_recreate()) return 1;
    if (!test_reader_follows_the_rebuilt_region_after_release()) return 1;

    std::cout << "shm-resubscribe: all tests passed\n";
    return 0;
}
