// Pins the SHM region generation policy in src/shm-generation.h.
//
// This is the third production incident from one defect class, so what is
// pinned here is the property that makes the class unreachable rather than the
// symptom of any one instance.
//
// The class: the region generation is what keeps a resize off a name another
// process might still map (shm_region_name() in engine-ipc.h suffixes the name
// from generation 2 on). The counter feeding it used to be a member of the
// per-source target struct — SourceTarget, ShareTarget, AudioTarget — every one
// of which is destroyed and rebuilt on a re-subscribe. The counter therefore
// restarted at 0, ensure_shm() asked for generation 1, and generation 1 is the
// LEGACY UNSUFFIXED NAME. So every re-created region landed back on the one
// name the plugin could still be holding, and the resize failed with
// ERROR_ACCESS_DENIED (5) — the 07:31 on-air line. On the Active Speaker
// source, which re-subscribes on every speaker change, that is routine.
//
// The property: for a given region base name the generation only ever
// increases within an engine process, and it survives the target being
// destroyed and recreated. Every test below that says "survives" fails against
// the old per-target counter — that is the point of them.
//
// What is NOT covered: nothing here runs the engine, the Zoom SDK, or OBS.
// These tests pin the policy and (on Windows) the operating-system consequence
// of getting it wrong. They do not pin the wiring — the three ensure_shm()
// call sites in engine/src are not exercised, because no test constructs a
// ParticipantSubscription, an EngineShare, or an EngineAudio. Proving the
// wiring needs a live engine.

#include "shm-generation.h"

#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

const std::string kUuid  = "source_abc_9";
const std::string kVideo = std::string(IPC_SHM_PREFIX) + kUuid;
const std::string kAudio = std::string(IPC_SHM_PREFIX) + kUuid + "_audio";

// Stands in for SourceTarget / ShareTarget / AudioTarget: it holds the
// generation the CURRENT region was published under, and it is destroyed and
// rebuilt whenever the source re-subscribes. The generation it publishes must
// come from outside it.
struct FakeTarget {
    uint32_t    published_gen = 0;
    std::string region_name;

    void create_region(ShmGenerationTable &table, const std::string &base)
    {
        const ShmRegionAllocation allocation = shm_next_region(table, base);
        published_gen = allocation.gen;
        region_name   = allocation.name;
    }
};

// ── The first region keeps the legacy name (wire compatibility) ──────────────
void test_first_region_uses_the_legacy_name()
{
    ShmGenerationTable table;
    FakeTarget target;
    target.create_region(table, kVideo);

    check(target.published_gen == 1, "first region is generation 1");
    check(target.region_name == kVideo,
          "generation 1 is the legacy unsuffixed name (old plugins can open it)");
    check(table.issued(kVideo) == 1, "issued() reports the generation handed out");
    check(table.issued("never_used") == 0,
          "issued() reports 0 for a name no region was ever created under");
}

// ── THE FIX. Fails against the old per-target counter ───────────────────────
// A re-subscribe destroys the target. If the counter goes with it, the
// replacement asks for generation 1 again and lands on the legacy name — the
// name the plugin may still be mapping.
void test_generation_survives_target_destruction()
{
    ShmGenerationTable table;

    std::string first_name;
    {
        FakeTarget original;
        original.create_region(table, kVideo);
        first_name = original.region_name;
    }  // re-subscribe: unsubscribe_locked() -> remove_source() -> target erased

    FakeTarget replacement;
    replacement.create_region(table, kVideo);

    check(replacement.published_gen == 2,
          "the target rebuilt by a re-subscribe gets generation 2, not 1");
    check(replacement.region_name != first_name,
          "the rebuilt region gets a name the old mapping cannot be holding");
    check(replacement.region_name == kVideo + "_g2",
          "the rebuilt region is suffixed _g2");

    // And again — a source that re-subscribes on every active-speaker change
    // must keep climbing, not oscillate between two names.
    {
        FakeTarget third;
        third.create_region(table, kVideo);
        check(third.published_gen == 3, "a third subscribe gets generation 3");
        check(third.region_name == kVideo + "_g3", "…on the _g3 name");
    }
}

// ── A resize on a LIVE target advances too (the original mechanism) ─────────
void test_resize_on_a_live_target_advances()
{
    ShmGenerationTable table;
    FakeTarget target;

    target.create_region(table, kVideo);          // first frame
    // Not named `small`: <windows.h> drags in rpcndr.h, which #defines it.
    const std::string before_resize = target.region_name;
    target.create_region(table, kVideo);          // participant sends larger frames

    check(target.published_gen == 2, "an in-place resize advances the generation");
    check(target.region_name != before_resize, "…onto a different name");
}

// ── Video and share share a counter; audio has its own ──────────────────────
// EngineVideo and EngineShare both name regions IPC_SHM_PREFIX + uuid, and
// main.cpp routes one uuid to either path by the subscribe's "mode". Two
// counters could therefore hand two live regions the same OS object name.
void test_video_and_share_never_collide_and_audio_is_separate()
{
    ShmGenerationTable table;

    FakeTarget video;
    video.create_region(table, kVideo);           // subscribed as a video source

    FakeTarget share;
    share.create_region(table, kVideo);           // re-subscribed as screenshare

    check(share.published_gen == 2,
          "share continues the video counter for the same base name");
    check(share.region_name != video.region_name,
          "share never re-creates the region name video just used");

    FakeTarget audio;
    audio.create_region(table, kAudio);
    check(audio.published_gen == 1,
          "audio starts its own counter (different base name)");
    check(audio.region_name == kAudio,
          "audio's first region is its legacy unsuffixed name");
    check(audio.region_name != video.region_name &&
              audio.region_name != share.region_name,
          "audio names never collide with video/share names");

    // Audio's counter advances independently of the video path's.
    FakeTarget audio_again;
    audio_again.create_region(table, kAudio);
    check(audio_again.published_gen == 2,
          "audio's second region is generation 2 regardless of video traffic");
}

// ── No name is ever issued twice, across any interleaving ───────────────────
void test_no_name_is_ever_issued_twice()
{
    ShmGenerationTable table;
    std::set<std::string> seen;
    bool duplicate = false;
    uint32_t previous_for_video = 0;

    const std::vector<std::string> bases = {
        kVideo, kAudio, std::string(IPC_SHM_PREFIX) + "source_other_2",
    };

    for (int round = 0; round < 50; ++round) {
        for (const std::string &base : bases) {
            FakeTarget target;                    // destroyed each iteration
            target.create_region(table, base);
            if (!seen.insert(target.region_name).second) duplicate = true;
            if (base == kVideo) {
                if (target.published_gen <= previous_for_video) duplicate = true;
                previous_for_video = target.published_gen;
            }
        }
    }

    check(!duplicate,
          "no region name is issued twice and no generation ever goes backwards");
    check(seen.size() == bases.size() * 50, "every issued name was distinct");
    check(table.tracked_names() == bases.size(),
          "one counter per base name, however many targets came and went");
}

// ── Concurrency: the three media paths run on separate SDK threads ──────────
void test_concurrent_issuers_never_repeat()
{
    ShmGenerationTable table;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;

    std::vector<std::vector<uint32_t>> issued(kThreads);
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&table, &issued, t, kPerThread]() {
            issued[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
                issued[t].push_back(table.next(kVideo));
        });
    }
    for (std::thread &worker : workers) worker.join();

    std::set<uint32_t> all;
    for (const std::vector<uint32_t> &per_thread : issued)
        all.insert(per_thread.begin(), per_thread.end());

    check(all.size() == static_cast<size_t>(kThreads) * kPerThread,
          "concurrent callers never receive the same generation twice");
    check(*all.begin() == 1 && *all.rbegin() == kThreads * kPerThread,
          "concurrent generations are the contiguous range 1..N");
    check(table.issued(kVideo) == kThreads * kPerThread,
          "the table's record matches what it handed out");
}

// ── Saturation, not wraparound ──────────────────────────────────────────────
// A uint32_t bumped once per re-subscribe cannot realistically wrap in a
// session. If it ever did, the counter must stick at the ceiling rather than
// wrap to a low generation whose name another mapping might still hold: that
// would resurrect exactly this defect. Stuck at the ceiling, one region reverts
// to repeated creates under one name — degraded, not corrupt.
void test_saturates_instead_of_wrapping()
{
    ShmGenerationTable table(3);  // the real ceiling is UINT32_MAX

    check(table.next(kVideo) == 1, "generation 1 below the ceiling");
    check(table.next(kVideo) == 2, "generation 2 below the ceiling");
    check(table.next(kVideo) == 3, "generation 3 reaches the ceiling");

    for (int i = 0; i < 5; ++i) {
        const uint32_t gen = table.next(kVideo);
        check(gen == 3, "a saturated counter keeps returning the ceiling");
        check(gen != 0 && gen != 1,
              "a saturated counter never wraps back to a reusable generation");
    }
    check(table.issued(kVideo) == 3, "issued() reports the saturated ceiling");
}

#if defined(WIN32)
// ── The operating-system consequence, end to end ────────────────────────────
// This is the 07:31 failure reproduced: the plugin holds a mapping of the
// legacy-named region, the source re-subscribes (target destroyed and rebuilt),
// and the rebuilt target needs a LARGER region. With a per-target counter the
// rebuild asks for generation 1 — the name still mapped — and MapViewOfFile
// returns ERROR_ACCESS_DENIED. With the surviving counter it asks for _g2 and
// the create succeeds while the old mapping is still live.
void test_rebuilt_target_can_resize_under_a_live_reader()
{
    const std::string base = "CoreVideoShmGenTest_" +
        std::to_string(GetCurrentProcessId());
    ShmGenerationTable table;

    ShmRegion engine_region;
    FakeTarget original;
    original.create_region(table, base);
    check(shm_region_create(engine_region, original.region_name, 4096),
          "engine creates the first region");

    ShmRegion plugin_mapping;  // the plugin's read mapping, still live
    check(shm_region_open_read(plugin_mapping, original.region_name, 4096),
          "plugin maps the first region");

    // The re-subscribe: the engine destroys the target and its region handle,
    // but the plugin's mapping keeps the section alive.
    shm_region_destroy(engine_region);

    // What the old per-target counter did: generation restarts, so the rebuilt
    // target asks for generation 1 — the name the plugin still maps — at the
    // new participant's larger size.
    ShmRegion reset_attempt;
    const bool reset_created =
        shm_region_create(reset_attempt, shm_region_name(base, 1), 65536);
    check(!reset_created,
          "a rebuilt target that restarts at generation 1 CANNOT resize under "
          "the live mapping (this is the defect)");
    if (!reset_created) {
        if (reset_attempt.last_error != ERROR_ACCESS_DENIED) {
            std::cerr << "  note: failure code was " << reset_attempt.last_error
                      << ", expected " << ERROR_ACCESS_DENIED << "\n";
        }
        check(reset_attempt.last_error == ERROR_ACCESS_DENIED,
              "…and it fails with ERROR_ACCESS_DENIED (5), the on-air log code");
    }
    shm_region_destroy(reset_attempt);

    // What the surviving counter does: generation 2, a name nothing can hold.
    FakeTarget rebuilt;
    rebuilt.create_region(table, base);
    check(rebuilt.published_gen == 2, "the rebuilt target is on generation 2");

    ShmRegion resized;
    check(shm_region_create(resized, rebuilt.region_name, 65536),
          "the rebuilt target resizes successfully WITH the old mapping still live");
    check(resized.size == 65536, "the resized region has the requested size");

    shm_region_destroy(resized);
    shm_region_destroy(plugin_mapping);
    shm_region_destroy(engine_region);
}
#endif  // WIN32

}  // namespace

int main()
{
    test_first_region_uses_the_legacy_name();
    test_generation_survives_target_destruction();
    test_resize_on_a_live_target_advances();
    test_video_and_share_never_collide_and_audio_is_separate();
    test_no_name_is_ever_issued_twice();
    test_concurrent_issuers_never_repeat();
    test_saturates_instead_of_wrapping();
#if defined(WIN32)
    test_rebuilt_target_can_resize_under_a_live_reader();
#else
    // Deliberately not asserted off Windows: shm_open + ftruncate grows a
    // region in place, so there is no blocked-recreate to observe. Reported
    // rather than silently passing, so this does not read as covered here.
    std::cout << "  test_rebuilt_target_can_resize_under_a_live_reader: SKIPPED "
                 "(Windows named-section rule; POSIX can grow in place)\n";
#endif

    if (g_failures == 0)
        std::cout << "shm-generation: all tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
