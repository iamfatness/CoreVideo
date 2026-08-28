#pragma once
//
// talkback-dock-state.h — what the dock's Talkback group shows, and which of
// its key buttons are live.
//
// Milestone 7, and a DELIBERATE, OWNER-APPROVED DEVIATION from the spec:
// docs/superpowers/specs/2026-08-24-zoom-talkback-design.md locks the keying
// surfaces to "Companion/Stream Deck, TCP/OSC control API, OBS hotkey. **Not**
// the dock", and its Dock section says "Configuration and tally only, by
// operator preference -- no talk button". The owner has since asked for a
// drivable dock, so the dock keys. Nothing else in that decision changed:
// identity is still by display name, keying still SELECTS a pre-provisioned
// channel, and every refusal still fails closed.
//
// Why a header with no Qt and no OBS in it, for UI code: every decision in
// here is a claim the operator acts on mid-show -- "this button is safe to
// press", "this person hears nothing", "this source is on air" -- and none of
// them could be exercised at all while they lived inside a QWidget that needs
// libobs, a Qt event loop and a running engine to construct. This is the same
// treatment (and the same reason) src/talkback-plan.h and
// src/talkback-nomination-dispatch.h already get; tests/talkback-dock-state-
// test.cpp drives it.
//
// WHAT THIS FILE DOES NOT KNOW. It decides from the CONFIRMED nomination plan
// (src/talkback-nomination.h) plus engine/meeting/key state. It cannot see the
// nomination ladder's in-flight progress -- the plugin is only ever told the
// finished plan -- so a button it reports enabled can still be refused by
// TalkbackController::key_on() (a source that is not active, an OBS audio
// setup talkback cannot use) or by the engine (`provisioning_incomplete`).
// That is why the dock renders the refusal reason as well as the buttons.
#include "talkback-nomination.h"
#include "talkback-plan.h"

#include <cstdint>
#include <string>
#include <vector>

// ── Key buttons ─────────────────────────────────────────────────────────────

struct TalkbackDockKeyButton {
    // Passed verbatim to TalkbackController::key_on(). "all"
    // (kTalkbackAllTalentTarget) or one nominee's display name -- never a Zoom
    // user id, which is meeting-scoped and points at nobody after a rejoin.
    std::string target;
    std::string label;
    bool        enabled = false;
    // Empty when enabled. Operator-facing: it goes in the button's tooltip, so
    // it must say which thing is wrong, not that talkback is unavailable.
    std::string reason;
    bool        all_talent = false;
};

// The state the dock has about keying at the moment it rebuilds its buttons.
struct TalkbackDockKeyContext {
    bool engine_running = false;
    bool in_meeting     = false;
    // TalkbackController's own view (its status_json()'s "open"), not the
    // dock's: a key opened over the control API disables the dock's other
    // buttons too, because key_on() refuses a second key outright.
    bool key_open = false;
    // Which target that open key is on. The button for THIS target stays
    // enabled -- it is the one being held, and disabling a pressed
    // QPushButton is how its released() signal gets lost (see zoom-dock.cpp).
    std::string open_target;
};

// One button per keyable target: "all" first, then every nominee in the order
// they were nominated.
//
// ENABLEMENT IS DELEGATED, NOT RE-DERIVED. The nomination half of the decision
// is talkback_target_known_unprovisioned() (src/talkback-plan.h) -- the exact
// predicate TalkbackController::key_on() refuses on -- so a button this
// function reports enabled can never be one key_on() would reject for that
// reason. Re-implementing the rule here is how the two would drift.
inline std::vector<TalkbackDockKeyButton> talkback_dock_key_buttons(
    const TalkbackNominationPlan &confirmed,
    const TalkbackDockKeyContext &ctx)
{
    std::vector<TalkbackDockKeyButton> buttons;

    const auto add = [&](const std::string &target, const std::string &label,
                         bool all_talent) {
        TalkbackDockKeyButton b;
        b.target     = target;
        b.label      = label;
        b.all_talent = all_talent;

        if (ctx.key_open && target != ctx.open_target) {
            b.reason = "another talkback key is open";
        } else if (!ctx.engine_running) {
            b.reason = "the Zoom engine is not running";
        } else if (!ctx.in_meeting) {
            b.reason = "not in a meeting";
        } else if (talkback_target_known_unprovisioned(
                       target, confirmed.requested, confirmed.uncovered_private)) {
            if (confirmed.requested.empty()) {
                b.reason = "no one has been nominated yet";
            } else {
                bool unreachable = false;
                for (const auto &u : confirmed.unreachable)
                    if (u == target) { unreachable = true; break; }
                b.reason = unreachable
                    ? "on no channel at all -- re-nominate a shorter list"
                    : "no private channel -- key All instead, or re-nominate";
            }
        } else {
            b.enabled = true;
        }
        buttons.push_back(std::move(b));
    };

    add(kTalkbackAllTalentTarget, "All talent", true);
    for (const auto &name : confirmed.requested)
        add(name, name, false);
    return buttons;
}

// ── The nomination outcome ──────────────────────────────────────────────────
//
// The budget outcome being visible AT NOMINATION TIME is the point of the
// whole reporting chain (src/talkback-plan.h's header comment: a shortfall is
// named, never swallowed, because it is invisible from the control room). So
// this renders every shortfall by name rather than a count.

struct TalkbackDockNominationReport {
    std::string headline;
    // Detail lines, each already operator-facing and safe to show verbatim.
    std::vector<std::string> lines;
    // True when something the operator asked for did not happen: a shortfall,
    // or a failed attempt. The dock colours the block on this.
    bool warn = false;
};

namespace talkback_dock_detail {

inline std::string join_names(const std::vector<std::string> &names)
{
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        out += names[i];
    }
    return out;
}

inline std::string plural(std::size_t n, const char *one, const char *many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace talkback_dock_detail

inline TalkbackDockNominationReport talkback_dock_nomination_report(
    const TalkbackNominationPlan &confirmed)
{
    using namespace talkback_dock_detail;
    TalkbackDockNominationReport report;

    if (!confirmed.done) {
        // Not "nothing happened": a failed-after-destroy or superseded
        // outcome also lands here, with done reset to false and the reason
        // kept (src/talkback-nomination.h). Saying "no channels" alone would
        // hide the WHY on exactly the paths that need it most.
        report.headline = "No talkback channels. Pick talent below and press "
                          "Nominate.";
    } else {
        const std::vector<std::string> covered =
            talkback_private_channel_names(confirmed.requested,
                                           confirmed.uncovered_private);
        report.headline =
            plural(confirmed.channels, "channel", "channels") + " in use of " +
            std::to_string(kTalkbackMaxChannels) + " for " +
            plural(confirmed.requested.size(), "nominee", "nominees") + ".";
        report.lines.push_back(
            covered.empty()
                ? std::string("Private channel: nobody.")
                : "Private channel: " + join_names(covered) + ".");
    }

    if (!confirmed.uncovered_private.empty()) {
        report.warn = true;
        report.lines.push_back(
            "No private channel (reach them by keying All): " +
            join_names(confirmed.uncovered_private) + ".");
    }
    if (!confirmed.unreachable.empty()) {
        report.warn = true;
        // Strictly worse than losing the private aside, and always a subset of
        // uncovered_private -- so it gets its own line rather than being
        // buried in the one above. See TalkbackPlan::unreachable.
        report.lines.push_back(
            "On no channel at all -- they hear nothing: " +
            join_names(confirmed.unreachable) + ".");
    }
    if (confirmed.done && !confirmed.all_talent_complete) {
        report.warn = true;
        report.lines.push_back(
            "All talent does not reach everyone: this list is larger than "
            "16 channels can fan out to.");
    }
    if (!confirmed.last_attempt_ok) {
        report.warn = true;
        // The last ATTEMPT, which can disagree with the confirmed plan above
        // without contradicting it (a refused re-nomination leaves the
        // standing channels exactly as they were). Said as a separate line
        // for that reason.
        report.lines.push_back(
            "Last Nominate was refused: " +
            (confirmed.last_attempt_reason.empty()
                 ? std::string("no reason reported")
                 : confirmed.last_attempt_reason) + ".");
    }
    return report;
}

// ── The program-track warning ───────────────────────────────────────────────
//
// The deferred half of the leak guarantee. The structural half holds
// unconditionally (a capture callback observes a source and cannot add it to
// any mix -- tests/talkback-isolation-test.cpp), but OBS enables all six mixer
// tracks on every new audio source by default, so a director who points
// talkback at the mic that is already on program puts the aside on air at full
// level through OBS's own routing, entirely outside CoreVideo's path. The tap
// already logs this at open() time (src/talkback-tap.cpp); this is the same
// advisory where the operator is actually looking, and BEFORE the key, not on
// it.

struct TalkbackDockTrackWarning {
    // True when the source has at least one mixer track enabled. "Enabled" is
    // not the same as "on air" -- obs_source_get_audio_mixers() reports which
    // tracks the source feeds, not which tracks are being recorded or
    // streamed right now, and this file has no way to know that. The wording
    // below is conditional for that reason.
    bool on_air_risk = false;
    // 1-based track numbers as OBS's Advanced Audio Properties shows them,
    // e.g. "1, 3".
    std::string tracks;
    std::string text;
};

// `mixers` is obs_source_get_audio_mixers()'s bitmask. Six tracks, matching
// both OBS's Advanced Audio Properties dialog and the identical loop in
// TalkbackTap::open().
inline TalkbackDockTrackWarning talkback_dock_track_warning(
    const std::string &source_name, uint32_t mixers)
{
    TalkbackDockTrackWarning w;
    if (source_name.empty()) {
        w.text = "No talkback source chosen. The safe pattern is a dedicated "
                 "audio source on an unused track.";
        return w;
    }
    for (int i = 0; i < 6; ++i) {
        if (!(mixers & (1u << i))) continue;
        if (!w.tracks.empty()) w.tracks += ", ";
        w.tracks += std::to_string(i + 1);
    }
    if (w.tracks.empty()) {
        w.text = "\"" + source_name + "\" is on no OBS track -- talkback only. "
                 "That is the safe pattern: a dedicated source on an unused "
                 "track.";
        return w;
    }
    w.on_air_risk = true;
    w.text = "\"" + source_name + "\" is on OBS track(s) " + w.tracks +
             ". If any of those are on air, the audience hears this talkback "
             "aside at full level. Uncheck them in Advanced Audio Properties, "
             "or use a dedicated source on an unused track.";
    return w;
}

// ── Live tally ──────────────────────────────────────────────────────────────

struct TalkbackDockTally {
    std::string text;
    // The key is open AND the engine has confirmed the channel. Anything less
    // is not "on air" and must not be shown as such -- the spec's own
    // requirement is that the tally reflects the engine's confirmed state,
    // never the plugin's intent.
    bool live = false;
    // Something failed and the operator has to act. Distinct from `live`.
    bool alert = false;
};

struct TalkbackDockSessionView {
    bool        key_open = false;
    std::string target;
    bool        engine_live = false;
    // The engine's own reason. Empty means "nothing reported yet", which is
    // NOT the same as success -- see ZoomEngineClient::TalkbackSessionStatus.
    std::string engine_reason;
    // The engine's own recovery hint for that reason, echoed rather than
    // inferred (`"recover":"re-nominate"` on a provisioning_incomplete
    // refusal). Empty when the engine offered none.
    std::string engine_recover;
    // From the engine's session_live line, as of the moment the key opened.
    // NOT refreshed while the key is held: the engine reports these once per
    // key press, so a talent who rejoins mid-press is not counted until the
    // next press. members_known is false when no session_live has arrived.
    bool     members_known = false;
    uint32_t members_present = 0;
    uint32_t members_total = 0;
};

inline TalkbackDockTally talkback_dock_tally(const TalkbackDockSessionView &s)
{
    TalkbackDockTally t;
    if (!s.key_open) {
        t.text = "Not keyed.";
        if (!s.engine_reason.empty() && !s.engine_live) {
            // The reason outlives the key that earned it (talkback_start()
            // clears it at the next press), so this is the last key's verdict
            // -- said as such, not as a current state.
            t.alert = true;
            t.text += " Last key: " + s.engine_reason + ".";
            if (!s.engine_recover.empty())
                t.text += " Recovery: " + s.engine_recover + ".";
        }
        return t;
    }

    const std::string target = s.target.empty() ? std::string("(no target)")
                                                : s.target;
    if (s.engine_live) {
        t.live = true;
        t.text = "LIVE to " + target;
        if (s.members_known) {
            t.text += " -- " + std::to_string(s.members_present) + " of " +
                      std::to_string(s.members_total) + " present";
        }
        t.text += ".";
        return t;
    }
    if (s.engine_reason.empty()) {
        t.text = "Keying " + target + " -- waiting for Zoom to confirm the "
                 "channel.";
        return t;
    }
    t.alert = true;
    t.text = "Key refused for " + target + ": " + s.engine_reason + ".";
    if (!s.engine_recover.empty())
        t.text += " Recovery: " + s.engine_recover + ".";
    return t;
}

// ── The dock's own lost-release backstop ────────────────────────────────────
//
// The dock passes needs_renewal = false to key_on(): its press and release are
// QPushButton signals on the Qt main thread, the same thread
// TalkbackController's QTimer runs evaluate()/key_off() on, so there is no
// transport between them to lose a release the way a socket can (see the
// renewal discussion at the top of src/talkback-key.h, which names exactly
// this class of surface).
//
// One in-process way a release CAN still go missing: QAbstractButton clears
// its pressed state on an EnabledChange without emitting released(), so
// disabling a held button would strand the key open. The dock does not disable
// a held button -- and this is the backstop for it either way. It compares the
// controller's open key against the widget's own current state, so a stalled
// UI thread cannot false-close a genuinely held key: while the thread is
// stalled this does not run at all, and once it resumes isDown() is accurate
// again. A latch is exempt by definition -- nothing is being held.
inline bool talkback_dock_release_lost(bool dock_owns_open_key,
                                       bool push_to_talk,
                                       bool button_down)
{
    return dock_owns_open_key && push_to_talk && !button_down;
}
