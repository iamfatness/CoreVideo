#pragma once
//
// talkback-nomination-dispatch.h — Task 5 fix round 2 (N5).
//
// The re-review's finding: F1 and N1 were never bugs in the pure state
// machine (src/talkback-nomination.h) -- that header's own mutation tests
// stayed green through both incidents. F1 was `talkback_nominate()` writing
// the wrong record; N1 is one more stage-name-to-transition mapping the
// caller could get wrong. The actual defect always lives in the WIRING: the
// one place that reads a report's "stage" (and, as of N1, its
// "channels_destroyed" field) and decides which pure transition to call.
// That wiring used to be inlined directly inside
// ZoomEngineClient::handle_event(), which requires the whole OBS plugin
// (obs-module.h, QTcpSocket, the engine IPC threads) to even compile, let
// alone exercise -- so nothing could pin "the refusal branch calls the
// WRONG transition" the way tests/talkback-nomination-test.cpp pins the
// transitions themselves. The re-review proved this concretely: routing the
// refusal branch to talkback_nomination_commit() instead of
// talkback_nomination_note_refused() (F1, reintroduced verbatim, in this
// exact mapping) left the full 66-test suite green.
//
// This header is the fix: the mapping itself, factored out to depend on
// nothing but Qt Core's JSON types (QJsonObject/QString) -- the same bar
// tests/zoom-control-parse-test.cpp already clears without OBS, a socket, or
// a thread. handle_event() calls this one function; a host test can drive
// it with the exact report shapes the engine emits and mutate the mapping
// to prove the test would catch a regression here again.
#include "talkback-nomination.h"

#include <QJsonObject>
#include <QString>

// Applies one "cmd":"talkback_nominate" report (already parsed into `obj`,
// with `stage` extracted) to `pending` (the in-flight attempt's staging
// area) and/or `confirmed` (the CONFIRMED plan). Mirrors
// engine/src/engine-talkback.cpp's report_nomination() call sites exactly:
//
//   "uncovered_private" / "unreachable"  -> stage into `pending` (name)
//   "plan"                               -> stage into `pending` (channels, all_talent_complete)
//   "nominate_done"                      -> commit `pending` into `confirmed` (this attempt succeeded)
//   "nominate", ok:false, channels_destroyed:true  -> `confirmed` is reset with a reason
//       (N1: the engine already destroyed the standing set before aborting)
//   "nominate", ok:false, channels_destroyed absent/false -> `confirmed` untouched, reason recorded
//       (an early refusal -- the standing set is still intact)
//
// Any other stage (e.g. "replacing", "create_channel") is intentionally a
// no-op here -- those are diagnostic-only trace lines handle_event() still
// logs verbatim before calling this, but they carry nothing this state
// machine needs to act on.
//
// C1 (CRITICAL, final whole-branch review 2026-08-26): every report is first
// matched against the ATTEMPT it belongs to. See
// TalkbackNominationPending::attempt -- a second nominate sent while an
// earlier ladder is still provisioning re-stages this slot, and without the
// id the earlier ladder's own nominate_done committed the LATER attempt's
// nominee list against the earlier ladder's channels.
inline void talkback_nomination_apply_report(TalkbackNominationPlan &confirmed,
                                              TalkbackNominationPending &pending,
                                              const QString &stage,
                                              const QJsonObject &obj)
{
    // WIRE COMPATIBILITY, stated deliberately: an engine built before this
    // fix emits no "attempt" field at all, and a DLL-only install (CLAUDE.md
    // calls that the canonical mistake here -- half the fixes in any release
    // are engine-side) makes that pairing routine rather than theoretical. A
    // report with NO attempt is therefore treated as matching, which is
    // exactly the pre-C1 behaviour: such an engine cannot tell attempts
    // apart, so neither can we, and refusing to act on its reports would
    // break nomination outright against it. Presence, not value, is the test
    // -- attempt ids are 1-based (ZoomEngineClient's counter pre-increments),
    // so a literal 0 could only come from a raw-pipe caller that sent none.
    const bool report_identifies_attempt = obj.contains(QLatin1String("attempt"));
    const uint32_t report_attempt =
        static_cast<uint32_t>(obj.value("attempt").toInt(0));
    if (report_identifies_attempt && report_attempt != pending.attempt) {
        // A SUPERSEDED attempt's report. It can never commit -- its staging
        // was overwritten by the attempt currently in the slot.
        //
        // THE TEST IS NOT WHOSE VERDICT IS NEWER, it is whether this report
        // PROVES THE ENGINE'S CHANNEL SET MOVED:
        //
        //   * It does (a completed ladder, or an abort carrying
        //     channels_destroyed) -> invalidate the confirmed plan, and carry
        //     the reason with it. Both branches below DO overwrite
        //     last_attempt_ok/last_attempt_reason, deliberately: an operator
        //     whose nomination has just ceased to exist needs to be told why
        //     that happened far more than they need the in-flight attempt's
        //     verdict, and the in-flight attempt's own terminal will overwrite
        //     these fields again when it lands. Leaving the plan standing
        //     instead is what key_on()'s pre-check would then trust -- a set
        //     Zoom no longer has, permanently. That is F2's symptom.
        //
        //   * It does not (a superseded refusal that destroyed nothing, any
        //     stage line) -> FULLY inert, diagnostics included. There is
        //     nothing to invalidate, so the only thing such a report could do
        //     is replace the staged attempt's verdict with an older and less
        //     relevant one, and make talkback_status describe the wrong
        //     attempt.
        if (stage == QLatin1String("nominate_done")) {
            talkback_nomination_note_superseded(confirmed, "superseded_nomination_completed");
        } else if (stage == QLatin1String("nominate") &&
                   !obj.value("ok").toBool(true) &&
                   obj.value("channels_destroyed").toBool(false)) {
            talkback_nomination_note_failed_after_destroy(confirmed,
                obj.value("reason").toString().toStdString());
        }
        return;
    }

    if (stage == QLatin1String("uncovered_private")) {
        talkback_nomination_note_uncovered(pending,
            obj.value("name").toString().toStdString());
    } else if (stage == QLatin1String("unreachable")) {
        talkback_nomination_note_unreachable(pending,
            obj.value("name").toString().toStdString());
    } else if (stage == QLatin1String("plan")) {
        talkback_nomination_note_plan(pending,
            static_cast<uint32_t>(obj.value("channels").toInt(0)),
            obj.value("all_talent_complete").toBool(true));
    } else if (stage == QLatin1String("nominate_done")) {
        // `channels` here is nominate_done's own count, which can differ
        // from the earlier "plan" stage's if a create failed partway (see
        // nomination_create_next() in engine/src/engine-talkback.cpp) --
        // though as of fix round 2 (N1) that specific path no longer reaches
        // "nominate_done" at all; it aborts through the branch below instead.
        talkback_nomination_commit(confirmed, pending,
            static_cast<uint32_t>(obj.value("channels").toInt(0)));
    } else if (stage == QLatin1String("nominate") && !obj.value("ok").toBool(true)) {
        const std::string reason = obj.value("reason").toString().toStdString();
        // N1: the explicit flag, not the reason string, decides the branch --
        // nominate()'s own early gate reports the identical "create_busy"
        // reason for a refusal that leaves the standing set untouched. See
        // talkback_nomination_note_failed_after_destroy()'s header comment
        // in talkback-nomination.h for why the string cannot be trusted here.
        if (obj.value("channels_destroyed").toBool(false)) {
            talkback_nomination_note_failed_after_destroy(confirmed, reason);
        } else {
            talkback_nomination_note_refused(confirmed, reason);
        }
    }
}
