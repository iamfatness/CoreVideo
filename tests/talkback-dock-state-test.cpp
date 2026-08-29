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
        // Two columns only when two of the WIDEST button fit with the gap
        // between them. Anything less is one full-width column -- which is
        // what stops a name being clipped mid-glyph at both ends, the defect
        // that made "Grant Whitehead" render as "rant Whitehead".
        check(talkback_dock_key_columns(320, 150, 8) == 2,
              "two buttons that fit were not given two columns");
        check(talkback_dock_key_columns(308, 150, 8) == 2,
              "the exact two-column fit was rejected");
        check(talkback_dock_key_columns(307, 150, 8) == 1,
              "two columns were used one pixel too narrow, which is where the "
              "clipping starts");
        check(talkback_dock_key_columns(200, 150, 8) == 1,
              "a narrow dock kept two columns");
        // The gap counts: it is the thing that makes the fit exact.
        check(talkback_dock_key_columns(300, 150, 0) == 2,
              "the gap was charged when there was none");
        check(talkback_dock_key_columns(300, 150, 8) == 1,
              "the gap was not charged against the fit");
        // Never zero columns, whatever it is asked.
        check(talkback_dock_key_columns(0, 150, 8) == 1,
              "an unrealised width produced no columns at all");
        check(talkback_dock_key_columns(320, 0, 8) == 1,
              "an unmeasured label produced no columns at all");
        check(talkback_dock_key_columns(10, 4000, 8) == 1,
              "a label wider than the dock produced no columns at all");
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
