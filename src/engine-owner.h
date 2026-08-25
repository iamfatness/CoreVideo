#pragma once

// Owner-scoped IPC names, and the predicate that decides which engines a
// launcher is allowed to kill.
//
// WHY THIS EXISTS
//
// terminate_stale_engine_processes() in zoom-engine-client.cpp kills EVERY
// ZoomObsEngine on the machine before launching a fresh one. That is correct
// for a single product and it is not gratuitous: an orphaned engine
// ghost-writes same-named shared-memory regions and sets ShmAudioHeader::notify
// with no pipe to deliver the event, permanently suppressing the live engine's
// edge notifications. Every source degrades to the ~2.5 s keepalive — measured
// ~92% audio loss, with no error surfaced anywhere. Root-caused live
// 2026-08-17; the sweep is the fix.
//
// The sweep works because it assumes one product owns the machine's engines.
// A second product built on the same engine (ZComms, the standalone intercom)
// breaks that assumption twice over, and both breakages are the SAME defect
// class the sweep was written to prevent:
//
//   * Mutual termination. The intercom runs alongside OBS by design, and it
//     runs one engine per channel. An image-name sweep means each product's
//     launch kills the other's engines, and each channel's launch kills its
//     siblings.
//   * Ghost writers, again. Every shared name is currently hardcoded to the
//     "ZoomObsPlugin_" prefix — pipes (_P2E/_E2P) and regions
//     (_video/_audio/_share). Two products writing regions under one prefix
//     recreates 2026-08-17 exactly, except now by construction rather than by
//     accident.
//
// So the owner id has to do both jobs at once, and it has to be ONE mechanism:
// the string that namespaces the names is the same string that decides
// ownership for the sweep. Splitting those into two schemes is how they drift.
//
// WHAT IS PINNED HERE, AND WHAT IS NOT
//
// This header is pure string policy: name construction and the ownership
// predicate, with no platform calls, so both platforms' name shapes are
// testable from any host (CoreVideoEngineOwnerTest runs them on the Linux CI
// job, which never links the SDK, OBS or Qt). It does NOT do the wiring —
// engine-ipc.h's constants, the engine's --owner argument, and the sweep's
// process enumeration are separate changes, and the sweep in particular needs
// a per-owner record of which pid to kill rather than an image-name scan.
// Nothing below is reachable from production code until that lands.

#include <cstddef>
#include <string>

// The OBS plugin's owner id. This exact value reproduces every name shipping
// today, so it MUST NOT CHANGE: an installed plugin and engine are a matched
// pair over these names, and half-updated installs are routine (the engine
// lives in zoom-runtime\ and gets missed). Changing it silently stops a new
// plugin from finding an old engine, with no error either side.
static constexpr const char *kEngineOwnerObsPlugin = "ZoomObsPlugin";

// Owner ids are bounded and alphanumeric because they become OS object names:
// a POSIX socket PATH and a Windows pipe name. A '/' or a ".." would escape
// /tmp, and a '\' would escape the \\.\pipe\ namespace — an owner id arrives
// on a command line, so treat it as untrusted input rather than as a constant.
static constexpr size_t kEngineOwnerMaxLen = 32;

inline bool engine_owner_id_valid(const std::string &owner)
{
    if (owner.empty() || owner.size() > kEngineOwnerMaxLen) return false;
    for (unsigned char c : owner) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// An unusable owner id falls back to the OBS plugin's rather than propagating.
// A malformed pipe name does not fail loudly — it fails as "connect found
// nothing", which reads identically to "the engine did not start" and costs an
// afternoon. Falling back keeps the plugin working and confines the blast
// radius of a bad --owner to "shares the plugin's namespace".
inline std::string engine_owner_or_default(const std::string &owner)
{
    return engine_owner_id_valid(owner) ? owner : std::string(kEngineOwnerObsPlugin);
}

// ── Name construction ────────────────────────────────────────────────────────
//
// Both platforms' builders are unconditional. They are pure string joins, and
// making them compile everywhere means the Linux test job pins the Windows
// name shape too — the platform where getting it wrong costs a live show.

inline std::string engine_shm_prefix(const std::string &owner)
{
    return engine_owner_or_default(owner) + "_";
}

// Windows: \\.\pipe\<owner>_P2E
inline std::string engine_pipe_name_win(const std::string &owner, bool plugin_to_engine)
{
    return "\\\\.\\pipe\\" + engine_owner_or_default(owner) +
           (plugin_to_engine ? "_P2E" : "_E2P");
}

// POSIX: /tmp/<owner>_P2E.sock
inline std::string engine_socket_path_posix(const std::string &owner, bool plugin_to_engine)
{
    return "/tmp/" + engine_owner_or_default(owner) +
           (plugin_to_engine ? "_P2E" : "_E2P") + ".sock";
}

// ── The sweep predicate ──────────────────────────────────────────────────────
//
// An engine that reports no owner is a binary that predates --owner. It is
// treated as the OBS plugin's, which preserves 2026-08-17's protection exactly
// where it was earned: a half-updated install (new plugin, old engine in
// zoom-runtime\) is the normal way an untagged engine shows up, and the plugin
// must still be able to sweep it. The intercom inherits no such history, so it
// must NOT claim untagged engines — an untagged engine is never one of its own,
// and killing it would take out the user's OBS session mid-show.
inline bool engine_owner_claims(const std::string &launcher_owner,
                                const std::string &engine_owner)
{
    const std::string launcher = engine_owner_or_default(launcher_owner);
    const std::string engine   = engine_owner.empty()
                                     ? std::string(kEngineOwnerObsPlugin)
                                     : engine_owner_or_default(engine_owner);
    return launcher == engine;
}
