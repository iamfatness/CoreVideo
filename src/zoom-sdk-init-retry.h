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
// Closing OBS mid-meeting can leave a ZoomObsEngine.exe orphaned. That orphan
// still holds the Zoom Meeting SDK, so the NEXT session's very first InitSDK
// call fails and the operator sees the engine request die for no visible
// reason; clicking again a few seconds later works, because by then the orphan
// has exited. Observed in the field (OBS log 2026-08-10 19:16): init failed at
// 19:16:31 with code 14 and the same request succeeded at 19:16:50, ~19s later.
//
// So this condition is transient and self-healing: waiting is the correct
// response, not failing and not killing a process that might be another OBS
// instance's LIVE engine. The wait is bounded — if the other instance is a real
// concurrent user of the SDK it will not go away, and the operator must be told
// that specifically rather than left clicking a button that cannot work.
//
// SDK error code
// --------------
// 14 is SDKERR_OTHER_SDK_INSTANCE_RUNNING: the 15th enumerator of `enum
// SDKError` in zoom_sdk_def.h (SDKERR_SUCCESS = 0 is the only explicit value,
// so the index is the value). Verified against
// zoom-sdk-windows-7.1.5.43953/x64/h/zoom_sdk_def.h.
inline constexpr int kSdkErrOtherSdkInstanceRunning = 14;

// Bound: 5 retries at 2s, 4s, 8s, 8s, 8s = 30s of waiting in total. That
// comfortably covers the ~19s orphan lifetime observed in the field while
// staying short enough that an operator who really does have another SDK app
// running gets a real answer inside a commercial break rather than a hang.
inline constexpr int    kSdkInitRetryMaxAttempts = 5;
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
    // no thundering herd to spread out, and a fixed 2/4/8/8/8 schedule is what
    // support reads back out of the OBS log.
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
// (another Zoom SDK instance) and the action, because the generic auth_fail
// text tells the operator nothing they can act on.
inline std::string sdk_init_other_instance_message(uint64_t waited_ms)
{
    return "Zoom engine could not start: another Zoom SDK instance is already "
           "running (SDKERR_OTHER_SDK_INSTANCE_RUNNING, code " +
           std::to_string(kSdkErrOtherSdkInstanceRunning) +
           "). This is usually a ZoomObsEngine.exe left over from a previous OBS "
           "session, or another app using the Zoom Meeting SDK. CoreVideo waited " +
           std::to_string(waited_ms / 1000) +
           "s for it to exit and it is still there. Wait for the other instance "
           "to close (or end ZoomObsEngine.exe in Task Manager), then request "
           "the Zoom engine again.";
}
