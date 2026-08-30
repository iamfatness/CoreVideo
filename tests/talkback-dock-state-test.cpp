// tests/talkback-dock-state-test.cpp
//
// Milestone 7: the decisions the Talkback dock group makes, pinned away from
// the QWidget that renders them.
//
// Every claim in here is one an operator acts on mid-show -- "this button is
// safe to press", "this person hears nothing", "this source is on air" -- and
// none of them could be exercised at all while they lived inside dock code
// that needs libobs, a Qt event loop and a running engine to construct. The
// two Majors this milestone has already shipped (F1, N1) both lived in
// exactly that kind of untestable wiring; see
// src/talkback-nomination-dispatch.h's header comment.
//
// The load-bearing one is the FIRST cluster: a button the dock shows as
// enabled must be one TalkbackController::key_on() would not refuse on the
// nomination check. The dock delegates to talkback_target_known_unprovisioned()
// for that instead of re-deriving the rule, and this drives both against the
// same plans so a re-derivation would show up as a disagreement here.
#include "talkback-dock-state.h"
// LAW 1 (2026-08-29): talkback_session_mic_blocked() -- the wire rule the
// banner state is driven from, so the chain is pinned end to end.
#include "talkback-key.h"

#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

static const TalkbackDockKeyButton *find_button(
    const std::vector<TalkbackDockKeyButton> &buttons, const std::string &target)
{
    for (const auto &b : buttons)
        if (b.target == target) return &b;
    return nullptr;
}

// A plan the engine confirmed: Sarah and Luis nominated, both privately
// covered.
static TalkbackNominationPlan confirmed_plan()
{
    TalkbackNominationPlan p;
    p.done = true;
    p.channels = 3;
    p.requested = {"Sarah", "Luis"};
    return p;
}

static TalkbackDockKeyContext ready_context()
{
    TalkbackDockKeyContext ctx;
    ctx.engine_running = true;
    ctx.in_meeting = true;
    ctx.source_chosen = true;
    return ctx;
}

// A key this dock opened on `target`, in the given mode.
static TalkbackDockOpenKey dock_key(const std::string &target, bool latched)
{
    TalkbackDockOpenKey open;
    open.open = true;
    open.dock_owned = true;
    open.target = target;
    open.latched = latched;
    return open;
}

// The names on a list, in the order it would render them.
static std::vector<std::string> row_names(
    const std::vector<TalkbackNomineeRow> &rows)
{
    std::vector<std::string> names;
    for (const auto &r : rows) names.push_back(r.name);
    return names;
}

static const TalkbackNomineeRow *find_row(
    const std::vector<TalkbackNomineeRow> &rows, const std::string &name)
{
    for (const auto &r : rows)
        if (r.name == name) return &r;
    return nullptr;
}

int main()
{
    // ── The talent list ───────────────────────────────────────────────────
    //
    // THE LIVE DEFECT (2026-08-29, Zoom Events production with breakout
    // rooms): "moving from room to room the nomination list doesn't update".
    // The roster cache the dock reads was verified fresh at the time and the
    // rows it derived were right; what was wrong was the rebuild GATE. The
    // signature was built by walking the roster in whatever order the Zoom SDK
    // returned it, so a REORDERED roster -- same people, same presence --
    // looked like a changed one and rebuilt the whole QListWidget, on a 100 ms
    // tick, in a room where two of Zoom's five roster callbacks fire on every
    // mute and camera toggle by anyone. A rebuild resets the scroll position
    // and destroys the item a click is in the middle of, so what the operator
    // saw was a list that would not take their ticks.
    {
        // A room move: A and B, tick A, then the roster becomes C and D.
        TalkbackNomineeListState list;
        check(talkback_nominee_list_refresh(list, {"A", "B"}, {}),
              "the first roster did not build the list");
        check(row_names(list.rows) == std::vector<std::string>({"A", "B"}),
              "the first roster did not render both people");

        // The operator ticks A; the widget is the only place that lives, so
        // the next tick reads it back in.
        check(!talkback_nominee_list_refresh(list, {"A", "B"}, {"A"}),
              "ticking a box rebuilt the list under the operator");
        check(find_row(list.rows, "A") && find_row(list.rows, "A")->checked,
              "the tick was not carried");

        // The room move.
        check(talkback_nominee_list_refresh(list, {"C", "D"}, {"A"}),
              "a room move did not rebuild the list");
        const auto *a = find_row(list.rows, "A");
        const auto *c = find_row(list.rows, "C");
        const auto *d = find_row(list.rows, "D");
        check(a && !a->present && a->checked,
              "the ticked name from the previous room was dropped, or was "
              "shown as present");
        check(c && c->present && !c->checked,
              "the new room's people did not arrive on the list");
        check(d && d->present, "the new room's people did not arrive in full");
        check(list.rows.size() == 3, "the list carried a stale row");
        // Present first, then the ticked strays, each in name order -- so the
        // rows an operator is ticking do not move under the cursor.
        check(row_names(list.rows) == std::vector<std::string>({"C", "D", "A"}),
              "the rows are not in the deterministic present-then-absent "
              "order");
    }
    {
        // The same move, but the roster goes transiently EMPTY in between --
        // which is what a rejoin looks like from here.
        TalkbackNomineeListState list;
        check(talkback_nominee_list_refresh(list, {"A", "B"}, {}),
              "the first roster did not build the list");
        check(talkback_nominee_list_refresh(list, {}, {"A"}),
              "an emptied roster did not rebuild the list");
        check(row_names(list.rows) == std::vector<std::string>({"A"}),
              "an emptied roster did not prune to the ticked names");
        check(!list.rows.front().present,
              "a name with nobody in the meeting was still shown as present");
        check(talkback_nominee_list_refresh(list, {"C", "D"}, {"A"}),
              "the roster coming back did not rebuild the list");
        check(row_names(list.rows) == std::vector<std::string>({"C", "D", "A"}),
              "the roster coming back after an empty tick did not repopulate");
    }
    {
        // THE REGRESSION ITSELF. The same people in a different order is not a
        // change, and must not cost the operator the widget they are clicking.
        TalkbackNomineeListState list;
        check(talkback_nominee_list_refresh(list, {"Ann", "Bo", "Cy"}, {}),
              "the first roster did not build the list");
        check(!talkback_nominee_list_refresh(list, {"Cy", "Ann", "Bo"}, {}),
              "a reordered roster rebuilt the list -- this is the live defect: "
              "at 100 ms a rebuild eats the click that is in flight");
        check(!talkback_nominee_list_refresh(list, {"Bo", "Cy", "Ann"}, {"Bo"}),
              "a reordered roster rebuilt the list once something was ticked");
        check(row_names(list.rows) ==
                  std::vector<std::string>({"Ann", "Bo", "Cy"}),
              "the rendered order followed the roster's order");
        check(talkback_nominee_signature(list.rows) ==
                  talkback_nominee_signature(
                      talkback_nominee_rows({"Cy", "Bo", "Ann"}, {})),
              "the signature is order-sensitive");
        // A real change still rebuilds: presence and membership are the only
        // things that may.
        check(talkback_nominee_list_refresh(list, {"Ann", "Bo"}, {"Bo"}),
              "somebody leaving did not rebuild the list");
        check(talkback_nominee_list_refresh(list, {"Ann", "Bo", "Cy"}, {"Bo"}),
              "somebody arriving did not rebuild the list");
    }
    {
        // Two people with the same display name are one row, and a duplicate
        // in the roster is not a change either.
        TalkbackNomineeListState list;
        check(talkback_nominee_list_refresh(list, {"Ann", "Ann", "Bo"}, {}),
              "the first roster did not build the list");
        check(row_names(list.rows) == std::vector<std::string>({"Ann", "Bo"}),
              "a duplicated display name produced two rows");
        check(!talkback_nominee_list_refresh(list, {"Bo", "Ann"}, {}),
              "dropping a duplicate rebuilt the list");
        // A ticked name that comes BACK flips to present and keeps its tick.
        check(talkback_nominee_list_refresh(list, {"Bo"}, {"Ann"}),
              "a ticked name leaving did not rebuild the list");
        check(talkback_nominee_list_refresh(list, {"Ann", "Bo"}, {"Ann"}),
              "a ticked name returning did not rebuild the list");
        const auto *ann = find_row(list.rows, "Ann");
        check(ann && ann->present && ann->checked,
              "a ticked name who came back was not shown as present and "
              "ticked");
    }

    // ── How much room the list and the key grid need ──────────────────────
    //
    // Both are arithmetic the owner's first look at the standalone dock caught
    // being wrong on screen (2026-08-29).
    {
        // Five people must show five WHOLE rows. The old code guessed a row
        // height from the font, missed the stylesheet's item padding and the
        // frame, and then got squeezed to its 3-row minimum by a dock shorter
        // than the panel -- about two and a half rows, with a scrollbar.
        check(talkback_dock_nominee_visible_rows(5) == 5,
              "five people did not get five rows");
        // An almost-empty list still has to read as a list, and an unbounded
        // one must not push the key buttons off the bottom.
        check(talkback_dock_nominee_visible_rows(0) == kTalkbackNomineeMinRows,
              "an empty list collapsed below the minimum");
        check(talkback_dock_nominee_visible_rows(1) == kTalkbackNomineeMinRows,
              "a one-row list collapsed to a sliver");
        check(talkback_dock_nominee_visible_rows(kTalkbackNomineeMaxRows) ==
                  kTalkbackNomineeMaxRows,
              "exactly the maximum did not get every row");
        check(talkback_dock_nominee_visible_rows(40) == kTalkbackNomineeMaxRows,
              "a long roster grew the list past the maximum instead of "
              "scrolling");
        check(kTalkbackNomineeMinRows <= kTalkbackNomineeMaxRows,
              "the row bounds are inverted");
    }
    {
        // TWO OR THREE, NEVER ONE. The grid this replaces sized every button
        // to the WIDEST label in the room, so one 28-character display name
        // ("Ronny Hofsoy, Tromso, Norway", 2026-08-29) dropped seven people
        // into a single full-width column -- a 400 px tower of buttons, and
        // nothing survivable at twenty-four. A cell is sized to a minimum
        // readable width instead and a name that does not fit is elided.
        //
        // The floor of two is the load-bearing claim: this grid is a status
        // display as much as a control surface, and one column of 24 cells is
        // not scannable whatever the dock width.
        check(talkback_dock_cell_columns(400, 118, 8) == 3,
              "a wide dock did not get three columns");
        check(talkback_dock_cell_columns(370, 118, 8) == 3,
              "the exact three-column fit was rejected");
        check(talkback_dock_cell_columns(369, 118, 8) == 2,
              "three columns were used one pixel too narrow");
        check(talkback_dock_cell_columns(276, 118, 8) == 2,
              "the 320 px dock minimum did not get two columns");
        // The gap counts: it is what makes the fit exact.
        check(talkback_dock_cell_columns(354, 118, 0) == 3,
              "the gaps were charged when there were none");
        check(talkback_dock_cell_columns(354, 118, 8) == 2,
              "the gaps were not charged against the fit");
        // Never one, never zero, whatever it is asked -- including before the
        // layout has run and the width is not real yet.
        check(talkback_dock_cell_columns(0, 118, 8) == 2,
              "an unrealised width collapsed the grid to a column");
        check(talkback_dock_cell_columns(120, 118, 8) == 2,
              "an absurdly narrow dock collapsed the grid to a column");
        check(talkback_dock_cell_columns(400, 0, 8) == 2,
              "an unmeasured cell width collapsed the grid to a column");
        check(kTalkbackDockCellMinPx > 0,
              "the minimum cell width is not a width");
    }

    // ── The minimum cell width is MEASURED, and 118 is only its floor ────
    //
    // The live defect this closes (operator's high-DPI screen, 2026-08-29):
    // 118 px was derived from an offscreen render at 1:1 font metrics and
    // then spent as a fixed budget. On a display that resolves the cell font
    // larger it stops meaning "the narrowest cell a name is still readable
    // in" -- it fits four characters -- and the grid confidently offers three
    // columns of unreadable stumps. The caller measures a gauge string in the
    // LIVE label's own metrics and adds the cell's own measured chrome; this
    // function only refuses to go below the floor.
    {
        // A font whose gauge and chrome fit inside the floor leaves the floor
        // in force, so nothing about the 1:1 layout moves.
        check(talkback_dock_cell_min_px(78, 28, kTalkbackDockCellMinPx) ==
                  kTalkbackDockCellMinPx,
              "a small font did not keep the 118 px floor");
        // Twice the font is nearly twice the minimum -- the whole point.
        check(talkback_dock_cell_min_px(156, 56, kTalkbackDockCellMinPx) == 212,
              "a large font did not widen the minimum cell");
        // ...and a wider minimum is what stops three columns being offered in
        // a dock that cannot read them.
        check(talkback_dock_cell_columns(400, talkback_dock_cell_min_px(
                  156, 56, kTalkbackDockCellMinPx), 8) == 2,
              "a large font still got three columns in a 400 px dock");
        // Nothing measured yet (before the first layout) must not produce a
        // minimum smaller than the floor, which would offer three columns on
        // the strength of a measurement that has not happened.
        check(talkback_dock_cell_min_px(0, 0, kTalkbackDockCellMinPx) ==
                  kTalkbackDockCellMinPx,
              "an unmeasured font dropped the minimum below the floor");
        check(talkback_dock_cell_min_px(-40, -10, kTalkbackDockCellMinPx) ==
                  kTalkbackDockCellMinPx,
              "a nonsense measurement dropped the minimum below the floor");
        check(kTalkbackDockCellGauge != nullptr &&
                  std::string(kTalkbackDockCellGauge).size() >= 8,
              "the gauge is too short to stand for a display name");
    }

    // ── Edit mode never costs the operator a key ─────────────────────
    //
    // The grid and the talent checklist share one slot, which is the whole
    // point (the same people used to appear twice on one panel). Hiding a
    // button the operator is HOLDING strands the key: a latch loses its only
    // close affordance and a push-to-talk loses its release, leaving the
    // director live to talent with nothing on screen to stop it.
    {
        TalkbackDockOpenKey none;
        check(talkback_dock_edit_mode(true, none),
              "the editor would not open with nothing keyed");
        check(!talkback_dock_edit_mode(false, none),
              "the editor opened without being asked for");
        check(!talkback_dock_edit_mode(true, dock_key("Sarah", true)),
              "the editor hid the grid while this dock held a latched key");
        check(!talkback_dock_edit_mode(true, dock_key("Sarah", false)),
              "the editor hid the grid while this dock held a key");
        TalkbackDockOpenKey other = dock_key("Sarah", false);
        other.dock_owned = false;
        check(!talkback_dock_edit_mode(true, other),
              "the editor stayed open while another surface held a key -- the "
              "grid is what the operator needs to be looking at");
    }

    // ── Buttons agree with key_on()'s own refusal rule ─────────────────────
    {
        const auto plan = confirmed_plan();
        const auto buttons = talkback_dock_key_buttons(plan, ready_context());
        check(buttons.size() == 3,
              "expected one All button plus one per nominee");
        check(buttons.front().target == kTalkbackAllTalentTarget &&
                  buttons.front().all_talent,
              "the all-talent button is not first, or is not flagged as such");
        for (const auto &b : buttons) {
            const bool known_unprovisioned = talkback_target_known_unprovisioned(
                b.target, plan.requested, plan.uncovered_private);
            check(b.enabled == !known_unprovisioned,
                  "a button's enabled state disagrees with the predicate "
                  "key_on() refuses on");
            check(b.enabled == b.reason.empty(),
                  "a button carries both an enabled state and a refusal reason");
        }
    }

    // A nominee with no private channel: the dock must not offer their button,
    // and must say the thing that actually works instead (key All).
    {
        auto plan = confirmed_plan();
        plan.uncovered_private = {"Luis"};
        const auto buttons = talkback_dock_key_buttons(plan, ready_context());
        const auto *luis = find_button(buttons, "Luis");
        const auto *all  = find_button(buttons, kTalkbackAllTalentTarget);
        check(luis && !luis->enabled,
              "a nominee with no private channel was offered a key button");
        check(luis && contains(luis->reason, "All"),
              "the uncovered-nominee reason does not name the way that works");
        check(all && all->enabled,
              "all-talent was refused even though someone was nominated");
    }

    // Unreachable is strictly worse than uncovered and must not be described
    // as "key All instead" -- All does not reach them either.
    {
        auto plan = confirmed_plan();
        plan.uncovered_private = {"Luis"};
        plan.unreachable = {"Luis"};
        plan.all_talent_complete = false;
        const auto buttons = talkback_dock_key_buttons(plan, ready_context());
        const auto *luis = find_button(buttons, "Luis");
        check(luis && !luis->enabled, "an unreachable nominee was keyable");
        check(luis && contains(luis->reason, "no channel"),
              "the unreachable reason does not say they are on no channel");
        check(luis && !contains(luis->reason, "key All"),
              "an unreachable nominee was told to key All, which does not "
              "reach them either");
    }

    // Nothing confirmed yet: every target refused, including all-talent --
    // the same fail-closed state talkback_nomination_reset() leaves behind.
    {
        TalkbackNominationPlan empty;
        const auto buttons = talkback_dock_key_buttons(empty, ready_context());
        check(buttons.size() == 1, "an unnominated plan produced nominee buttons");
        check(!buttons.front().enabled,
              "all-talent was keyable with nothing nominated");
        check(contains(buttons.front().reason, "channel"),
              "the no-nomination reason does not say that nobody has a "
              "channel");
        check(!contains(buttons.front().reason, "nominat"),
              "operator-facing copy still says nominate");
    }

    // Engine/meeting preconditions, and the one-key-at-a-time rule.
    {
        const auto plan = confirmed_plan();
        auto ctx = ready_context();
        ctx.engine_running = false;
        for (const auto &b : talkback_dock_key_buttons(plan, ctx))
            check(!b.enabled, "a key button was live with the engine stopped");

        ctx = ready_context();
        ctx.in_meeting = false;
        for (const auto &b : talkback_dock_key_buttons(plan, ctx))
            check(!b.enabled, "a key button was live outside a meeting");

        ctx = ready_context();
        ctx.open = dock_key("Sarah", /*latched=*/false);
        const auto buttons = talkback_dock_key_buttons(plan, ctx);
        const auto *sarah = find_button(buttons, "Sarah");
        const auto *luis  = find_button(buttons, "Luis");
        // The held button must stay enabled: it is the operator's only way to
        // release a latch, and disabling a pressed QPushButton is itself how a
        // released() signal gets lost. See talkback_dock_release_lost().
        check(sarah && sarah->enabled,
              "the button for the key this dock is holding was disabled");
        check(luis && !luis->enabled,
              "a second key button was live while a key was already open");
    }

    // m3: a key held by ANOTHER surface. key_on() refuses a second key
    // unconditionally and the dock's toggle-off cannot apply to a key it does
    // not own, so every button must refuse -- including the open target's,
    // which the dock-owned case deliberately leaves live.
    {
        const auto plan = confirmed_plan();
        auto ctx = ready_context();
        ctx.open = dock_key("Sarah", /*latched=*/false);
        ctx.open.dock_owned = false;
        const auto buttons = talkback_dock_key_buttons(plan, ctx);
        for (const auto &b : buttons) {
            check(!b.enabled,
                  "a key button was live while another surface held the key");
            check(contains(b.reason, "surface"),
                  "the reason does not say another surface holds the key");
        }
    }

    // m4: no talk source chosen. Every press would reach key_on() and be
    // refused for want of a tap, so say it on the button instead.
    {
        const auto plan = confirmed_plan();
        auto ctx = ready_context();
        ctx.source_chosen = false;
        for (const auto &b : talkback_dock_key_buttons(plan, ctx)) {
            check(!b.enabled, "a key button was live with no talk source");
            check(contains(b.reason, "source"),
                  "the reason does not name the missing source");
        }
    }

    // ── The intercom grid: what a cell says about a person ───────────
    //
    // THE REDESIGN'S OWN CLAIM. The old panel could only say "this person has
    // a channel". On 2026-08-29 that was true of John Wallace and he could
    // hear NOTHING: every invite for him was refused SDKERR_WRONG_USAGE (2)
    // because he was in a different breakout room from the engine, and Zoom's
    // talkback reaches only the room the inviter is in. Grant Whitehead's
    // client reported no talkback support at all (supported:false, then
    // SDKERR_INVALID_PARAMETER (3)). Both rendered as a ready, pressable key
    // on a person who hears silence.
    //
    // Enablement is NOT re-derived here -- a cell carries
    // talkback_dock_key_buttons()' own answer verbatim, so the state below can
    // never make a cell live that key_on() would refuse. These pin the STATE.
    {
        auto plan = confirmed_plan();
        plan.requested = {"Sarah", "Luis", "Dana", "Mo"};
        plan.uncovered_private = {"Mo"};
        TalkbackChannelPresence presence;
        talkback_presence_note(presence, "Sarah", TalkbackPersonPresence::Present);
        talkback_presence_note(presence, "Luis", TalkbackPersonPresence::NotInChannel);
        talkback_presence_note(presence, "Dana", TalkbackPersonPresence::NoTalkback);
        // Mo: nothing observed at all.

        const auto cells = talkback_dock_cells(plan, ready_context(), presence, "");
        check(cells.size() == 5,
              "expected one All cell plus one per nominee");
        check(cells.front().all_talent &&
                  cells.front().target == kTalkbackAllTalentTarget,
              "the all-talent cell is not first, or is not flagged as such");

        const auto find = [&cells](const std::string &t) -> const TalkbackDockCell * {
            for (const auto &c : cells) if (c.target == t) return &c;
            return nullptr;
        };
        const auto *sarah = find("Sarah");
        const auto *luis  = find("Luis");
        const auto *dana  = find("Dana");
        const auto *mo    = find("Mo");

        check(sarah && sarah->state == TalkbackDockCellState::Ready &&
                  sarah->state_line == "ready",
              "a covered, present nominee was not shown as ready");
        check(luis && luis->state == TalkbackDockCellState::NotInChannel,
              "somebody with a channel who is not in it was shown as ready -- "
              "this is the 2026-08-29 breakout-room case");
        check(luis && contains(luis->state_line, "not in channel"),
              "the not-in-channel state line does not say so");
        check(luis && contains(luis->hint, "breakout"),
              "the not-in-channel hint does not name the likely cause");
        check(dana && dana->state == TalkbackDockCellState::Unreachable &&
                  contains(dana->state_line, "no talkback"),
              "a client with no talkback support was not shown as unreachable");
        check(mo && mo->state == TalkbackDockCellState::NoChannel &&
                  contains(mo->state_line, "no channel"),
              "a nominee with no private channel was not shown as such");
        // Enablement is the key buttons' answer, verbatim -- never re-derived
        // from the state above.
        const auto buttons = talkback_dock_key_buttons(plan, ready_context());
        check(buttons.size() == cells.size(),
              "the grid and the key buttons disagree about how many targets "
              "there are");
        for (std::size_t i = 0; i < cells.size(); ++i)
            check(cells[i].enabled == buttons[i].enabled &&
                      cells[i].reason == buttons[i].reason &&
                      cells[i].target == buttons[i].target,
                  "a cell re-derived its own enablement instead of carrying "
                  "talkback_dock_key_buttons()' answer");
    }
    {
        // ON AIR BEATS EVERYTHING. A live key is the one fact on this panel an
        // operator must never have to reason about; every other state is about
        // whether a key WOULD work. `live_target` is the caller's answer (the
        // banner says Live AND this dock owns it), so the engine-confirmation
        // rule is decided once and not a second time here.
        auto plan = confirmed_plan();
        plan.unreachable = {"Luis"};
        plan.uncovered_private = {"Luis"};
        TalkbackChannelPresence presence;
        talkback_presence_note(presence, "Luis", TalkbackPersonPresence::NotInChannel);
        auto ctx = ready_context();
        ctx.open = dock_key("Luis", /*latched=*/true);
        const auto cells = talkback_dock_cells(plan, ctx, presence, "Luis");
        for (const auto &c : cells) {
            if (c.target != "Luis") continue;
            check(c.state == TalkbackDockCellState::OnAir,
                  "a cell holding a live key did not say ON AIR");
            check(c.state_line == "ON AIR",
                  "the live cell's state line does not say ON AIR");
        }
        // ...and nothing else claims it.
        for (const auto &c : cells)
            check(c.target == "Luis" || c.state != TalkbackDockCellState::OnAir,
                  "a cell that is not holding the key claimed to be ON AIR");
        // No live target reported -> no cell is ON AIR, whatever the dock's
        // own record says. This is the C2 rule at the cell.
        for (const auto &c : talkback_dock_cells(plan, ctx, presence, ""))
            check(c.state != TalkbackDockCellState::OnAir,
                  "a cell painted itself ON AIR without the engine's "
                  "confirmation");
    }
    {
        // UNREACHABLE BEATS NO-CHANNEL. TalkbackPlan::unreachable is always a
        // subset of uncovered_private, so the generic "no channel, assign
        // again" would otherwise win and send the operator to re-assign
        // channels for somebody no assignment can reach.
        auto plan = confirmed_plan();
        plan.requested = {"Sarah", "Luis"};
        plan.uncovered_private = {"Luis"};
        plan.unreachable = {"Luis"};
        plan.all_talent_complete = false;
        const auto cells =
            talkback_dock_cells(plan, ready_context(), TalkbackChannelPresence{}, "");
        for (const auto &c : cells) {
            if (c.target != "Luis") continue;
            check(c.state == TalkbackDockCellState::Unreachable,
                  "an unreachable nominee was reported as merely having no "
                  "channel");
            check(!contains(c.state_line, "no channel"),
                  "an unreachable nominee was told to look for a channel");
        }
        // The same precedence from the OTHER source of unreachability: the
        // person's client, not the budget.
        auto covered = confirmed_plan();
        TalkbackChannelPresence presence;
        talkback_presence_note(presence, "Luis", TalkbackPersonPresence::NoTalkback);
        for (const auto &c :
             talkback_dock_cells(covered, ready_context(), presence, "")) {
            if (c.target != "Luis") continue;
            check(c.state == TalkbackDockCellState::Unreachable,
                  "a client with no talkback support was shown as ready "
                  "because it had a channel");
        }
    }
    {
        // NO-CHANNEL BEATS NOT-IN-CHANNEL: there is no channel to be in. And
        // a plan that is not confirmed yet says "assigning...", not "no
        // channel" -- a ladder that is still running is not a failure to
        // re-assign.
        auto plan = confirmed_plan();
        plan.requested = {"Sarah", "Luis"};
        plan.uncovered_private = {"Luis"};
        TalkbackChannelPresence presence;
        talkback_presence_note(presence, "Luis", TalkbackPersonPresence::NotInChannel);
        for (const auto &c :
             talkback_dock_cells(plan, ready_context(), presence, "")) {
            if (c.target != "Luis") continue;
            check(c.state == TalkbackDockCellState::NoChannel,
                  "somebody with no channel at all was described by their "
                  "membership of it");
        }
        TalkbackNominationPlan pending;
        check(talkback_dock_cell_state_line(TalkbackDockCellState::NoChannel,
                                            pending) == "assigning...",
              "a ladder that has not reported yet was called a missing "
              "channel");
        auto done = confirmed_plan();
        check(talkback_dock_cell_state_line(TalkbackDockCellState::NoChannel,
                                            done) == "no channel",
              "a confirmed plan's shortfall was still called in progress");

        // REVIEW ROUND 1, m7: ALL TALENT HAS NO CLIENT.
        //
        // talkback_dock_cell_state() legitimately returns Unreachable for the
        // ALL-TALENT cell -- when a completed plan could not finish the
        // fan-out (`done && !all_talent_complete`, the 16-channel cap) -- and
        // the person-shaped copy for that state says "no talkback" with a hint
        // blaming "their Zoom client". There is no client. The state is right;
        // the words were a claim about somebody who does not exist, and they
        // send the operator to debug a Zoom install instead of re-assigning
        // channels for fewer people.
        check(talkback_dock_cell_state_line(TalkbackDockCellState::Unreachable,
                                            done, /*all_talent=*/true) !=
                  "no talkback",
              "the ALL-TALENT cell described a short fan-out as a person's "
              "client lacking talkback support");
        check(talkback_dock_cell_state_line(TalkbackDockCellState::Unreachable,
                                            done, /*all_talent=*/false) ==
                  "no talkback",
              "a PERSON who cannot be reached lost their own copy -- the "
              "all-talent wording must not leak onto the people it was carved "
              "out from");
        const std::string all_hint =
            talkback_dock_cell_hint(TalkbackDockCellState::Unreachable, true);
        check(!contains(all_hint, "their Zoom client"),
              "the ALL-TALENT hint still blames a Zoom client that does not "
              "exist for this cell");
        check(contains(all_hint, "budget") && contains(all_hint, "Assign"),
              "the ALL-TALENT hint does not name the real cause (the channel "
              "budget) or the action that fixes it");
        check(contains(talkback_dock_cell_hint(TalkbackDockCellState::Unreachable,
                                               false),
                       "their Zoom client"),
              "a PERSON's unreachable hint lost the diagnosis that names the "
              "actual cause");
    }
    {
        // UNKNOWN IS READY, not absent. An engine that reports none of the
        // membership stages, or a plugin that has simply not seen them yet,
        // must not paint every person amber -- absence of evidence is not
        // evidence of absence, and this record never gates a key anyway.
        const auto cells = talkback_dock_cells(
            confirmed_plan(), ready_context(), TalkbackChannelPresence{}, "");
        for (const auto &c : cells)
            check(c.state == TalkbackDockCellState::Ready,
                  "a person nobody has reported on was painted as a problem");
    }
    {
        // All talent is a fan-out, not a person: no client to lack support, no
        // membership of its own. Its only failures are the plan's.
        TalkbackChannelPresence presence;
        talkback_presence_note(presence, kTalkbackAllTalentTarget,
                               TalkbackPersonPresence::NotInChannel);
        const auto ready = talkback_dock_cells(confirmed_plan(),
                                               ready_context(), presence, "");
        check(ready.front().state == TalkbackDockCellState::Ready,
              "the all-talent cell took a per-person absence personally");

        TalkbackNominationPlan nothing;
        const auto empty = talkback_dock_cells(nothing, ready_context(),
                                               TalkbackChannelPresence{}, "");
        check(empty.size() == 1 &&
                  empty.front().state == TalkbackDockCellState::NoChannel,
              "all-talent was shown as ready with nothing assigned");

        auto short_fanout = confirmed_plan();
        short_fanout.all_talent_complete = false;
        check(talkback_dock_cells(short_fanout, ready_context(),
                                  TalkbackChannelPresence{}, "")
                      .front()
                      .state == TalkbackDockCellState::Unreachable,
              "an all-talent fan-out the channel cap could not complete was "
              "still offered as reaching everyone");
    }
    {
        // The global refusals (engine stopped, out of meeting, no talk source)
        // are NOT per-person facts. They disable every cell with their own
        // reason -- which is the key buttons' job, unchanged -- while the
        // state line still describes the PERSON. "Ready, and you cannot press
        // it right now" is two true facts, and the banner says which.
        auto ctx = ready_context();
        ctx.engine_running = false;
        for (const auto &c : talkback_dock_cells(confirmed_plan(), ctx,
                                                 TalkbackChannelPresence{}, "")) {
            check(!c.enabled, "a cell was live with the engine stopped");
            check(c.state == TalkbackDockCellState::Ready,
                  "a stopped engine was reported as a person's own problem");
            check(contains(c.reason, "engine"),
                  "the cell lost the key button's refusal reason");
        }
    }

    // ── Per-person presence, the record the states are read from ───────
    {
        TalkbackChannelPresence presence;
        check(talkback_presence_for(presence, "Sarah") ==
                  TalkbackPersonPresence::Unknown,
              "an empty record invented a state for somebody");
        talkback_presence_note(presence, "Sarah", TalkbackPersonPresence::NotInChannel);
        check(talkback_presence_for(presence, "Sarah") ==
                  TalkbackPersonPresence::NotInChannel,
              "an observation was not recorded");
        talkback_presence_note(presence, "Sarah", TalkbackPersonPresence::Present);
        check(talkback_presence_for(presence, "Sarah") ==
                  TalkbackPersonPresence::Present,
              "a later confirmed join did not replace an earlier refusal -- "
              "on 2026-08-29 three people were refused SDKERR_TOO_FREQUENT_"
              "CALL on the all-talent invite and admitted to their own "
              "channel a second later");
        // NoTalkback survives a later NotInChannel, because the wire produces
        // BOTH for a client that cannot do talkback (supported:false, then the
        // invite refused) and the specific diagnosis is the useful one: "their
        // Zoom cannot do this" rather than "re-assign channels".
        talkback_presence_note(presence, "Dana", TalkbackPersonPresence::NoTalkback);
        talkback_presence_note(presence, "Dana", TalkbackPersonPresence::NotInChannel);
        check(talkback_presence_for(presence, "Dana") ==
                  TalkbackPersonPresence::NoTalkback,
              "a no-talkback client was downgraded to a generic absence by "
              "its own invite failure");
        talkback_presence_note(presence, "Dana", TalkbackPersonPresence::Present);
        check(talkback_presence_for(presence, "Dana") ==
                  TalkbackPersonPresence::Present,
              "a confirmed join could not clear a stale no-support report");
        // A nameless or Unknown observation is not a fact.
        talkback_presence_note(presence, "", TalkbackPersonPresence::Present);
        talkback_presence_note(presence, "Sarah", TalkbackPersonPresence::Unknown);
        check(talkback_presence_for(presence, "Sarah") ==
                  TalkbackPersonPresence::Present,
              "an Unknown observation erased a real one");
        talkback_presence_reset(presence);
        check(talkback_presence_for(presence, "Sarah") ==
                  TalkbackPersonPresence::Unknown &&
                  talkback_presence_for(presence, "Dana") ==
                      TalkbackPersonPresence::Unknown,
              "the world-reset left observations about destroyed channels "
              "behind");
    }

    // ── The nomination outcome ────────────────────────────────────────────
    {
        auto plan = confirmed_plan();
        plan.channels = 3;
        const auto report = talkback_dock_nomination_report(plan);
        check(!report.warn, "a fully covered nomination was reported as a problem");
        check(contains(report.headline, "3 channels"),
              "the headline does not report the channel count");
        check(contains(report.headline, "16"),
              "the headline does not report the channel budget");
        check(contains(report.headline, "2 people"),
              "the headline does not report how many people are covered");
        bool named_both = false;
        for (const auto &line : report.lines)
            if (contains(line, "Sarah") && contains(line, "Luis")) named_both = true;
        check(named_both, "the private-channel line does not name who has one");
    }

    // Shortfalls are NAMED. This is the reporting chain's whole purpose: a
    // count tells the operator that someone is short, not who.
    {
        auto plan = confirmed_plan();
        plan.requested = {"Sarah", "Luis", "Dana"};
        plan.uncovered_private = {"Dana"};
        const auto report = talkback_dock_nomination_report(plan);
        check(report.warn, "a nomination with a shortfall was not flagged");
        bool named = false;
        for (const auto &line : report.lines)
            if (contains(line, "Dana") && contains(line, "All")) named = true;
        check(named, "the uncovered nominee was not named with their remedy");
    }
    {
        auto plan = confirmed_plan();
        plan.requested = {"Sarah", "Luis", "Dana"};
        plan.uncovered_private = {"Dana"};
        plan.unreachable = {"Dana"};
        plan.all_talent_complete = false;
        const auto report = talkback_dock_nomination_report(plan);
        check(report.warn, "an unreachable nominee was not flagged");
        bool hears_nothing = false, fanout = false;
        for (const auto &line : report.lines) {
            if (contains(line, "Dana") && contains(line, "hear nothing"))
                hears_nothing = true;
            if (contains(line, "All talent does not reach everyone"))
                fanout = true;
        }
        check(hears_nothing,
              "an unreachable nominee was not reported as hearing nothing");
        check(fanout, "an incomplete all-talent fan-out was not reported");
    }

    // A refused attempt is diagnostic: it must be SHOWN, and it must not
    // erase the standing plan's own report (F1's symptom, at the UI).
    {
        auto plan = confirmed_plan();
        talkback_nomination_note_refused(plan, "create_busy");
        const auto report = talkback_dock_nomination_report(plan);
        check(report.warn, "a refused nominate attempt was not flagged");
        bool refused_line = false;
        for (const auto &line : report.lines)
            if (contains(line, "create_busy")) refused_line = true;
        check(refused_line, "the refusal reason was not shown");
        check(contains(report.headline, "3 channels"),
              "a refused attempt erased the standing plan from the report");
    }

    // A ladder that aborted after destroying the standing set, and a
    // superseded one, both reset `done` while keeping the reason. The report
    // must show that as "no channels, and here is why", never as silence.
    {
        TalkbackNominationPlan plan = confirmed_plan();
        talkback_nomination_note_failed_after_destroy(plan, "create_rate_limited");
        const auto report = talkback_dock_nomination_report(plan);
        check(contains(report.headline, "No channels assigned"),
              "a destroyed channel set was still reported as provisioned");
        bool why = false;
        for (const auto &line : report.lines)
            if (contains(line, "create_rate_limited")) why = true;
        check(why, "a destroyed channel set was reported with no reason");
    }

    // A long shortfall list is ELIDED on the visible line and complete in the
    // tooltip. The elision must never cost a name: naming a shortfall is the
    // entire purpose of the reporting chain (src/talkback-plan.h), so a
    // tooltip that dropped anyone would break the guarantee quietly.
    {
        auto plan = confirmed_plan();
        plan.requested = {"A", "B", "C", "D", "E", "F", "G"};
        plan.uncovered_private = {"A", "B", "C", "D", "E", "F", "G"};
        const auto report = talkback_dock_nomination_report(plan);
        bool elided = false;
        for (const auto &line : report.lines)
            if (contains(line, "and 2 more")) elided = true;
        check(elided, "a seven-name shortfall was not elided on the line");
        for (const auto &name : plan.uncovered_private)
            check(contains(report.tooltip, name),
                  "a name was dropped from the full-list tooltip");
        check(contains(report.tooltip, "G") &&
                  !contains(report.tooltip, "and 2 more"),
              "the tooltip repeated the elision instead of listing everyone");
    }
    {
        // At the threshold, nothing is elided -- an elision that fired early
        // would hide names for no reason.
        auto plan = confirmed_plan();
        plan.requested = {"A", "B", "C", "D", "E"};
        plan.uncovered_private = {"A", "B", "C", "D", "E"};
        const auto report = talkback_dock_nomination_report(plan);
        for (const auto &line : report.lines)
            check(!contains(line, "more"),
                  "a list at the elision threshold was elided anyway");
    }

    // ── The program-track warning ─────────────────────────────────────────
    //
    // `short_text` is the line the panel renders beside the source combo and
    // `text` is its tooltip. The split exists because the first live render
    // put the whole paragraph on the panel; the SHORT form still has to carry
    // the verdict on its own, because that is the only part most operators
    // will ever read.
    {
        const auto w = talkback_dock_track_warning("Talkback Mic", 0);
        check(!w.on_air_risk, "a source on no track was flagged as on air");
        check(w.tracks.empty(), "a source on no track listed tracks");
        check(contains(w.text, "dedicated"),
              "the safe-pattern text does not name the safe pattern");
        check(contains(w.short_text, "Off program") &&
                  contains(w.short_text, "safe"),
              "the short status does not say the source is off program");
    }
    {
        // Tracks 1 and 3 (bits 0 and 2) -- 1-based numbering, as OBS's
        // Advanced Audio Properties shows them.
        const auto w = talkback_dock_track_warning("Host Mic", 0b101u);
        check(w.on_air_risk, "a source on program tracks was not flagged");
        check(w.tracks == "1, 3",
              "the enabled tracks were not listed as OBS numbers them");
        check(contains(w.text, "Host Mic") && contains(w.text, "1, 3"),
              "the warning names neither the source nor its tracks");
        check(contains(w.short_text, "tracks 1, 3"),
              "the short status does not name the tracks that are on air");
        check(contains(w.short_text, "audience"),
              "the short status does not say who else hears it");
        check(w.short_text.size() < w.text.size(),
              "the short status is not shorter than the full explanation");
    }
    {
        // One track: the short line must not read "tracks 1".
        const auto w = talkback_dock_track_warning("Host Mic", 0b1u);
        check(w.on_air_risk, "a source on one program track was not flagged");
        check(contains(w.short_text, "track 1") &&
                  !contains(w.short_text, "tracks"),
              "a single track was announced in the plural");
    }
    {
        const auto w = talkback_dock_track_warning("", 0);
        check(!w.on_air_risk, "no chosen source was flagged as on air");
        check(contains(w.text, "No talkback source"),
              "an unchosen source did not say so");
        check(contains(w.short_text, "No source"),
              "the short status did not say no source was chosen");
    }

    // ── The ON AIR banner ─────────────────────────────────────────────────
    // The banner follows the ENGINE's confirmed state, never the plugin's
    // intent -- the spec's own requirement, and the C2 Critical that made it
    // one. It replaced the one-line tally when the dock became its own panel;
    // these are the tally's own claims, re-pinned on the thing that renders
    // them now.
    {
        // Nothing keyed, nothing to answer for: the quiet state.
        TalkbackDockSessionView s;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Off,
              "an idle panel was not shown as off air");
        check(contains(b.headline, "Off air"), "the idle banner did not say so");
        check(b.detail.empty(), "the idle banner invented a detail line");
    }
    {
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Waiting,
              "an unconfirmed key was shown as live");
        check(contains(b.headline, "Sarah"),
              "the waiting banner does not name the target");
        check(contains(b.detail, "Waiting"), "a pending key did not say so");
    }
    {
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_live = true;
        s.members_known = true;
        s.members_present = 2;
        s.members_total = 3;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Live,
              "a confirmed key was not shown as live");
        check(contains(b.headline, "ON AIR"),
              "the live banner does not say ON AIR");
        check(contains(b.headline, "Sarah"),
              "the live banner does not name the target");
        check(contains(b.headline, "2 of 3 present"),
              "the live banner does not report membership");
        check(!s.mic_blocked && b.state != TalkbackDockBannerState::LiveMicBlocked,
              "a key with an OPEN mic was shown as muted -- the blocked state "
              "must cost something to reach, or it means nothing when it fires");
    }
    // ── TALKBACK DELIVERY LAW 1 (2026-08-29): ON AIR TO NOBODY ──────────────
    //
    // Talkback delivers only while the engine's own meeting audio is open.
    // Muted, Zoom ACCEPTS every SendAudioDataToChannel -- success codes,
    // members confirmed, zero failures -- and every member hears silence. The
    // operator's own production that day had the bot muted by the host, so
    // this is the live case and not a hypothetical.
    //
    // The engine has reported `"mic":"blocked"` on its confirmed-state line
    // since the laws landed. REVIEW ROUND 1, M1: nothing on this side read it
    // -- no field on TalkbackSessionStatus, no input to the banner -- while
    // three comments and a commit message asserted the banner said it out
    // loud. This is the assertion that makes those true, and severing any link
    // in the chain (parse -> status -> view -> banner) fails it.
    {
        // DRIVEN FROM THE WIRE TOKEN, not from the bool, so this pins the
        // RULE end to end: the engine writes "blocked",
        // talkback_session_mic_blocked() is the rule the plugin's parser
        // applies to it, and the banner renders the result. Severing the rule
        // fails this. Two links it does NOT pin, because their TUs compile in
        // no host test: the parse assignment in zoom-engine-client.cpp and
        // the view.mic_blocked copy in zoom-talkback-panel.cpp -- those are
        // review-guarded, the same documented limit as every other wiring
        // call site in this feature.
        check(talkback_session_mic_blocked("blocked"),
              "the engine's own \"blocked\" token was not read as blocked");
        check(!talkback_session_mic_blocked("open"),
              "an OPEN mic was read as blocked");
        check(!talkback_session_mic_blocked(""),
              "an engine that reports no mic state at all was read as blocked "
              "-- a DLL-only install is this project's canonical mistake, and "
              "a permanent false alarm is one the operator learns to ignore");

        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_live = true;
        s.mic_blocked = talkback_session_mic_blocked("blocked");
        s.members_known = true;
        s.members_present = 3;
        s.members_total = 3;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::LiveMicBlocked,
              "A KEY LIVE OVER A MUTED BOT WAS SHOWN AS PLAIN ON AIR -- Zoom "
              "accepts every buffer and delivers silence, so this is the "
              "accepted-but-silent ghost reaching the operator as success");
        check(b.state != TalkbackDockBannerState::Live,
              "the blocked-mic state collapsed back into Live -- one enum "
              "value for both is exactly the plain ON AIR that hid the ghost");
        check(contains(b.headline, "ON AIR"),
              "the blocked banner stopped saying ON AIR -- the key IS open and "
              "the director IS talking; hiding that trades one wrong belief "
              "for another");
        check(contains(b.headline, "MUTED"),
              "the blocked banner does not say the bot is muted");
        check(contains(b.headline, "Sarah"),
              "the blocked banner does not name the target");
        check(!b.detail.empty() && contains(b.detail, "host"),
              "the blocked banner does not name the ACTION -- the operator's "
              "only move is to ask the host to unmute CoreVideo, and a state "
              "with no remedy is an alarm they cannot answer");
        check(!contains(b.headline, "3 of 3 present"),
              "the blocked banner reported membership -- \"3 of 3 present\" "
              "beside \"nobody can hear you\" reads as reassurance, and it is "
              "the exact instrument that made the ghost look healthy");
    }
    {
        // ...and it clears. A host unmuting the bot mid-key must put the
        // banner back to a clean ON AIR, or the alarm is permanent and the
        // operator learns to ignore it. (The engine re-emits its
        // confirmed-state line on the EDGE; this is the rendering half.)
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_live = true;
        s.mic_blocked = false;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Live,
              "a key whose mic was re-opened stayed in the blocked state");
    }
    {
        // mic_blocked is meaningful only alongside engine_live. A REFUSED key
        // has a reason of its own, and "you were refused AND the mic was shut"
        // is two answers to a question with one.
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_live = false;
        s.engine_reason = "target_not_provisioned";
        s.mic_blocked = true;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Refused,
              "a REFUSED key was shown as on air because the mic flag was set "
              "-- mic state must never promote a key the engine did not "
              "confirm, which is the C2 rule through a new door");
    }
    {
        // The all-talent sentinel is a target, not a name: it must never reach
        // the operator as the raw string the wire uses.
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = kTalkbackAllTalentTarget;
        s.engine_live = true;
        const auto b = talkback_dock_banner(s);
        check(contains(b.headline, "All talent"),
              "the all-talent target was not spelled out on the banner");
        check(b.headline.find(" all") == std::string::npos,
              "the raw all-talent sentinel leaked onto the banner");
    }
    {
        // An open key whose target the controller has not reported -- the n7
        // half-parse shape, or a control-API key seen before its target is
        // echoed. The headline is what an operator reads under pressure, so it
        // must not degrade to a dangling "ON AIR:" with nothing after it.
        TalkbackDockSessionView s;
        s.key_open = true;
        s.engine_live = true;
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Live,
              "an open, engine-confirmed key with no reported target was not "
              "shown as live");
        check(contains(b.headline, "no target"),
              "a key with no reported target left the banner headline dangling");
        check(talkback_dock_target_label("") == "no target",
              "an empty target rendered as an empty label");
    }
    {
        // Refused mid-ladder: the engine's own recovery hint is echoed, not
        // inferred -- the plugin never invents a remedy the engine did not
        // name.
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_reason = "provisioning_incomplete";
        s.engine_recover = "re-nominate";
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Refused,
              "a refused key was not flagged");
        check(contains(b.headline, "Sarah"),
              "the refusal banner does not name the target");
        check(contains(b.detail, "provisioning_incomplete"),
              "the refusal reason was not shown");
        check(contains(b.detail, "re-assign channels"),
              "the engine's recovery hint was not surfaced in the operator's "
              "own vocabulary");
        check(!contains(b.detail, "nominat"),
              "the operator-facing recovery hint still says nominate");
    }
    {
        // The recovery hint is still ECHOED, never inferred: a hint this
        // vocabulary does not know is passed through verbatim rather than
        // dropped or reworded, and no hint stays no hint.
        check(talkback_dock_recovery_label("re-nominate") ==
                  "re-assign channels",
              "the engine's re-nominate hint was not spelled for an operator");
        check(talkback_dock_recovery_label("restart the engine") ==
                  "restart the engine",
              "an unrecognised recovery hint was not passed through verbatim");
        TalkbackDockSessionView s;
        s.key_open = true;
        s.target = "Sarah";
        s.engine_reason = "provisioning_incomplete";
        const auto b = talkback_dock_banner(s);
        check(!contains(b.detail, "Recovery"),
              "a recovery hint was invented for a refusal that carried none");
    }
    {
        // The whole operator-facing report, swept for the word the owner
        // rejected. Internal vocabulary (the wire command, the code) is
        // deliberately untouched; this is only what is on screen.
        auto plan = confirmed_plan();
        plan.uncovered_private = {"Dana"};
        plan.unreachable = {"Dana"};
        talkback_nomination_note_refused(plan, "create_busy");
        const auto report = talkback_dock_nomination_report(plan);
        check(!contains(report.headline, "nominat") &&
                  !contains(report.tooltip, "nominat"),
              "the plan report still says nominate");
        for (const auto &line : report.lines)
            check(!contains(line, "nominat"),
                  "a plan report line still says nominate");
        bool failed_line = false;
        for (const auto &line : report.lines)
            if (contains(line, "Channel setup failed")) failed_line = true;
        check(failed_line, "a refusal is no longer named as a setup failure");
    }
    {
        // A closed key keeps the last verdict visible, said as history.
        TalkbackDockSessionView s;
        s.engine_reason = "channels_destroyed";
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Refused,
              "a closed key that had failed was shown as a clean idle state");
        check(contains(b.headline, "Off air"),
              "a closed key was not shown as off air");
        check(contains(b.detail, "Last key"),
              "the last key's verdict was not said as history");
        check(contains(b.detail, "channels_destroyed"),
              "the last key's failure reason was dropped once it closed");
    }
    {
        // A key that closed cleanly is Off, not Refused: an engine_live
        // session leaves its reason behind and it must not read as a failure.
        TalkbackDockSessionView s;
        s.engine_live = true;
        s.engine_reason = "ok";
        const auto b = talkback_dock_banner(s);
        check(b.state == TalkbackDockBannerState::Off,
              "a cleanly closed key was reported as a failure");
    }

    // ── M1: the mode CAPTURED AT THE PRESS governs closing ────────────────
    //
    // The Major this round fixes. Latch on, key Sarah, then uncheck Latch and
    // press Sarah again to close it: the checkbox now says push-to-talk, but
    // the key that is open is a latch, and only a press can close a latch. If
    // this decision reads the checkbox instead of the open key, the press
    // becomes an open attempt, key_on() refuses it as "already open", and the
    // director stays LIVE to talent with the dock's only close affordance
    // answering with an error.
    {
        const auto held = dock_key("Sarah", /*latched=*/true);
        check(talkback_dock_press_action(held, "Sarah", /*latch_selected=*/false) ==
                  TalkbackDockPressAction::CloseHeldKey,
              "unchecking Latch while a latched key is live made its own button "
              "stop closing it");
        check(talkback_dock_press_action(held, "Sarah", /*latch_selected=*/true) ==
                  TalkbackDockPressAction::CloseHeldKey,
              "a latched key's own button did not close it");
        // A different target is still an open attempt (key_on() refuses it, and
        // the button is disabled anyway) -- never a close of the wrong key.
        check(talkback_dock_press_action(held, "Luis", false) ==
                  TalkbackDockPressAction::OpenPushToTalk,
              "pressing another target closed the latched key");
    }
    {
        // The reverse interleaving: checking Latch while a PTT key is held must
        // not turn its button into a toggle -- the release is still coming.
        const auto held = dock_key("Sarah", /*latched=*/false);
        check(talkback_dock_press_action(held, "Sarah", /*latch_selected=*/true) !=
                  TalkbackDockPressAction::CloseHeldKey,
              "a push-to-talk key was treated as a latch because the checkbox "
              "changed underneath it");
    }
    {
        TalkbackDockOpenKey none;
        check(talkback_dock_press_action(none, "Sarah", false) ==
                  TalkbackDockPressAction::OpenPushToTalk,
              "a press with nothing open did not open push-to-talk");
        check(talkback_dock_press_action(none, "Sarah", true) ==
                  TalkbackDockPressAction::OpenLatch,
              "a press with Latch selected did not open a latch");
        // A latched key belonging to another surface is not the dock's to
        // toggle: key_on() will refuse, which is the honest outcome.
        auto other = dock_key("Sarah", true);
        other.dock_owned = false;
        check(talkback_dock_press_action(other, "Sarah", false) !=
                  TalkbackDockPressAction::CloseHeldKey,
              "the dock tried to close a key another surface owns");
    }

    // ── Release ───────────────────────────────────────────────────────────
    {
        check(talkback_dock_release_closes(dock_key("Sarah", false), "Sarah"),
              "releasing a held push-to-talk key did not close it");
        check(!talkback_dock_release_closes(dock_key("Sarah", true), "Sarah"),
              "releasing a latch closed it -- a latch is closed by the next "
              "press");
        check(!talkback_dock_release_closes(dock_key("Sarah", false), "Luis"),
              "a release closed a key on a different target");
        auto other = dock_key("Sarah", false);
        other.dock_owned = false;
        check(!talkback_dock_release_closes(other, "Sarah"),
              "a release closed a key another surface owns");
        TalkbackDockOpenKey none;
        check(!talkback_dock_release_closes(none, "Sarah"),
              "a stray release closed something with nothing open");
    }

    // ── The dock's lost-release backstop ──────────────────────────────────
    {
        check(talkback_dock_release_lost(dock_key("Sarah", false), false),
              "a held PTT key whose button is no longer down was not closed");
        check(!talkback_dock_release_lost(dock_key("Sarah", false), true),
              "a genuinely held PTT key was closed");
        check(!talkback_dock_release_lost(dock_key("Sarah", true), false),
              "a latch was closed for not being held -- nothing is held in "
              "latch mode");
        TalkbackDockOpenKey other = dock_key("Sarah", false);
        other.dock_owned = false;
        check(!talkback_dock_release_lost(other, false),
              "a key this dock does not own was closed by the dock");
        TalkbackDockOpenKey none;
        check(!talkback_dock_release_lost(none, false),
              "the backstop fired with no key open at all");
    }

    if (failures == 0) std::cout << "talkback-dock-state-test: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
