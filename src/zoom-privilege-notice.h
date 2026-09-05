#pragma once

#include <string>

// The record-privilege handshake, and why the first half of it is not an
// error (live defect, 2026-09-05: starting the engine popped a modal "raw
// recording failed" dialog on a session that was working exactly as
// designed).
//
// Zoom's canStartRawRecording() comes back NoPermission for a participant who
// has not been granted local recording; the engine's own response is to call
// requestLocalRecordingPrivilege() and report
// "cmd":"error","msg":"raw_media_start_failed","privilege_requested":true --
// see engine/src/main-macos.mm's handle_start_media(). The host sees a Zoom
// prompt, and once they grant it the SDK's own delegate callback restarts raw
// media and the engine reports "raw_media_ready". Nothing failed; this is the
// NORMAL first half of the handshake, and the operator's fix is just to click
// Start Engine again once the host has granted it.
//
// ZoomEngineClient::handle_event() already refuses to route this report into
// the join-failure/reconnect machinery -- a live incident once flipped a
// healthy joined session to Failed over exactly this report, which then gated
// start_engine, resubscription and recovery for the rest of the session (see
// the comment on that branch). What it got wrong downstream of that guard is
// the subject of this fix: it still set m_last_error and fired every
// registered error callback, which is what pops the "Zoom Join"
// QMessageBox -- the modal this header exists to stop.
//
// This header decides only the operator-facing TEXT and which of the two
// wire shapes a report is; the notice-vs-error plumbing (storage, callbacks,
// clearing on raw_media_ready) lives in zoom-engine-client.h/.cpp, which need
// the Qt JSON types this header stays free of, so the classification below
// can be host-tested without Qt or libobs.
//
// The engine's own retry loop (main-macos.mm's g_privilege_requested) asks
// the host only ONCE per meeting; every later cannot_start_raw_recording
// report is just a re-report of the SAME still-pending wait, and the engine
// marks it with a different "detail" string ("...was already requested and
// has not been granted.") rather than a separate machine-readable field. That
// substring is the only wire signal telling the two apart, so the
// classification below keys on it.

// Whether `detail` (the engine's raw_media_start_failed "detail" field) says
// the privilege was already asked for this meeting and the host has not
// granted it yet -- i.e. a REPEAT report of the same still-pending wait, not
// a fresh request just sent to the host.
inline bool zoom_privilege_already_requested(const std::string &detail)
{
    return detail.find("already requested") != std::string::npos;
}

// Operator-facing copy for each half of the handshake. Short and actionable
// on purpose (the owner's ask, live 2026-09-05): name the ACTION, not the
// error code. "Start Engine" is named literally because that is the exact
// button label the operator has to press again.
inline const char *zoom_privilege_notice_first_request()
{
    return "Waiting for the meeting host to approve recording. Once they "
           "approve it, click Start Engine again.";
}

// Firmer than the first-request copy on purpose: a repeat report means the
// host has not acted yet, so this is not a fresh ask -- it says so.
inline const char *zoom_privilege_notice_still_pending()
{
    return "Still waiting on the host to approve recording -- nothing will "
           "start until they do. Ask them to approve the Zoom prompt now, "
           "then click Start Engine again.";
}

// Picks the right copy for a raw_media_start_failed report's "detail" text.
inline std::string zoom_privilege_notice_text(const std::string &detail)
{
    return zoom_privilege_already_requested(detail)
               ? zoom_privilege_notice_still_pending()
               : zoom_privilege_notice_first_request();
}
