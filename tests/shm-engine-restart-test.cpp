// Pins the last residual of the SHM region-name defect class: an engine
// RESTART.
//
// The class, in one line: the engine must never create a region under a name
// the plugin might still be mapping, because Windows will not recreate a named
// section at a larger size while anything maps it.
//
// Two fixes already landed, and BOTH are scoped to a single engine process:
//
//   * src/shm-generation.h keeps the generation counter in a process-wide
//     table, so a target destroyed and rebuilt by a re-subscribe keeps climbing
//     (_g2, _g3, …) instead of restarting on the legacy unsuffixed name.
//     tests/shm-generation-test.cpp pins that.
//   * src/shm-resubscribe.h makes the plugin drop its mapping before a
//     subscribe that re-points a uuid. tests/shm-resubscribe-test.cpp pins that.
//
// Neither survives the engine process ending. A new ZoomObsEngine starts with
// an EMPTY table, so its first create for any region asks for generation 1 —
// the legacy unsuffixed name — no matter how high the dead engine had climbed.
// Every mapping the plugin carried across the restart is therefore sitting on a
// name the new engine is about to ask for.
//
// What that costs is worst for audio, and audio is the reason this file exists.
// Video is re-subscribed after a restart through ZoomSource::subscribe(), which
// releases first; audio is not released on any subscribe path (deliberately —
// see release_audio_shm_locked() in src/zoom-source.cpp) and it has no
// self-healing fallback: a failed ensure_shm() publishes no audio event
// (engine/src/engine-audio.cpp), so nothing ever prompts the plugin to reopen
// and that source stays silent for the rest of the session.
//
// The fix is a single release of EVERY mapping when a new engine process comes
// up, dispatched from ZoomEngineClient::start() to every registered source.
// The two tests below pin the two claims that decision rests on: that a new
// engine process really does land back on the legacy name, and that releasing
// only the region the re-subscribe touches is not enough.
//
// What is NOT covered, stated plainly. Nothing here constructs a
// ZoomEngineClient, a ZoomSource, a CoreVideoAudioSource or a tile feed — they
// need Qt, OBS and a live engine. These tests pin the platform rule and the
// generation arithmetic that make the release necessary. They do NOT pin the
// wiring: deleting the on_new_engine_process callback from any of its four
// registration sites would fail nothing here. Proving the wiring needs a live
// engine and a running OBS.

#include "shm-generation.h"
#include "shm-resubscribe.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

const std::string kUuid  = "source_restart_7";
const std::string kVideo = std::string(IPC_SHM_PREFIX) + kUuid;
const std::string kAudio = std::string(IPC_SHM_PREFIX) + kUuid + "_audio";

// ── A new engine process lands back on the legacy name ──────────────────────
// This is the residual in arithmetic. The generation table is per PROCESS, so
// "the counter survives the target" (shm-generation.h) says nothing about a
// counter that dies with the engine.
void test_a_new_engine_process_restarts_at_the_legacy_name()
{
    // Engine A runs a full show: this source re-points repeatedly and its audio
    // region climbs well clear of the legacy name.
    ShmGenerationTable engine_a;
    std::string last_audio_name;
    for (int i = 0; i < 4; ++i)
        last_audio_name = shm_next_region(engine_a, kAudio).name;
    check(last_audio_name == kAudio + "_g4",
          "engine A's audio region climbed to _g4");
    check(shm_next_region(engine_a, kVideo).name == kVideo,
          "engine A's first video region is the legacy unsuffixed name");

    // Engine A dies (crash, heartbeat timeout, or a deliberate stop/start) and
    // engine B is launched. A process-wide table is exactly as wide as the
    // process: engine B's is empty.
    ShmGenerationTable engine_b;

    const ShmRegionAllocation first = shm_next_region(engine_b, kAudio);
    check(first.gen == 1,
          "a new engine process issues generation 1 for a region the dead one "
          "had taken to generation 4");
    check(first.name == kAudio,
          "…and generation 1 is the LEGACY UNSUFFIXED name — the one the plugin "
          "may still be mapping");
    check(first.name != last_audio_name,
          "…which is not the name the dead engine last used, so following the "
          "old generation is no defence either");

    // Not a defect in the table: generation 1 must stay the legacy name so a
    // plugin build that predates suffixed names can still open a first region.
    // The plugin side is what has to change, by letting go.
    check(engine_b.issued(kVideo) == 0,
          "the new process has no memory of any region the old one created");
}

#if defined(WIN32)
std::string unique_base()
{
    static int counter = 0;
    // Deliberately NOT under IPC_SHM_PREFIX: a real engine may be live on this
    // machine while the tests run, and these names must not collide with it.
    return "CoreVideoShmRestartTest_" + std::to_string(GetCurrentProcessId()) +
           "_" + std::to_string(++counter);
}

// ── Releasing only what the re-subscribe touches is not enough ──────────────
// The operating-system consequence, in the exact shape the restart takes: one
// source owns TWO regions, "<base>" (video) and "<base>_audio". After the
// restart the plugin re-subscribes, which releases the video mapping and only
// the video mapping. The new engine's first video create then succeeds and its
// first audio create fails — which is the bug, because a failed audio create
// publishes no event and so nothing ever asks the plugin to reopen.
void test_releasing_only_the_resubscribed_region_leaves_audio_blocked()
{
    const std::string base       = unique_base();
    const std::string audio_base = base + "_audio";

    // ── Engine A ────────────────────────────────────────────────────────────
    ShmGenerationTable engine_a;
    const ShmRegionAllocation a_video = shm_next_region(engine_a, base);
    const ShmRegionAllocation a_audio = shm_next_region(engine_a, audio_base);
    check(a_video.name == base && a_audio.name == audio_base,
          "engine A's first regions use the legacy unsuffixed names");

    ShmRegion engine_a_video;
    ShmRegion engine_a_audio;
    check(shm_region_create(engine_a_video, a_video.name, 4096),
          "engine A creates the video region");
    check(shm_region_create(engine_a_audio, a_audio.name, 4096),
          "engine A creates the audio region");

    // The plugin maps both and records the generation each was opened against.
    ShmRegion plugin_video;
    uint32_t  plugin_video_gen = 0;
    ShmRegion plugin_audio;
    uint32_t  plugin_audio_gen = 0;
    check(shm_region_open_read(plugin_video, a_video.name, 4096),
          "the plugin maps the video region");
    plugin_video_gen = a_video.gen;
    check(shm_region_open_read(plugin_audio, a_audio.name, 4096),
          "the plugin maps the audio region");
    plugin_audio_gen = a_audio.gen;

    // ── Engine A exits ──────────────────────────────────────────────────────
    // Its handles close with the process. The sections stay alive at their old
    // size because the PLUGIN still maps them — that is the whole problem.
    shm_region_destroy(engine_a_video);
    shm_region_destroy(engine_a_audio);

    // ── Engine B ────────────────────────────────────────────────────────────
    ShmGenerationTable engine_b;

    // What the restart path did before this fix: ZoomSource::subscribe()
    // releases the video mapping, and nothing releases the audio one.
    check(shm_release_for_resubscribe(plugin_video, plugin_video_gen),
          "the re-subscribe drops the video mapping");
    check(plugin_video_gen == 0, "…and forgets the generation it held");

    // Engine B's first video create: generation 1, legacy name, larger than
    // engine A's region because the participant is streaming at a higher
    // resolution. Nothing maps that name any more, so it succeeds.
    const ShmRegionAllocation b_video = shm_next_region(engine_b, base);
    check(b_video.name == base, "engine B's first video region is the legacy name");
    ShmRegion engine_b_video;
    check(shm_region_create(engine_b_video, b_video.name, 65536),
          "engine B recreates the video region larger, because we let go of it");

    // Engine B's first audio create: same generation 1, same legacy name — and
    // this one we are still holding.
    const ShmRegionAllocation b_audio = shm_next_region(engine_b, audio_base);
    check(b_audio.name == audio_base,
          "engine B's first audio region is the legacy name too");
    ShmRegion engine_b_audio;
    const bool blocked_create =
        shm_region_create(engine_b_audio, b_audio.name, 65536);
    check(!blocked_create,
          "engine B CANNOT create the larger audio region while the plugin's "
          "pre-restart mapping is still live (this is the defect)");
    if (!blocked_create) {
        if (engine_b_audio.last_error != ERROR_ACCESS_DENIED) {
            std::cerr << "  note: failure code was " << engine_b_audio.last_error
                      << ", expected " << ERROR_ACCESS_DENIED << "\n";
        }
        check(engine_b_audio.last_error == ERROR_ACCESS_DENIED,
              "…and it fails with ERROR_ACCESS_DENIED (5), the on-air log code");
    }
    shm_region_destroy(engine_b_audio);

    // ── The fix ─────────────────────────────────────────────────────────────
    // One release of EVERY mapping when the new engine process comes up, not
    // one release per subscribe. The audio create then succeeds on its first
    // attempt, which is the only attempt it gets.
    check(shm_release_for_resubscribe(plugin_audio, plugin_audio_gen),
          "the new-engine release drops the audio mapping the re-subscribe missed");
    check(plugin_audio_gen == 0, "…and forgets the generation it held");
    check(shm_region_create(engine_b_audio, b_audio.name, 65536),
          "engine B now creates the larger audio region successfully");
    check(engine_b_audio.size == 65536,
          "…at the size it asked for, not the dead engine's");

    shm_region_destroy(engine_b_audio);
    shm_region_destroy(engine_b_video);
    shm_release_for_resubscribe(plugin_video, plugin_video_gen);
    shm_release_for_resubscribe(plugin_audio, plugin_audio_gen);
}

// ── The release has to come first, and there is no second chance ────────────
// Order is the whole rule (shm-resubscribe.h). Releasing after the new engine
// has already tried does not repair that attempt: the create failed, and on the
// audio path a failed create publishes no event, so nothing on the plugin side
// is ever prompted to try again. This pins the "no second chance" half — that
// the failed create leaves NO region behind for anyone to read.
void test_a_late_release_does_not_repair_the_failed_create()
{
    const std::string base = unique_base() + "_audio";

    ShmGenerationTable engine_a;
    const ShmRegionAllocation a = shm_next_region(engine_a, base);
    ShmRegion engine_a_region;
    check(shm_region_create(engine_a_region, a.name, 4096),
          "engine A creates the audio region");

    ShmRegion plugin;
    uint32_t  plugin_gen = 0;
    check(shm_region_open_read(plugin, a.name, 4096),
          "the plugin maps it");
    plugin_gen = a.gen;
    shm_region_destroy(engine_a_region);  // engine A exits

    ShmGenerationTable engine_b;
    const ShmRegionAllocation b = shm_next_region(engine_b, base);
    ShmRegion engine_b_region;
    check(!shm_region_create(engine_b_region, b.name, 65536),
          "engine B's create is blocked by the surviving mapping");

    // Now release — too late. The engine has already given up on this buffer
    // and reported audio_shm_create_failed; the region it wanted does not
    // exist, and re-opening finds nothing at the new size.
    shm_release_for_resubscribe(plugin, plugin_gen);
    ShmRegion reopen_attempt;
    check(!shm_region_open_read(reopen_attempt, b.name, 65536),
          "there is no region to reopen after a failed create — the late "
          "release recovers nothing, which is why the trigger must fire before "
          "the new engine can be sent a subscribe");
    shm_region_destroy(reopen_attempt);

    // And the same create, run in the correct order, works.
    check(shm_region_create(engine_b_region, b.name, 65536),
          "the create succeeds once the mapping is gone");
    shm_region_destroy(engine_b_region);
    shm_release_for_resubscribe(plugin, plugin_gen);
}
#endif  // WIN32

}  // namespace

int main()
{
    test_a_new_engine_process_restarts_at_the_legacy_name();
#if defined(WIN32)
    test_releasing_only_the_resubscribed_region_leaves_audio_blocked();
    test_a_late_release_does_not_repair_the_failed_create();
#else
    // Deliberately not asserted off Windows: shm_open + ftruncate grows a
    // region in place, so there is no blocked-recreate to observe. Reported
    // rather than silently passing, so this does not read as covered here.
    std::cout << "  test_releasing_only_the_resubscribed_region_leaves_audio_"
                 "blocked: SKIPPED (Windows named-section rule; POSIX can grow "
                 "in place)\n";
    std::cout << "  test_a_late_release_does_not_repair_the_failed_create: "
                 "SKIPPED (Windows named-section rule; POSIX can grow in "
                 "place)\n";
#endif

    if (g_failures == 0)
        std::cout << "shm-engine-restart: all tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
