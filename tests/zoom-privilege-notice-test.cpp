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

    // ── The two notices are distinct copy, not the same string twice ────────
    // If a future edit collapsed both branches to identical text, the "handle
    // both, and the second warrants firmer copy" requirement this fix was
    // written against would silently stop being true.
    const std::string first_copy = zoom_privilege_notice_first_request();
    const std::string still_copy = zoom_privilege_notice_still_pending();
    check(!first_copy.empty(), "first-request copy non-empty");
    check(!still_copy.empty(), "still-pending copy non-empty");
    check(first_copy != still_copy, "the two notices are distinct copy");

    // ── Copy names the ACTION, not the error code ────────────────────────────
    // The owner's ask, verbatim: "capture this error and not display an
    // error... you just hit it again". Neither string may leak a raw SDK
    // error code digit, and both must name the actual button the operator
    // has to press.
    auto has_digit = [](const std::string &s) {
        return s.find_first_of("0123456789") != std::string::npos;
    };
    check(!has_digit(first_copy), "first-request copy: no error code digits");
    check(!has_digit(still_copy), "still-pending copy: no error code digits");
    check(first_copy.find("Start Engine") != std::string::npos,
          "first-request copy: names the Start Engine action");
    check(still_copy.find("Start Engine") != std::string::npos,
          "still-pending copy: names the Start Engine action");

    // ── The still-pending copy is the firmer one ─────────────────────────────
    // A repeat report means the host has not acted yet; the copy should say
    // so rather than reading like a brand-new ask. Pinned narrowly (not by
    // wording, which is free to change) as: it says the host has not granted
    // it / nothing will start, which the first-request copy does not.
    check(still_copy.find("Still waiting") != std::string::npos ||
              still_copy.find("nothing will") != std::string::npos,
          "still-pending copy: reads as a repeat wait, not a fresh ask");

    if (g_failures == 0) {
        std::cout << "zoom-privilege-notice-test: all checks passed\n";
        return 0;
    }
    std::cerr << "zoom-privilege-notice-test: " << g_failures
              << " check(s) failed\n";
    return 1;
}
