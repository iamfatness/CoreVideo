// Unit test for the record-privilege handshake notice (src/zoom-privilege-
// notice.h), the fix for the 2026-09-05 live defect: starting the engine
// popped a "Zoom Join" error modal for the NORMAL first half of Zoom's
// record-privilege handshake (canStartRawRecording -> NoPermission -> the
// engine asks the host -> raw_media_ready once granted). Pure C++, no Qt/
// OBS/Zoom SDK dependency -- the actual notice-vs-error plumbing lives in
// ZoomEngineClient and cannot be host-tested the same way (see this
// project's CLAUDE.md on that class), so this file pins the one part of the
// fix that CAN be driven without it: which copy the operator sees, and for
// which report shape.
#include "zoom-privilege-notice.h"

#include <iostream>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string &name)
{
    if (!cond) {
        std::cerr << "FAIL: " << name << "\n";
        ++g_failures;
    }
}

int main()
{
    // ── The two exact wire strings the engine actually emits ────────────────
    // Copied verbatim from engine/src/main-macos.mm's handle_start_media() so
    // a change to either engine string that stops matching the classifier's
    // substring check fails HERE, not silently in the field.
    const std::string first_request_detail =
        "Raw recording needs local-recording permission. The meeting host "
        "must allow this participant to record.";
    const std::string still_pending_detail =
        "Local-recording permission was already requested and has not been "
        "granted.";

    // ── Classification routes each shape correctly ───────────────────────────
    check(!zoom_privilege_already_requested(first_request_detail),
          "first request: not classified as already-requested");
    check(zoom_privilege_already_requested(still_pending_detail),
          "still pending: classified as already-requested");

    check(zoom_privilege_notice_text(first_request_detail) ==
              zoom_privilege_notice_first_request(),
          "first request: routes to first-request copy");
    check(zoom_privilege_notice_text(still_pending_detail) ==
              zoom_privilege_notice_still_pending(),
          "still pending: routes to still-pending copy");

    // ── Absent/unrecognized detail fails toward the FIRST-request copy ──────
    // Same tolerance rule this codebase applies elsewhere (e.g. talkback's
    // "ABSENT MEANS NOT BLOCKED" for the "mic" field): a missing or unknown
    // detail must not be read as "the host already ignored a request", or an
    // engine that omits/changes this field would show the firmer, wrong copy
    // on every very first attempt.
    check(!zoom_privilege_already_requested(""),
          "empty detail: not classified as already-requested");
    check(zoom_privilege_notice_text("") == zoom_privilege_notice_first_request(),
          "empty detail: routes to first-request copy");
    check(!zoom_privilege_already_requested("some unrelated future detail text"),
          "unrecognized detail: not classified as already-requested");

    const std::string expected = "Waiting for the host to allow recording. Media will start automatically when permission is granted.";
    check(zoom_privilege_notice_text(first_request_detail) == expected, "automatic recovery guidance");
    check(zoom_privilege_notice_text(still_pending_detail) == expected, "repeat request is same wait episode");

    const auto denied = zoom_raw_media_state_notice("denied", "privilege_denied");
    check(denied.find("Ask the host") != std::string::npos, "denial has host action");
    check(zoom_raw_media_state_notice("starting", "privilege_granted") != denied, "grant replaces denial");
    check(zoom_raw_media_state_notice("waiting_permission", "privilege_request_timeout").find("timed out") != std::string::npos, "timeout distinguished from denial");
    check(zoom_raw_media_state_notice("active", "privilege_request_timeout").empty(), "late timeout cannot demote active media");
    check(zoom_raw_media_state_notice("failed", "start_raw_recording_failed").empty(), "terminal failure retains SDK error path");

    if (g_failures == 0) {
        std::cout << "zoom-privilege-notice-test: all checks passed\n";
        return 0;
    }
    std::cerr << "zoom-privilege-notice-test: " << g_failures
              << " check(s) failed\n";
    return 1;
}
