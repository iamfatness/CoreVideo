#pragma once
//
// talkback-dock-state.h — what the Talkback dock shows, and which of its key
// buttons are live.
//
// The Talkback surface began as a group box at the bottom of the Zoom Control
// dock. After its first live render the owner's verdict was that it needed to
// be its own panel -- keying is the mid-show action and it was sharing a column
// with join fields and routing -- so it is now a standalone dock
// (src/zoom-talkback-panel.cpp, registered as "ZoomTalkbackDock"). Every
// decision below moved across unchanged; only the layout and the wording did.
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

#include <algorithm>
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

// THE ONE RECORD OF THE KEY THAT IS CURRENTLY OPEN. Fix round 1 (M1, Major):
// the dock used to decide "does this press close the held key?" from the Latch
// CHECKBOX at press time while deciding "does this release close it?" from the
// mode captured when the key opened. Unchecking Latch while a latched key was
// live therefore made it un-closeable from the dock: the toggle-off branch was
// skipped, key_on() refused ("A talkback key is already open"), and the
// following released() bailed out because the captured mode still said latch --
// leaving the director live to talent with the surface they are looking at
// answering with an error. Every decision below now reads `latched` from HERE,
// so there is one mode per key and it is the one that key was opened with.
struct TalkbackDockOpenKey {
    // A key is open somewhere -- TalkbackController's own view (its
    // status_json()'s "open"), not the dock's intent.
    bool open = false;
    // ...and this dock is the surface that opened it. A key opened over the
    // control API or Companion is NOT the dock's to close or to hold: key_on()
    // refuses a second key outright, so every dock button must refuse while one
    // is live (fix round 1, m3).
    bool dock_owned = false;
    std::string target;
    // The mode CAPTURED AT THE OPENING PRESS, never re-read from the checkbox.
    bool latched = false;
};

// The state the dock has about keying at the moment it rebuilds its buttons.
struct TalkbackDockKeyContext {
    // See TalkbackDockSessionView::platform_supported. Refused AHEAD of
    // engine/meeting/source, all three of which are also false on a macOS box
    // -- each would be a true statement that sends the operator to debug
    // something that is not the problem.
    bool platform_supported = true;
    bool engine_running = false;
    bool in_meeting     = false;
    // An OBS audio source is selected. Without one key_on() cannot open a tap
    // at all, so a button offered here could only ever refuse (fix round 1,
    // m4).
    bool source_chosen  = false;
    TalkbackDockOpenKey open;
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

        // The button for a key THIS DOCK is holding stays enabled, and is
        // checked before everything else: it is the operator's only way to let
        // go of a latch, and disabling a pressed QPushButton is itself how a
        // release gets lost (see talkback_dock_release_lost()).
        const bool held_here = ctx.open.open && ctx.open.dock_owned &&
                               ctx.open.target == target;
        if (held_here) {
            b.enabled = true;
        } else if (!ctx.platform_supported) {
            // Below held_here and nowhere else. Never disabling a button the
            // operator is holding is the stronger law (a disabled QPushButton
            // drops `down` without emitting released(), which strands the key)
            // and it costs nothing to honour here: with no talkback engine
            // there is no key to hold, so this ordering is unreachable rather
            // than a trade.
            b.reason = "talkback is not available on macOS yet";
        } else if (ctx.open.open && !ctx.open.dock_owned) {
            // m3: not "another talkback key is open" -- naming the surface is
            // what tells the operator that pressing here cannot help and that
            // the thing holding the channel is somewhere else.
            b.reason = "another surface (Companion or the control API) holds "
                       "the talkback key";
        } else if (ctx.open.open) {
            b.reason = "another talkback key is open";
        } else if (!ctx.engine_running) {
            b.reason = "the Zoom engine is not running";
        } else if (!ctx.in_meeting) {
            b.reason = "not in a meeting";
        } else if (!ctx.source_chosen) {
            b.reason = "choose the OBS audio source you talk through first";
        } else if (talkback_target_known_unprovisioned(
                       target, confirmed.requested, confirmed.uncovered_private)) {
            if (confirmed.requested.empty()) {
                b.reason = "no one has a channel yet";
            } else {
                bool unreachable = false;
                for (const auto &u : confirmed.unreachable)
                    if (u == target) { unreachable = true; break; }
                b.reason = unreachable
                    ? "on no channel at all. Assign channels for a shorter "
                      "list."
                    : "no private channel. Key All talent instead, or assign "
                      "channels again.";
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

// ── The intercom grid: one cell per person ──────────────────────────────────
//
// THE REDESIGN THIS IS THE POINT OF (owner, after running the first dock live
// with seven talent: "need to rethink how this works and how it will look").
// The old panel showed the same people TWICE -- a key button each, and a tick
// box each -- so the surface grew twice as fast as the cast, and a single
// 28-character display name flipped the adaptive grid to one full-width
// column and built a 400 px tower of buttons for seven people. A 24-person
// show was not survivable.
//
// The model is a broadcast intercom panel (Clear-Com/RTS), because that is
// what the operator already is: ONE grid, one compact cell per person, and
// the cell is both the status display and the talk key. The tick-box list
// moves behind an [Edit talent] toggle (talkback_dock_edit_mode() above), so
// there is exactly one list of people on screen at a time.
//
// WHAT A CELL DECIDES, AND WHAT IT DOES NOT. The state below is about the
// PERSON -- can they hear me if I press this. Whether the cell may be pressed
// at all is still talkback_dock_key_buttons()' answer and is not re-derived
// here: that function delegates to talkback_target_known_unprovisioned(), the
// exact predicate TalkbackController::key_on() refuses on, and re-deciding
// enablement from a second, presence-flavoured rule is how a button the dock
// shows as live would become one key_on() rejects. So a cell carries the
// button's `enabled` and `reason` verbatim, and adds a state of its own.
//
// The consequence is deliberate and worth stating: with the engine stopped,
// or outside a meeting, or with no talk source chosen, every cell is disabled
// with the global reason in its tooltip while its state line still describes
// the PERSON. "Ready, and you cannot press it right now" is two true facts,
// not a contradiction, and the banner above the grid says which.
enum class TalkbackDockCellState {
    // Has a channel, and nothing says they are not in it. Also the state for
    // "we have not been told anything about this person" -- see
    // TalkbackPersonPresence::Unknown for why absence of evidence is not
    // painted as evidence of absence.
    Ready,
    // A key THIS DOCK is holding, that the ENGINE has confirmed, on THIS
    // target. Never the plugin's intent alone -- the C2 rule.
    OnAir,
    // Nominated but with no private channel of their own: the budget was
    // short, or the plan is not confirmed yet.
    NoChannel,
    // Nothing reaches them: their client reported no talkback support, or
    // the plan put them outside even the all-talent fan-out.
    Unreachable,
    // They have a channel and are NOT in it. The 2026-08-29 live case: every
    // invite refused SDKERR_WRONG_USAGE because the person was in a different
    // breakout room from the engine.
    NotInChannel,
};

struct TalkbackDockCell {
    // Verbatim from TalkbackDockKeyButton -- never re-derived here.
    std::string target;
    std::string label;
    bool        enabled = false;
    std::string reason;
    bool        all_talent = false;

    TalkbackDockCellState state = TalkbackDockCellState::Ready;
    // The small second line on the cell. Short enough to survive a 118 px
    // column; the sentence that explains it is `hint`, which the panel hangs
    // in the tooltip.
    std::string state_line;
    // Empty when there is nothing to add beyond `reason`.
    std::string hint;
};

// PRECEDENCE, in the order the checks run, and each is load-bearing:
//
//   1. ON AIR beats everything. A live key is the one fact on this panel an
//      operator must never have to reason about, and every other state is
//      about whether a key WOULD work.
//   2. Unreachable beats no-channel. An unreachable person is always also
//      uncovered_private (TalkbackPlan::unreachable is a strict subset), so
//      the generic "no channel, assign again" would otherwise win and send
//      the operator to re-assign channels for someone no assignment reaches.
//   3. No channel beats not-in-channel. There is no channel to be in.
//   4. Not-in-channel beats ready, which is the whole reason this exists.
inline TalkbackDockCellState talkback_dock_cell_state(
    const std::string &target, bool all_talent,
    const TalkbackNominationPlan &confirmed, bool live_here,
    TalkbackPersonPresence presence)
{
    if (live_here) return TalkbackDockCellState::OnAir;

    // All talent is a fan-out, not a person: it has no client to lack
    // support and no membership of its own to be absent from. Its only
    // failure is the plan's ("nobody has a channel yet", or a fan-out the
    // 16-channel cap could not complete).
    if (all_talent) {
        if (talkback_target_known_unprovisioned(target, confirmed.requested,
                                                confirmed.uncovered_private))
            return TalkbackDockCellState::NoChannel;
        if (confirmed.done && !confirmed.all_talent_complete)
            return TalkbackDockCellState::Unreachable;
        return TalkbackDockCellState::Ready;
    }

    if (presence == TalkbackPersonPresence::NoTalkback)
        return TalkbackDockCellState::Unreachable;
    for (const auto &u : confirmed.unreachable)
        if (u == target) return TalkbackDockCellState::Unreachable;

    if (talkback_target_known_unprovisioned(target, confirmed.requested,
                                            confirmed.uncovered_private))
        return TalkbackDockCellState::NoChannel;

    if (presence == TalkbackPersonPresence::NotInChannel)
        return TalkbackDockCellState::NotInChannel;

    return TalkbackDockCellState::Ready;
}

// The words on the cell. Kept beside the state so a new state cannot ship
// without one, and plain ASCII because these sources carry no BOM and the
// build passes MSVC no /utf-8 (the same rule the banner's tally dot follows).
// REVIEW ROUND 1, m7: `all_talent` is a parameter because the ALL-TALENT cell
// can legitimately reach Unreachable -- talkback_dock_cell_state() returns it
// when a completed plan could not finish the fan-out (`confirmed.done &&
// !confirmed.all_talent_complete`, i.e. the 16-channel cap) -- and the
// person-shaped copy for that state says "no talkback" over a hint about
// "their Zoom client". ALL TALENT HAS NO CLIENT. The state is right; only the
// words were wrong, because one enum value legitimately means two different
// things depending on whether the cell is a person or a fan-out.
inline std::string talkback_dock_cell_state_line(
    TalkbackDockCellState state, const TalkbackNominationPlan &confirmed,
    bool all_talent = false)
{
    switch (state) {
    case TalkbackDockCellState::OnAir:        return "ON AIR";
    case TalkbackDockCellState::Unreachable:
        // Not "no talkback" for the fan-out: what is short is the PLAN, which
        // the operator can act on, not a property of anybody's client.
        return all_talent ? "some missed" : "no talkback";
    case TalkbackDockCellState::NotInChannel: return "not in channel";
    case TalkbackDockCellState::NoChannel:
        // A ladder that has not reported a terminal yet is genuinely still
        // working; saying "no channel" there would send the operator to
        // re-assign something that is mid-assignment.
        return confirmed.done ? "no channel" : "assigning...";
    case TalkbackDockCellState::Ready:        break;
    }
    return "ready";
}

// The sentence behind the two words, for the tooltip. Empty when the state
// line already says everything.
//
// The not-in-channel hint names BREAKOUT ROOMS specifically, which is not a
// guess: it is what happened on 2026-08-29, and Zoom's talkback reaches only
// the room the inviting client is in. It is worded as a possibility ("may
// be") because the plugin sees a refused invite, not a room list.
inline std::string talkback_dock_cell_hint(TalkbackDockCellState state,
                                           bool all_talent = false)
{
    // REVIEW ROUND 1, m7: see talkback_dock_cell_state_line() above. The
    // all-talent cell reaches Unreachable through the PLAN's fan-out
    // shortfall, never through a client's capability, so it needs the plan's
    // sentence and an action -- "their Zoom client reported no talkback
    // support" is a claim about somebody who does not exist.
    if (all_talent && state == TalkbackDockCellState::Unreachable) {
        return "The channel budget could not cover everybody, so this key "
               "does not reach all of them. Assign channels again with fewer "
               "people, or key the missing talent individually.";
    }
    switch (state) {
    case TalkbackDockCellState::NotInChannel:
        return "They have a channel but are not in it. They may be in a "
               "different breakout room -- talkback reaches only the room the "
               "engine is in.";
    case TalkbackDockCellState::Unreachable:
        return "Nothing reaches them: their Zoom client reported no talkback "
               "support, or the channel budget could not cover them.";
    case TalkbackDockCellState::OnAir:
    case TalkbackDockCellState::NoChannel:
    case TalkbackDockCellState::Ready:
        break;
    }
    return {};
}

// One cell per keyable target, in talkback_dock_key_buttons()' own order
// (all-talent first, then nominees). `live_target` is the target the BANNER
// is calling ON AIR and this dock owns -- empty when neither is true, so the
// C2 rule (live requires the engine's confirmation, never the plugin's
// intent) and the ownership rule are both decided once, by the caller, and
// not a second time here.
inline std::vector<TalkbackDockCell> talkback_dock_cells(
    const TalkbackNominationPlan &confirmed,
    const TalkbackDockKeyContext &ctx,
    const TalkbackChannelPresence &presence,
    const std::string &live_target)
{
    std::vector<TalkbackDockCell> cells;
    for (const auto &b : talkback_dock_key_buttons(confirmed, ctx)) {
        TalkbackDockCell cell;
        cell.target     = b.target;
        cell.label      = b.label;
        cell.enabled    = b.enabled;
        cell.reason     = b.reason;
        cell.all_talent = b.all_talent;
        cell.state = talkback_dock_cell_state(
            b.target, b.all_talent, confirmed,
            !live_target.empty() && live_target == b.target,
            talkback_presence_for(presence, b.target));
        cell.state_line = talkback_dock_cell_state_line(cell.state, confirmed,
                                                        cell.all_talent);
        cell.hint       = talkback_dock_cell_hint(cell.state, cell.all_talent);
        cells.push_back(std::move(cell));
    }
    return cells;
}

// ── The talent list ─────────────────────────────────────────────────────────
//
// The tick-box list the operator picks talent from, and the diff that decides
// when the widget behind it is thrown away and rebuilt.
//
// WHY THE ORDER IS OURS AND NOT THE ROSTER'S -- the live defect this section
// exists for. Reported by the operator mid-show, 2026-08-29, in a Zoom Events
// production with breakout rooms: "moving from room to room the nomination
// list doesn't update". The roster cache the dock reads was verified fresh at
// the time (the control API's list_participants reads the same
// ZoomEngineClient::roster() and showed the new room's people), and the row
// content was right. What was wrong was the REBUILD GATE: the signature it
// compared was built by walking the roster in the order the Zoom SDK's
// GetParticipantsList() happened to return it, so a roster that had merely
// been REORDERED -- same people, same presence -- produced a different
// signature and a full rebuild.
//
// The panel ticks at 100 ms and the engine re-sends the roster on all five of
// Zoom's roster callbacks (two of which fire on every mute and camera toggle
// by anyone in the meeting), so in a busy room that is a QListWidget::clear()
// plus a re-add several times a second. From the operator's seat that is not a
// list that refreshes, it is a list that CANNOT BE USED: every rebuild resets
// the scroll position, and a tick-box click that spans one -- an ordinary
// click is 80-150 ms -- lands its press on an item that no longer exists by
// the time the release arrives, so the tick never registers. Hence "doesn't
// update": the operator's own edits were being thrown away, not the roster's.
//
// So the rows are ordered HERE, deterministically, from content alone:
// everyone present (sorted), then the ticked names who have left (sorted). Two
// consequences are deliberate:
//   * the signature is now a function of the SET, so a reorder cannot rebuild;
//   * the visible order stops moving under the operator's cursor while they
//     are ticking, which the roster's own order did on every mute.
// It also makes the nominee list this feature sends deterministic: with the
// channel budget short, WHICH names get a private channel is decided by list
// order (talkback_plan(), src/talkback-plan.h), and roster order made that
// arbitrary and re-rollable on any roster event.

struct TalkbackNomineeRow {
    // The identity, and the only thing ever sent to the engine. The row's
    // visible TEXT can also say "(not in the meeting)", which is why the
    // widget carries this separately in Qt::UserRole.
    std::string name;
    // In the meeting right now. False rows are ticked names who have left.
    bool present = false;
    bool checked = false;
};

// What the dock's list widget currently shows, and what it was built from.
struct TalkbackNomineeListState {
    std::vector<TalkbackNomineeRow> rows;
    // The signature of the last build. Deliberately not derived on demand: it
    // is the record of what is ON SCREEN, which `rows` alone cannot be after
    // the operator has ticked something the widget knows about and this does
    // not.
    std::string signature;
};

// A TICKED NAME THAT HAS LEFT THE ROSTER STAYS ON THE LIST. Nominating
// somebody who is not here right now is meaningful -- the engine re-resolves
// nominations by name on every roster change and invites them when they arrive
// (resolve_roster_change(), engine/src/engine-talkback.cpp) -- so dropping the
// row would silently drop them from the next press, on the exact path where a
// talent has just disconnected and the director is re-assigning to fix
// something else.
//
// `roster_names` may repeat a name and may arrive in any order; both are
// normalised here. Someone with no display name cannot be addressed at all and
// is expected to have been dropped by the caller.
inline std::vector<TalkbackNomineeRow> talkback_nominee_rows(
    const std::vector<std::string> &roster_names,
    const std::vector<std::string> &checked_names)
{
    const auto ticked = [&checked_names](const std::string &name) {
        return std::find(checked_names.begin(), checked_names.end(), name) !=
               checked_names.end();
    };

    std::vector<std::string> present(roster_names);
    std::sort(present.begin(), present.end());
    present.erase(std::unique(present.begin(), present.end()), present.end());

    std::vector<std::string> absent;
    for (const auto &name : checked_names) {
        if (name.empty()) continue;
        if (std::binary_search(present.begin(), present.end(), name)) continue;
        absent.push_back(name);
    }
    std::sort(absent.begin(), absent.end());
    absent.erase(std::unique(absent.begin(), absent.end()), absent.end());

    std::vector<TalkbackNomineeRow> rows;
    rows.reserve(present.size() + absent.size());
    for (const auto &name : present)
        rows.push_back(TalkbackNomineeRow{name, true, ticked(name)});
    for (const auto &name : absent)
        rows.push_back(TalkbackNomineeRow{name, false, true});
    return rows;
}

// What a rebuild is gated on: presence and name, nothing else. The tick state
// is NOT in here on purpose -- the operator setting a box must not cost them
// the widget they are setting it in.
inline std::string talkback_nominee_signature(
    const std::vector<TalkbackNomineeRow> &rows)
{
    std::string signature;
    for (const auto &row : rows) {
        signature += row.present ? "+" : "-";
        signature += row.name;
        signature += '\n';
    }
    return signature;
}

// One tick of the list. `checked_names` comes from the WIDGET, which is the
// only place the operator's ticks live; returns true when the caller must
// throw the widget's items away and re-add them from `state.rows`.
inline bool talkback_nominee_list_refresh(
    TalkbackNomineeListState &state,
    const std::vector<std::string> &roster_names,
    const std::vector<std::string> &checked_names)
{
    auto rows = talkback_nominee_rows(roster_names, checked_names);
    const std::string signature = talkback_nominee_signature(rows);
    if (signature == state.signature) {
        // No rebuild, but the widget's ticks have moved on without us; keep
        // this side honest rather than letting it describe a stale screen.
        state.rows = std::move(rows);
        return false;
    }
    state.rows = std::move(rows);
    state.signature = signature;
    return true;
}

// ── How much room the list and the key grid need ────────────────────────────
//
// Two sizing decisions that were wrong on screen in the owner's first look at
// the standalone dock (2026-08-29, "UI still doesn't feel polished"), and are
// arithmetic rather than painting -- so they are decided here, where they can
// be exercised, and only APPLIED by the panel.

// The talent list is sized in ROWS, never in pixels guessed from a font: five
// people must show five whole rows. Below the minimum an almost-empty list
// collapses to a sliver that does not read as a list at all; past the maximum
// it would push the key buttons -- the one thing on this dock that must never
// move -- off the bottom, so it scrolls instead.
constexpr int kTalkbackNomineeMinRows = 3;
constexpr int kTalkbackNomineeMaxRows = 6;

inline int talkback_dock_nominee_visible_rows(std::size_t row_count)
{
    if (row_count < static_cast<std::size_t>(kTalkbackNomineeMinRows))
        return kTalkbackNomineeMinRows;
    if (row_count > static_cast<std::size_t>(kTalkbackNomineeMaxRows))
        return kTalkbackNomineeMaxRows;
    return static_cast<int>(row_count);
}

// How many person cells fit across.
//
// THE DEFECT THIS REPLACES, and why the answer is never one. The first grid
// was hard-coded two-up and sized to the WIDEST label, so one long real name
// ("Ronny Hofsoy, Tromso, Norway", 2026-08-29) dropped the whole grid to a
// single full-width column: seven people became a 400 px tower of buttons,
// and a 24-person show would have been unusable. Sizing every cell to the
// longest name in the room is the mistake -- it lets one person's Zoom
// display name decide the layout for everybody.
//
// So the cell is sized to a MINIMUM READABLE WIDTH instead, and a name that
// does not fit is elided (end-elide only, full name in the tooltip). The
// floor of two columns is the load-bearing part: this grid is a status
// display as much as a control surface, and a column of 24 stacked cells is
// not scannable at a glance whatever the dock width. Three when the room is
// there, two otherwise, never one and never zero.
constexpr int kTalkbackDockCellMinPx = 118;

// ...AND WHY 118 IS ONLY A FLOOR (operator's high-DPI screen, 2026-08-29).
// That number was derived from an offscreen render at 1:1 font metrics, and
// on a display that resolves the cell font larger it stops describing "the
// narrowest cell a name is still readable in" and starts describing "a cell
// that fits four characters". Every pixel budget on this panel that was
// derived once and spent later was wrong on that screen; this one is now
// MEASURED at decision time instead -- the caller passes the width of a gauge
// string in the live label's own QFontMetrics and the cell's own measured
// chrome, and the constant survives only as the floor beneath them.
//
// The gauge is a lower-case run because that is what a display name mostly is,
// and one capital because that is what it starts with. Ten characters: fewer
// and two names that differ late read alike, more and a 320 px dock could not
// hold two columns at any font size.
constexpr const char *kTalkbackDockCellGauge = "Wnnnnnnnnn";

inline int talkback_dock_cell_min_px(int gauge_text_px, int chrome_px,
                                     int floor_px)
{
    const int measured = std::max(0, gauge_text_px) + std::max(0, chrome_px);
    return std::max(floor_px, measured);
}

inline int talkback_dock_cell_columns(int available_px, int min_cell_px,
                                      int gap_px)
{
    if (available_px <= 0 || min_cell_px <= 0) return 2;
    if (available_px >= 3 * min_cell_px + 2 * gap_px) return 3;
    return 2;
}

// ── Edit mode ───────────────────────────────────────────────────────────────
//
// The grid area shows EITHER the intercom grid or the talent checklist, never
// both. That is the whole point of it: before this, the same seven people
// appeared twice on one panel -- once as key buttons, once as tick boxes --
// and both lists grew linearly with the cast.
//
// Editing is never allowed to cost the operator a key they are holding.
// Hiding a held button strands it (the latch loses its only close affordance;
// a push-to-talk loses its release and waits on
// talkback_dock_release_lost()), so a key that is open anywhere -- this
// dock's or another surface's -- forces the grid back on screen. Keying wins
// over editing, always.
inline bool talkback_dock_edit_mode(bool requested,
                                    const TalkbackDockOpenKey &open)
{
    return requested && !open.open;
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
    // Names past kTalkbackDockNameListMax are ELIDED here -- see `tooltip`,
    // which always carries every one of them.
    std::vector<std::string> lines;
    // The same report with no name elided at all, newline-separated. The dock
    // hangs this off the block as a tooltip, so eliding the visible line never
    // costs the operator a name: the guarantee src/talkback-plan.h's reporting
    // chain exists for is that a shortfall is NAMED, and an elision that had
    // nowhere to put the rest of the names would quietly break it.
    std::string tooltip;
    // True when something the operator asked for did not happen: a shortfall,
    // or a failed attempt. The dock colours the block on this.
    bool warn = false;
};

// Past this many names a line stops being scannable at a glance in a dock the
// width of an OBS side panel, and an operator mid-show skips a paragraph. The
// elided form is what they read; the full list is one hover away.
constexpr std::size_t kTalkbackDockNameListMax = 5;

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

// The visible form. Never silently truncated: the count of what was dropped is
// part of the text, so a reader always knows there is more to see.
inline std::string join_names_elided(const std::vector<std::string> &names)
{
    if (names.size() <= kTalkbackDockNameListMax)
        return join_names(names);
    const std::vector<std::string> head(
        names.begin(),
        names.begin() + static_cast<std::ptrdiff_t>(kTalkbackDockNameListMax));
    return join_names(head) + " and " +
           std::to_string(names.size() - kTalkbackDockNameListMax) + " more";
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

    // Every detail line goes through here so the visible and the full form
    // cannot drift: one call site, two renderings of the same names.
    const auto push = [&report](const std::string &prefix,
                                const std::vector<std::string> &names,
                                const std::string &suffix) {
        report.lines.push_back(prefix + join_names_elided(names) + suffix);
        if (!report.tooltip.empty()) report.tooltip += "\n";
        report.tooltip += prefix + join_names(names) + suffix;
    };

    if (!confirmed.done) {
        // Not "nothing happened": a failed-after-destroy or superseded
        // outcome also lands here, with done reset to false and the reason
        // kept (src/talkback-nomination.h). Saying "no channels" alone would
        // hide the WHY on exactly the paths that need it most.
        report.headline = "No channels assigned yet. Tick talent and press "
                          "Assign channels.";
        report.tooltip = report.headline;
    } else {
        const std::vector<std::string> covered =
            talkback_private_channel_names(confirmed.requested,
                                           confirmed.uncovered_private);
        report.headline =
            plural(confirmed.channels, "channel", "channels") + " in use of " +
            std::to_string(kTalkbackMaxChannels) + " for " +
            plural(confirmed.requested.size(), "person", "people") + ".";
        report.tooltip = report.headline;
        if (covered.empty())
            push("Private channel: ", {}, "nobody.");
        else
            push("Private channel: ", covered, ".");
    }

    if (!confirmed.uncovered_private.empty()) {
        report.warn = true;
        push("No private channel, reach them via All talent: ",
             confirmed.uncovered_private, ".");
    }
    if (!confirmed.unreachable.empty()) {
        report.warn = true;
        // Strictly worse than losing the private aside, and always a subset of
        // uncovered_private -- so it gets its own line rather than being
        // buried in the one above. See TalkbackPlan::unreachable.
        push("On no channel at all, they hear nothing: ",
             confirmed.unreachable, ".");
    }
    if (confirmed.done && !confirmed.all_talent_complete) {
        report.warn = true;
        push("All talent does not reach everyone: this list is larger than "
             "16 channels can fan out to.", {}, "");
    }
    if (!confirmed.last_attempt_ok) {
        report.warn = true;
        // The last ATTEMPT, which can disagree with the confirmed plan above
        // without contradicting it (a refused re-nomination leaves the
        // standing channels exactly as they were). Said as a separate line
        // for that reason.
        push("Channel setup failed: ", {},
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
    // ONE SHORT LINE, for the row beside the source combo. The dock's first
    // live render put `text` on the panel and the owner's verdict was that a
    // wall of prose next to a control is not a status -- an operator scanning
    // mid-show needs to know only "is this safe or not", and the paragraph
    // that says WHY belongs where they can go and read it deliberately.
    std::string short_text;
    // The full explanation, including what to do about it. The dock renders
    // this as the tooltip of the short line, never as body copy.
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
        w.short_text = "No source chosen";
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
        w.short_text = "Off program (safe)";
        w.text = source_name + " is on no OBS track, so it feeds talkback "
                 "only. That is the safe pattern: a dedicated source on an "
                 "unused track.";
        return w;
    }
    w.on_air_risk = true;
    const bool one = w.tracks.find(',') == std::string::npos;
    w.short_text = std::string("On air via ") + (one ? "track " : "tracks ") +
                   w.tracks + ". The audience will hear this.";
    w.text = source_name + " is on OBS " + (one ? "track " : "tracks ") +
             w.tracks +
             ". If any of those are on air, the audience hears this talkback "
             "aside at full level. Uncheck them in Advanced Audio Properties, "
             "or use a dedicated source on an unused track.";
    return w;
}

// ── The ON AIR banner ───────────────────────────────────────────────────────
//
// This replaced the one-line tally after the dock's first live render: the
// owner's verdict was that the single most important fact on the panel -- am I
// audible to talent right now -- was a line of small red text underneath the
// buttons. It says the same four things the tally said, with the same rule
// about which of them counts as "live"; what changed is that it is a full-width
// strip at the top of the dock, sized and coloured to be read from across the
// room rather than leaned into.
//
// The claims are unchanged and each is load-bearing:
//   * LIVE requires the ENGINE's confirmation, never the plugin's intent (the
//     spec's own requirement, and the Critical -- C2 -- that made it one: a
//     ladder abort used to destroy the channels a live key was talking on with
//     the plugin still reporting live).
//   * A refusal is shown WITH the engine's own recovery hint, echoed and never
//     inferred -- the plugin does not invent a remedy the engine did not name.
//   * A closed key keeps the last verdict visible, said as history. The reason
//     outlives the key that earned it (talkback_start() clears it at the next
//     press), so it must not read as a current state.

enum class TalkbackDockBannerState {
    // Nothing keyed and nothing to answer for.
    Off,
    // A key is open and the engine has not confirmed the channel yet.
    Waiting,
    // Open AND engine-confirmed. The only state that may be shown as on air.
    Live,
    // TALKBACK DELIVERY LAW 1 (2026-08-29): open, engine-confirmed, and the
    // engine could NOT open its own meeting audio -- so the channels are real,
    // the key is real, and NOBODY CAN HEAR IT. Muted, Zoom ACCEPTS every
    // SendAudioDataToChannel (success codes, members confirmed) and delivers
    // silence, which is why this cannot be folded into Live: "on air" and "on
    // air to nobody" are the two states this whole law exists to separate, and
    // one enum value for both is exactly the plain ON AIR that hid the ghost.
    //
    // A SEPARATE VALUE RATHER THAN A bool ON THE BANNER, because the panel
    // paints from this enum via a style property, and a bool would have to be
    // remembered at every paint site instead of being impossible to drop.
    // Its consequence at the cell level is deliberate: this dock's standing
    // rule is RED MEANS THE DIRECTOR IS AUDIBLE, and they are not, so a cell
    // keyed into a blocked mic does not go red -- see zoom-talkback-panel.cpp.
    LiveMicBlocked,
    // A key was refused, or the last one that closed had failed.
    Refused,
    // This build's engine has no talkback at all (macOS: engine-talkback.cpp
    // is only in ENGINE_SOURCES, which the main-macos.mm target does not use).
    // The dock is cross-platform and compiles anyway, so without this it is a
    // panel of controls that send commands nothing on the other end answers.
    //
    // It is checked FIRST and returns, which is what keeps the roadmap wording
    // out of the ON AIR strip STRUCTURALLY rather than by promise: on a build
    // with no talkback engine nothing can key, so nothing can be live, and
    // Unavailable and Live are unreachable together by construction.
    Unavailable,
};

struct TalkbackDockBanner {
    TalkbackDockBannerState state = TalkbackDockBannerState::Off;
    // Read from across the room. Short, and it names the target.
    std::string headline;
    // One short line under it; empty when there is nothing to add.
    std::string detail;
};

struct TalkbackDockSessionView {
    // False on a build whose engine has no talkback (macOS). A FIELD, not an
    // #ifdef in this header: this file is Qt/OBS-free and compiles on every
    // platform, so the macOS rendering is pinned by a Windows or Linux CI run
    // -- otherwise a macOS-only branch is tested by nothing this project runs.
    // The caller sets it (see zoom-talkback-panel.cpp).
    bool        platform_supported = true;
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
    // TALKBACK DELIVERY LAW 1 (2026-08-29): the engine reported this key live
    // over a bot whose meeting audio it could not open (a meeting that locks
    // mute, or a host who muted the bot). The key is real and the channels are
    // real; nothing is audible. Straight from
    // ZoomEngineClient::TalkbackSessionStatus::mic_blocked, which is false
    // when the engine never reported a mic state at all -- so an engine older
    // than Law 1 renders exactly as it always did.
    //
    // Only meaningful while engine_live is true. A key that is not live has a
    // reason of its own, and "you were refused AND the mic was shut" is two
    // answers to a question with one.
    bool mic_blocked = false;
    // From the engine's session_live line, as of the moment the key opened.
    // NOT refreshed while the key is held: the engine reports these once per
    // key press, so a talent who rejoins mid-press is not counted until the
    // next press. members_known is false when no session_live has arrived.
    bool     members_known = false;
    uint32_t members_present = 0;
    uint32_t members_total = 0;
};

// The label an operator reads for a key target. kTalkbackAllTalentTarget is a
// sentinel ("all"), not somebody's name, and it is the target most likely to be
// read under pressure -- so it is spelled out rather than shown raw.
inline std::string talkback_dock_target_label(const std::string &target)
{
    if (target.empty()) return "no target";
    if (target == kTalkbackAllTalentTarget) return "All talent";
    return target;
}

// The engine's own recovery hint, in the operator's vocabulary.
//
// This does NOT weaken "echoed, never inferred": a hint the engine did not
// send is still nothing, and a hint this function does not recognise is passed
// through verbatim rather than dropped or reworded. All it does is spell the
// engine's `"recover":"re-nominate"` token -- which names an internal command,
// `talkback_nominate` -- as the action the dock's own button offers. The wire
// token is deliberately unchanged; Companion and the control API depend on it.
inline std::string talkback_dock_recovery_label(const std::string &recover)
{
    if (recover == "re-nominate") return "re-assign channels";
    return recover;
}

inline TalkbackDockBanner talkback_dock_banner(const TalkbackDockSessionView &s)
{
    TalkbackDockBanner b;
    // FIRST, and it returns. Everything below describes a key on an engine
    // that can carry one; this build's cannot. See TalkbackDockBannerState::
    // Unavailable for why the precedence is the safety property and not a
    // style choice.
    if (!s.platform_supported) {
        b.state = TalkbackDockBannerState::Unavailable;
        b.headline = "Talkback is coming to macOS";
        b.detail = "Talkback is Windows-only in this release.";
        return b;
    }
    if (!s.key_open) {
        b.headline = "Off air";
        if (!s.engine_reason.empty() && !s.engine_live) {
            // The last key's verdict, said as history -- see the header
            // comment above for why it cannot read as a current state.
            b.state = TalkbackDockBannerState::Refused;
            b.detail = "Last key failed: " + s.engine_reason + ".";
            if (!s.engine_recover.empty())
                b.detail += " Recovery: " +
                            talkback_dock_recovery_label(s.engine_recover) + ".";
        }
        return b;
    }

    const std::string label = talkback_dock_target_label(s.target);
    if (s.engine_live) {
        // TALKBACK DELIVERY LAW 1 (2026-08-29): LIVE IS NOT ENOUGH.
        //
        // Talkback delivers only while the engine's own meeting audio is open.
        // Muted, every SendAudioDataToChannel is ACCEPTED -- success codes,
        // members confirmed, zero failures -- and every member hears silence.
        // The operator's own production on 2026-08-29 had the bot muted by the
        // host, so this is the live case, not a hypothetical.
        //
        // The headline says it FIRST, before the target, because that is the
        // word order an operator reads under pressure and the thing they have
        // to act on is the mute, not who they were keying. It deliberately
        // still contains "ON AIR": the key IS open and the director IS
        // talking, so hiding that would trade one wrong belief for another.
        // The member tally is dropped here on purpose -- "3 of 4 present" next
        // to "nobody can hear you" reads as reassurance and is the exact
        // instrument that made the ghost look healthy.
        if (s.mic_blocked) {
            b.state = TalkbackDockBannerState::LiveMicBlocked;
            b.headline = "ON AIR - BOT MUTED: " + label;
            b.detail = "Zoom will not let CoreVideo unmute, so nobody hears "
                       "this key. Ask the host to unmute CoreVideo.";
            return b;
        }
        b.state = TalkbackDockBannerState::Live;
        b.headline = "ON AIR: " + label;
        if (s.members_known) {
            b.headline += " (" + std::to_string(s.members_present) + " of " +
                          std::to_string(s.members_total) + " present)";
        }
        return b;
    }
    if (s.engine_reason.empty()) {
        b.state = TalkbackDockBannerState::Waiting;
        b.headline = "Keying " + label;
        b.detail = "Waiting for Zoom to confirm the channel.";
        return b;
    }
    b.state = TalkbackDockBannerState::Refused;
    b.headline = "Key refused: " + label;
    b.detail = s.engine_reason + ".";
    if (!s.engine_recover.empty())
        b.detail += " Recovery: " +
                    talkback_dock_recovery_label(s.engine_recover) + ".";
    return b;
}

// ── What a press and a release mean ─────────────────────────────────────────

enum class TalkbackDockPressAction {
    OpenPushToTalk,
    OpenLatch,
    // Close the latched key this dock is already holding on this target.
    CloseHeldKey,
};

// `latch_selected` is the Latch checkbox as it stands RIGHT NOW, and it decides
// only what a NEW key would be opened as. Whether this press CLOSES one is
// decided by `open.latched` -- the mode that key was opened with (M1). The two
// disagree exactly when the operator toggles the checkbox while a key is live,
// which is the interleaving that used to leave a latched key un-closeable.
inline TalkbackDockPressAction talkback_dock_press_action(
    const TalkbackDockOpenKey &open, const std::string &pressed_target,
    bool latch_selected)
{
    if (open.open && open.dock_owned && open.latched &&
        open.target == pressed_target)
        return TalkbackDockPressAction::CloseHeldKey;
    return latch_selected ? TalkbackDockPressAction::OpenLatch
                          : TalkbackDockPressAction::OpenPushToTalk;
}

// Does this button release close the open key? Only a push-to-talk key this
// dock owns, on the target being released. A latch is closed by the next PRESS
// (above), never by a release -- and a key belonging to another surface, or to
// another target, is not this release's to close: a stray release arriving
// after the dead-man switch already closed something must not close whatever
// was opened next.
inline bool talkback_dock_release_closes(const TalkbackDockOpenKey &open,
                                         const std::string &released_target)
{
    return open.open && open.dock_owned && !open.latched &&
           open.target == released_target;
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
// A release can still go missing in-process, and NOT only in one way:
// QAbstractButton clears its pressed state without emitting released() on an
// EnabledChange, on any non-popup focus loss (focusOutEvent), and anywhere a
// style or a caller reaches setDown(false). This backstop is deliberately
// CAUSE-AGNOSTIC -- it asks the widget whether it is still down, not why it
// stopped being down -- so it covers all of those and whatever Qt adds next.
// It is load-bearing, not a redundant second opinion: "the dock never disables
// a held button" closes exactly one of those causes and is not coverage.
//
// Reading the widget's state (rather than a renewal deadline) is also what
// makes it unable to false-close a genuinely held key: while the UI thread is
// stalled this does not run at all, and once it resumes isDown() is accurate
// again. A latch is exempt by definition -- nothing is being held.
inline bool talkback_dock_release_lost(const TalkbackDockOpenKey &open,
                                       bool button_down)
{
    return open.open && open.dock_owned && !open.latched && !button_down;
}
