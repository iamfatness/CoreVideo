#pragma once

#include "zoom-reconnect-backoff.h"

#include <cstdint>
#include <string>

// Rules for retrying the engine's Zoom SDK init handshake after
// SDKERR_OTHER_SDK_INSTANCE_RUNNING. Pure logic, no OBS/Qt/Zoom-SDK
// dependency, so it is unit tested on the host (tests/zoom-sdk-init-retry-test.cpp).
//
// Why this exists
// ---------------
// Closing OBS leaves a ZoomObsEngine.exe behind for a while: the shutdown is
// clean (the plugin sends quit), but the Zoom SDK's own teardown runs long and
// outlives the OBS process. That leftover engine still holds the SDK, so the
// NEXT session's very first InitSDK fails with code 14 and the operator sees
// the engine request die for no visible reason.
//
// So this condition is transient and self-healing: waiting is the correct
// response, not failing and not killing a process that might be another OBS
// instance's LIVE engine. The wait is bounded — if the other instance is a real
// concurrent user of the SDK it will not go away, and the operator must be told
// that specifically rather than left clicking a button that cannot work.
//
// How long an orphan actually lives
// ---------------------------------
// Measured from the owner's OBS logs of 2026-08-10 (%APPDATA%\obs-studio\logs):
//
//   19:15:52.734  "Shutting down CoreVideo runtime"   (19-08-11.txt) — quit sent
//   19:16:31.346  after_init_sdk code=14              (19-16-15.txt) — STILL held
//   19:18:06.250  after_init_sdk code=0               (19-17-55.txt) — released
//
// That is a hard lower bound of 38.6s on one orphan's lifetime, and an upper
// bound of 133.5s; nothing probed the SDK in between, so the true figure is
// somewhere in that window. (An earlier reading of these logs claimed the
// orphan cleared in ~19s because a second click at 19:16:50 appeared to
// succeed. It did not: with the engine process still alive, ZoomEngineClient::
// start() early-returns on m_running and sends nothing — the 19:16:50 lines
// show a queued join and no "Launching ZoomObsEngine". The success at 19:18:06
// came from a different OBS process. Do not re-derive this bound from that.)
//
// The bound below is therefore calibrated on the 38.6s measurement, not on the
// bad 19s one. It cannot cover the whole 38.6–133.5s uncertainty band without
// making the operator stare at nothing for over two minutes, so exhaustion
// stays a real outcome — which is why the exhausted path tears the engine down
// and says something the operator can actually act on.
//
// SDK error code
// --------------
// 14 is SDKERR_OTHER_SDK_INSTANCE_RUNNING: the 15th enumerator of `enum
// SDKError` in zoom_sdk_def.h (SDKERR_SUCCESS = 0 is the only explicit value,
// so the index is the value). Verified against
// zoom-sdk-windows-7.1.5.43953/x64/h/zoom_sdk_def.h.
inline constexpr int kSdkErrOtherSdkInstanceRunning = 14;

// Bound: 11 retries at 2s, 4s, 8s, then 8s each = 78s of waiting in total.
// That is 2x the 38.6s orphan lifetime actually measured above, which is the
// only hard number we have. The 8s cap is deliberate: the operator's wait ends
// within ~8s (plus the monitor thread's ~1s tick) of the orphan really letting
// go, so a long total budget does not cost latency in the common case — it only
// buys patience in the rare one.
inline constexpr int    kSdkInitRetryMaxAttempts = 11;
inline constexpr double kSdkInitRetryBaseDelayMs = 2000.0;
inline constexpr double kSdkInitRetryMultiplier  = 2.0;
inline constexpr double kSdkInitRetryMaxDelayMs  = 8000.0;

struct SdkInitRetryDecision {
    // Replay the init command after delay_ms.
    bool     retry     = false;
    // This IS the transient collision, but the retry bound is used up: fail
    // with the specific, actionable message instead of the generic auth error.
    bool     exhausted = false;
    uint64_t delay_ms  = 0;
};

// `stage` / `code` come straight off the engine's
// {"cmd":"auth_fail","stage":...,"code":N} line. `attempts_so_far` is how many
// retries have already been scheduled for this engine session (0 on the first
// failure).
//
// Anything that is not the init-stage instance collision returns {false,false,0}
// so the caller keeps today's behaviour exactly. Restricting to stage "init" is
// deliberate: SDKERR_OTHER_SDK_INSTANCE_RUNNING is an InitSDK() result, and a
// code 14 arriving from the auth stages would mean something we have never
// observed and should not silently swallow.
inline SdkInitRetryDecision decide_sdk_init_retry(const std::string &stage,
                                                  int code,
                                                  int attempts_so_far)
{
    SdkInitRetryDecision decision;
    if (stage != "init" || code != kSdkErrOtherSdkInstanceRunning)
        return decision;
    if (attempts_so_far < 0) attempts_so_far = 0;
    if (attempts_so_far >= kSdkInitRetryMaxAttempts) {
        decision.exhausted = true;
        return decision;
    }
    decision.retry = true;
    // Same exponential-backoff-with-clamp math the reconnect manager uses.
    // Jitter is deliberately not applied (factor 1.0): the contended resource
    // is a single machine-local SDK instance, not a shared server, so there is
    // no thundering herd to spread out, and a fixed 2/4/8/8/8... schedule is
    // what support reads back out of the OBS log.
    decision.delay_ms = static_cast<uint64_t>(
        compute_backoff_delay_ms(attempts_so_far, kSdkInitRetryBaseDelayMs,
                                 kSdkInitRetryMultiplier, kSdkInitRetryMaxDelayMs,
                                 1.0));
    return decision;
}

// Total wall-clock time the retry schedule will spend waiting before it gives
// up. Derived from decide_sdk_init_retry() rather than hand-summed so it cannot
// drift away from the schedule it describes.
inline uint64_t sdk_init_retry_total_budget_ms()
{
    uint64_t total = 0;
    for (int attempt = 0; attempt < kSdkInitRetryMaxAttempts; ++attempt) {
        const SdkInitRetryDecision d =
            decide_sdk_init_retry("init", kSdkErrOtherSdkInstanceRunning, attempt);
        if (!d.retry) break;
        total += d.delay_ms;
    }
    return total;
}

// The operator-facing message for the exhausted case. It must name the cause
// (another Zoom SDK instance) and an action that actually works, because the
// generic auth_fail text tells the operator nothing they can act on.
//
// "Request the engine again" is only truthful because the caller tears the
// engine process down before showing this: while the engine process is alive,
// ZoomEngineClient::start() early-returns on m_running and a second request
// does nothing at all. If that teardown is ever removed, this text becomes a
// lie and must change with it.
inline std::string sdk_init_other_instance_message(uint64_t waited_ms)
{
    return "Zoom engine could not start: another Zoom SDK instance is already "
           "running (SDKERR_OTHER_SDK_INSTANCE_RUNNING, code " +
           std::to_string(kSdkErrOtherSdkInstanceRunning) +
           "). This is usually a ZoomObsEngine.exe left over from a previous OBS "
           "session — the Zoom SDK's shutdown can outlive OBS by a minute or "
           "more — or another app using the Zoom Meeting SDK. CoreVideo retried "
           "for " + std::to_string(waited_ms / 1000) +
           "s and has shut its own engine down, so requesting the Zoom engine "
           "again will launch a fresh one. If it fails the same way, wait for "
           "the leftover ZoomObsEngine.exe to disappear from Task Manager (or "
           "end it there) before requesting again.";
}
