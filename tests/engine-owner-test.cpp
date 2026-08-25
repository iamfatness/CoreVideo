// Pins the owner-id policy in src/engine-owner.h.
//
// The defect class this belongs to is 2026-08-17's: an engine the launcher did
// not start, sharing names with the engine it did start. That cost ~92% of
// audio with no error surfaced anywhere, and the fix — sweep every
// ZoomObsEngine before launching — only holds while one product owns the
// machine's engines. A second product on the same engine (ZComms) breaks it in
// both directions at once: the two products' launches terminate each other,
// and their regions collide under the one hardcoded "ZoomObsPlugin_" prefix.
//
// What is pinned: distinct owners can never collide on any name, a launcher
// only ever claims its own engines, and the default owner reproduces the names
// shipping today byte-for-byte. The last one is not a nicety — an installed
// plugin and engine are a matched pair over these strings, and a half-updated
// install (new DLL, old engine still in zoom-runtime\) is the routine case.
//
// What is NOT covered: nothing here launches a process, opens a pipe, or maps
// a region. This is string policy. The wiring — engine-ipc.h's constants, the
// engine's --owner argument, and replacing the sweep's image-name scan with a
// per-owner pid record — is a separate change and is not exercised by any test
// in this file. Proving the sweep needs two live engines on one machine.

#include "engine-owner.h"

#include <iostream>
#include <set>
#include <string>
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

// ── The names shipping today ─────────────────────────────────────────────────
// Hardcoded here on purpose. If a refactor changes what the default owner
// produces, this test is the thing that says so, and the literals below are
// what the currently-installed engine binaries expect.
void test_legacy_names_are_byte_identical()
{
    const std::string obs = kEngineOwnerObsPlugin;

    check(engine_shm_prefix(obs) == "ZoomObsPlugin_",
          "default shm prefix matches IPC_SHM_PREFIX shipping today");
    check(engine_pipe_name_win(obs, true) == "\\\\.\\pipe\\ZoomObsPlugin_P2E",
          "default P2E pipe matches the name shipping today");
    check(engine_pipe_name_win(obs, false) == "\\\\.\\pipe\\ZoomObsPlugin_E2P",
          "default E2P pipe matches the name shipping today");
    check(engine_socket_path_posix(obs, true) == "/tmp/ZoomObsPlugin_P2E.sock",
          "default P2E socket matches the path shipping today");
    check(engine_socket_path_posix(obs, false) == "/tmp/ZoomObsPlugin_E2P.sock",
          "default E2P socket matches the path shipping today");

    // An empty or malformed owner must land on the same names, not on a
    // half-formed one: a plugin built before --owner exists passes nothing.
    check(engine_shm_prefix("") == "ZoomObsPlugin_",
          "empty owner falls back to the legacy prefix");
    check(engine_pipe_name_win("", true) == "\\\\.\\pipe\\ZoomObsPlugin_P2E",
          "empty owner falls back to the legacy pipe name");
}

// ── Distinct owners never collide ────────────────────────────────────────────
// This is the ghost-writer property. Every name either product can construct
// must be unique across owners, or 2026-08-17 happens again by construction.
void test_distinct_owners_share_no_name()
{
    const std::vector<std::string> owners = {
        "ZoomObsPlugin", "ZComms", "ZComms-1", "ZComms-2", "zcomms", "a", "A",
    };

    std::set<std::string> seen;
    for (const auto &o : owners) {
        for (bool p2e : {true, false}) {
            check(seen.insert(engine_pipe_name_win(o, p2e)).second,
                  "pipe name is unique across owners and directions");
            check(seen.insert(engine_socket_path_posix(o, p2e)).second,
                  "socket path is unique across owners and directions");
        }
        check(seen.insert(engine_shm_prefix(o)).second,
              "shm prefix is unique across owners");
    }

    // Prefixes must not nest either: "ZComms_" must not be a prefix of another
    // owner's prefix, or a region name under one could parse as the other's.
    for (const auto &a : owners) {
        for (const auto &b : owners) {
            if (a == b) continue;
            const std::string pa = engine_shm_prefix(a);
            const std::string pb = engine_shm_prefix(b);
            check(pb.compare(0, pa.size(), pa) != 0,
                  "no owner's shm prefix is a prefix of another's");
        }
    }
}

// ── A launcher claims only its own engines ───────────────────────────────────
// The literal statement of "engine A's launch does not kill engine B".
void test_sweep_claims_only_its_own()
{
    check(engine_owner_claims("ZComms", "ZComms"),
          "a launcher claims an engine carrying its own owner id");
    check(!engine_owner_claims("ZComms", "ZoomObsPlugin"),
          "the intercom never claims the OBS plugin's engine");
    check(!engine_owner_claims("ZoomObsPlugin", "ZComms"),
          "the OBS plugin never claims the intercom's engine");
    check(!engine_owner_claims("ZComms-1", "ZComms-2"),
          "one channel's engine does not claim a sibling channel's");

    // Case matters: these become OS object names, and a Windows pipe name is
    // case-insensitive while a POSIX socket path is not. Treating them as
    // distinct owners is the safe direction — it can only ever fail to kill.
    check(!engine_owner_claims("ZComms", "zcomms"),
          "owner comparison is case-sensitive");
}

// ── Untagged engines belong to the plugin ────────────────────────────────────
// A half-updated install (new DLL, old engine in zoom-runtime\) is the routine
// way an engine with no --owner appears. The plugin must still sweep it — that
// is 2026-08-17's protection. The intercom must not, or it takes out the
// user's OBS session mid-show.
void test_untagged_engine_belongs_to_the_plugin()
{
    check(engine_owner_claims("ZoomObsPlugin", ""),
          "the plugin sweeps an engine that predates --owner");
    check(engine_owner_claims("", ""),
          "an untagged launcher and an untagged engine are the same owner");
    check(!engine_owner_claims("ZComms", ""),
          "the intercom never sweeps an untagged engine");
}

// ── Owner ids are untrusted input ────────────────────────────────────────────
// They arrive on a command line and become a socket path and a pipe name, so
// anything that could escape /tmp or the \\.\pipe\ namespace is rejected
// outright rather than escaped.
void test_hostile_owner_ids_are_rejected()
{
    const std::vector<std::string> bad = {
        "",  "..", "../etc", "a/b", "a\\b", "a b", "a.b", "a\tb",
        std::string("x") + '\0' + "y",
        std::string(kEngineOwnerMaxLen + 1, 'a'),
    };
    for (const auto &o : bad)
        check(!engine_owner_id_valid(o), "hostile or malformed owner id rejected");

    const std::vector<std::string> good = {
        "ZoomObsPlugin", "ZComms", "ZComms-2", "z_1", "a",
        std::string(kEngineOwnerMaxLen, 'a'),
    };
    for (const auto &o : good)
        check(engine_owner_id_valid(o), "well-formed owner id accepted");

    // Rejection must route to the default, never into a name.
    check(engine_socket_path_posix("../etc/passwd", true) ==
              "/tmp/ZoomObsPlugin_P2E.sock",
          "a path-escaping owner id cannot reach a socket path");
    check(engine_pipe_name_win("a\\b", true) == "\\\\.\\pipe\\ZoomObsPlugin_P2E",
          "a namespace-escaping owner id cannot reach a pipe name");
}

} // namespace

int main()
{
    test_legacy_names_are_byte_identical();
    test_distinct_owners_share_no_name();
    test_sweep_claims_only_its_own();
    test_untagged_engine_belongs_to_the_plugin();
    test_hostile_owner_ids_are_rejected();

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "engine-owner: all checks passed\n";
    return 0;
}
