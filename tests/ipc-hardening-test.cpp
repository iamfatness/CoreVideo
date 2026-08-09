// Unit tests for the IPC/SHM hardening policy helpers in engine-ipc.h
// (issue #106): the heartbeat-timeout decision, the SHM generation/staleness
// guard, and the SHM source cap. On Windows this file also exercises the
// line-I/O framing and write-failure semantics over a real pipe (the POSIX
// socket equivalent lives in ipc-line-io-test.cpp).

#include "engine-ipc.h"

#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

// ── Heartbeat timeout decision (used by ZoomEngineClient::monitor_loop) ──────
static void test_heartbeat_expiry()
{
    // Nothing received yet — never expired (caller seeds the clock on connect).
    check(!ipc_heartbeat_expired(50000, 0, 10000), "last_rx=0 is never expired");

    // Fresh traffic within the timeout window.
    check(!ipc_heartbeat_expired(15000, 10000, 10000),
          "silence shorter than timeout is not expired");

    // Exactly at the timeout boundary — not yet expired (strictly greater).
    check(!ipc_heartbeat_expired(20000, 10000, 10000),
          "silence equal to timeout is not expired");

    // Past the timeout — expired.
    check(ipc_heartbeat_expired(20001, 10000, 10000),
          "silence longer than timeout is expired");

    // last_rx in the future (clock re-seed race) — treated as fresh.
    check(!ipc_heartbeat_expired(10000, 20000, 10000),
          "future last_rx is not expired");
}

// ── SHM mapping staleness / generation guard (plugin read side) ──────────────
static void test_shm_mapping_stale()
{
    int dummy = 0;
    const void *mapped = &dummy;

    // Never mapped — must open.
    check(shm_mapping_stale(nullptr, 0, 100, 1, 0), "unmapped region is stale");

    // Mapped but too small for this frame — must re-open.
    check(shm_mapping_stale(mapped, 64, 100, 1, 1),
          "undersized mapping is stale");

    // Same generation and big enough — reusable.
    check(!shm_mapping_stale(mapped, 100, 100, 3, 3),
          "matching generation is not stale");

    // Engine recreated the region (generation moved on) — old mapping is
    // orphaned even though it is big enough.
    check(shm_mapping_stale(mapped, 4096, 100, 4, 3),
          "generation change makes mapping stale");

    // Old engine that never sends a generation (event_gen == 0) — the
    // generation check must be skipped for backward compatibility.
    check(!shm_mapping_stale(mapped, 4096, 100, 0, 3),
          "missing event generation skips the generation check");

    // First open recorded gen 0 (old engine), new engine now reports gen 1 —
    // must re-open.
    check(shm_mapping_stale(mapped, 4096, 100, 1, 0),
          "upgrade from gen-less mapping to generation 1 is stale");
}

// ── SHM source cap (engine video/share/audio subscribe paths) ────────────────
static void test_shm_source_cap()
{
    // Below the cap: new sources are accepted.
    check(!shm_source_over_cap(0, false), "first source accepted");
    check(!shm_source_over_cap(kMaxShmSources - 1, false),
          "source at cap-1 accepted");

    // At the cap: a NEW source is rejected...
    check(shm_source_over_cap(kMaxShmSources, false),
          "new source over cap rejected");

    // ...but re-registering an existing source is always allowed.
    check(!shm_source_over_cap(kMaxShmSources, true),
          "existing source re-subscribe allowed at cap");
    check(!shm_source_over_cap(kMaxShmSources + 5, true),
          "existing source re-subscribe allowed over cap");

    // Explicit cap override behaves the same way.
    check(shm_source_over_cap(2, false, 2), "custom cap enforced");
    check(!shm_source_over_cap(1, false, 2), "custom cap not yet reached");
}

#if defined(WIN32)
// ── Line I/O over a real Windows pipe ────────────────────────────────────────
// Mirrors the POSIX socket coverage in ipc-line-io-test.cpp: framing round
// trips, the oversized-line guard, and — most importantly — that
// ipc_write_line() reports failure instead of silently dropping bytes once the
// peer is gone.
static void test_win_line_io()
{
    HANDLE rd = nullptr, wr = nullptr;
    check(CreatePipe(&rd, &wr, nullptr, 0) != 0, "anonymous pipe created");

    check(ipc_write_line(wr, "{\"cmd\":\"ping\"}"), "write returns true");
    check(ipc_write_line(wr, "second"), "second write returns true");

    std::string line;
    check(ipc_read_line(rd, line) && line == "{\"cmd\":\"ping\"}",
          "round-trip payload matches (newline stripped)");
    check(ipc_read_line(rd, line) && line == "second", "second line framed");

    // Oversized line must be rejected rather than read unbounded.
    check(ipc_write_line(wr, std::string(64, 'x')), "oversized payload written");
    check(!ipc_read_line(rd, line, 16), "oversized line rejected");

    CloseHandle(rd);
    CloseHandle(wr);
}

static void test_win_broken_pipe()
{
    HANDLE rd = nullptr, wr = nullptr;
    check(CreatePipe(&rd, &wr, nullptr, 0) != 0, "anonymous pipe created");

    CloseHandle(rd); // drop the reader

    // Writing to a pipe whose read end is closed must report failure, not
    // silently drop the message — the hardened reconnect path depends on it.
    bool saw_failure = false;
    for (int i = 0; i < 10000 && !saw_failure; ++i) {
        if (!ipc_write_line(wr, "{\"cmd\":\"frame\"}"))
            saw_failure = true;
    }
    check(saw_failure, "write to closed peer eventually returns false");

    CloseHandle(wr);
}

static void test_win_invalid_fd()
{
    check(!ipc_write_line(kIpcInvalidFd, "anything"),
          "write to invalid handle returns false");
}
#endif // WIN32

// ── Generation-suffixed region names (resize deadlock, live incident fix) ────
static void test_shm_region_name()
{
    // Generations 0/1 keep the legacy unsuffixed name (compat with engines
    // that never resize and with pre-suffix plugin binaries).
    check(shm_region_name("ZoomObsPlugin_abc", 0) == "ZoomObsPlugin_abc",
          "gen 0 uses the legacy name");
    check(shm_region_name("ZoomObsPlugin_abc", 1) == "ZoomObsPlugin_abc",
          "gen 1 uses the legacy name");
    check(shm_region_name("ZoomObsPlugin_abc", 2) == "ZoomObsPlugin_abc_g2",
          "gen 2 is suffixed");
    check(shm_region_name("ZoomObsPlugin_abc_audio", 3) ==
              "ZoomObsPlugin_abc_audio_g3",
          "audio base names suffix the same way");
}

#if defined(WIN32)
// Pins the OS behavior behind the 2026-08-08 live incident: a named section
// cannot be recreated at a larger size while another mapping keeps it alive
// (CreateFileMapping returns the old smaller section; the larger view fails),
// but a generation-suffixed fresh name succeeds immediately.
static void test_win_shm_resize_requires_new_name()
{
    const std::string base = "CoreVideoShmResizeTest_" +
        std::to_string(GetCurrentProcessId());

    ShmRegion writer;
    check(shm_region_create(writer, shm_region_name(base, 1), 4096),
          "initial region creates");

    ShmRegion reader; // simulates the plugin holding a mapping
    check(shm_region_open_read(reader, shm_region_name(base, 1), 4096),
          "reader maps the initial region");

    // The engine's old behavior: recreate LARGER under the SAME name while
    // the reader still maps it — must fail (this was the deadlock).
    ShmRegion resized;
    check(!shm_region_create(resized, shm_region_name(base, 1), 65536),
          "larger recreate under the same live name fails");

    // The fix: a generation-suffixed fresh name succeeds immediately.
    check(shm_region_create(resized, shm_region_name(base, 2), 65536),
          "larger recreate under the gen-suffixed name succeeds");
    check(resized.size == 65536, "suffixed region has the requested size");

    shm_region_destroy(resized);
    shm_region_destroy(reader);
    shm_region_destroy(writer);
}
#endif // WIN32

int main()
{
    test_heartbeat_expiry();
    test_shm_mapping_stale();
    test_shm_source_cap();
    test_shm_region_name();
#if defined(WIN32)
    test_win_line_io();
    test_win_broken_pipe();
    test_win_invalid_fd();
    test_win_shm_resize_requires_new_name();
#endif

    if (g_failures == 0)
        std::cout << "All IPC hardening tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
