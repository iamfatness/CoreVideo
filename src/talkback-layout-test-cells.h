#pragma once
//
// talkback-layout-test-cells.h — the fake population behind
// COREVIDEO_TALKBACK_LAYOUT_TEST.
//
// THIS IS A LAYOUT-VERIFICATION INSTRUMENT, NOT A DEMO MODE. It exists because
// of a specific, repeated failure: three separate rounds of work on the
// Talkback dock's grid were certified by an offscreen Qt harness -- real cells,
// real fonts, measured renders -- and all three were then CLIPPED on the
// operator's actual OBS dock (the last one, 2026-08-30, gave the grid ~90 px,
// cut the single "All talent / assigning..." cell across its state line, and
// left 500 px of empty panel underneath). A harness is not the operator's
// screen: it has a different platform plugin, a different style, a different
// DPI and a different parent chain, and the defects have lived in exactly those
// differences.
//
// So the verification moved into the real product. With the environment
// variable set, the panel populates ITSELF, at construction, with this cast:
// every cell state at once, long real-shaped names included, and no engine, no
// meeting and no Zoom SDK anywhere near it. Whoever is verifying opens OBS,
// looks at the dock, and sees the real widget tree under the real theme.
//
// Pure, Qt-free and OBS-free for the same reason every other decision in this
// feature is (src/talkback-dock-state.h's header): the claim "the instrument
// renders every state" is one a reviewer acts on, so it is pinned by
// tests/talkback-layout-test-cells-test.cpp rather than asserted in a comment.

#include "talkback-dock-state.h"
#include "talkback-nomination.h"
#include "talkback-plan.h"

#include <string>
#include <vector>

// How many fake people the instrument makes when the environment variable is
// set but carries no usable number.
constexpr int kTalkbackLayoutTestDefaultPeople = 8;
// A ceiling, so a typo cannot ask for ten thousand widgets. Comfortably past
// the 24-talent show this grid was redesigned for.
constexpr int kTalkbackLayoutTestMaxPeople = 64;

namespace talkback_layout_test_detail {

// Display names shaped like the ones that have actually broken this grid.
// NON-ASCII IS WRITTEN AS EXPLICIT UTF-8 BYTES: these sources carry no BOM and
// the build passes MSVC no /utf-8, so a raw glyph in a narrow literal would be
// decoded as the system codepage (the same rule the banner's tally dot follows
// in zoom-talkback-panel.cpp). "\xC3\xB8" is U+00F8, the o-slash.
inline const std::vector<std::string> &names()
{
    static const std::vector<std::string> kNames = {
        // The 28-character name that flipped the first adaptive grid into a
        // single 400 px column, with its two real diacritics.
        "Ronny Hofs\xC3\xB8y, Troms\xC3\xB8, Norway",
        // Two names that differ only LATE: if a cell ever elides from the
        // front, or clips both ends the way a centred QPushButton does, these
        // two read identically -- and this control opens a live microphone to
        // one named person.
        "Jeffrey Wiltshire",
        "Jeffrey Wiltshaw",
        // 38 characters: the monster. Nothing about the layout may depend on
        // it fitting.
        "Dr. Alexandra Fitzwilliam-Featherstone",
        "Grant Whitehead",
        // Short, because a grid that only survives long names is not a grid.
        "Al",
        "Priya Raghunathan",
        "Luis",
        "Sarah Kim",
        "Mika",
    };
    return kNames;
}

// EVERY STATE THE GRID CAN PAINT, in the order the cells cycle through them.
// The sixth is NoChannel again but against a plan that has NOT reported a
// terminal, which renders "assigning..." instead of "no channel" -- a distinct
// line, and the exact one that was cut in half on the operator's screen.
enum class LayoutTestVariant {
    Ready,
    OnAir,
    NoChannel,
    NotInChannel,
    NoTalkback,
    Assigning,
};

inline const std::vector<LayoutTestVariant> &variants()
{
    static const std::vector<LayoutTestVariant> kVariants = {
        LayoutTestVariant::Ready,      LayoutTestVariant::OnAir,
        LayoutTestVariant::NoChannel,  LayoutTestVariant::NotInChannel,
        LayoutTestVariant::NoTalkback, LayoutTestVariant::Assigning,
    };
    return kVariants;
}

} // namespace talkback_layout_test_detail

// A plan shaped like one an operator would be looking at mid-assignment: some
// people covered, some short, some unreachable, and a name run long enough to
// exercise the report's own elision. Rendered by the real
// talkback_dock_nomination_report().
inline TalkbackNominationPlan talkback_layout_test_plan()
{
    using namespace talkback_layout_test_detail;
    TalkbackNominationPlan plan;
    plan.done = true;
    plan.channels = 9;
    plan.all_talent_complete = false;
    plan.last_attempt_ok = true;
    for (const auto &name : names())
        plan.requested.push_back(name);
    // Short by two, and one of those two reaches nobody at all -- the two
    // shortfall lines the report renders separately.
    plan.uncovered_private = {names()[3], names()[6]};
    plan.unreachable = {names()[6]};
    return plan;
}

// The instrument's cast: the all-talent key first, then `people` person cells
// cycling through every state.
//
// The ALL-TALENT cell is deliberately the "assigning..." one. That is the exact
// cell the operator's 2026-08-30 screenshot cut in half -- one full-width cell,
// two lines, at the top of a grid area that had been given ~90 px -- so the
// instrument reproduces the failing shape first and everything else after it.
inline std::vector<TalkbackDockCell> talkback_layout_test_cells(int people)
{
    using namespace talkback_layout_test_detail;

    std::vector<TalkbackDockCell> cells;

    TalkbackDockCell all;
    all.target     = kTalkbackAllTalentTarget;
    all.label      = "All talent";
    all.all_talent = true;
    all.enabled    = false;
    all.reason     = "no one has a channel yet";
    all.state      = TalkbackDockCellState::NoChannel;
    {
        TalkbackNominationPlan assigning;   // done == false
        all.state_line = talkback_dock_cell_state_line(all.state, assigning, true);
    }
    all.hint = talkback_dock_cell_hint(all.state, true);
    cells.push_back(all);

    if (people < 0) people = 0;
    if (people > kTalkbackLayoutTestMaxPeople) people = kTalkbackLayoutTestMaxPeople;

    TalkbackNominationPlan done;
    done.done = true;
    TalkbackNominationPlan assigning;   // done == false

    bool on_air_used = false;
    for (int i = 0; i < people; ++i) {
        const auto &pool = names();
        const std::size_t index = static_cast<std::size_t>(i) % pool.size();
        const int lap = i / static_cast<int>(pool.size());

        TalkbackDockCell cell;
        // Unique targets even past a lap of the name pool: the panel matches
        // cells to specs by target, so two cells sharing one would paint each
        // other's state.
        cell.label  = lap == 0 ? pool[index]
                               : pool[index] + " " + std::to_string(lap + 1);
        cell.target = cell.label;

        auto variant = variants()[static_cast<std::size_t>(i) % variants().size()];
        // EXACTLY ONE ON AIR, however long the cast runs. Only one talkback key
        // can be open at a time, and this dock's standing rule is that red
        // means the director is audible to THAT person -- two red cells would
        // be an instrument rendering a state the product cannot reach, which is
        // the opposite of the point. Later laps of the cycle land on Ready.
        if (variant == LayoutTestVariant::OnAir) {
            if (on_air_used) variant = LayoutTestVariant::Ready;
            on_air_used = true;
        }
        switch (variant) {
        case LayoutTestVariant::Ready:
            cell.state = TalkbackDockCellState::Ready;
            cell.enabled = true;
            break;
        case LayoutTestVariant::OnAir:
            cell.state = TalkbackDockCellState::OnAir;
            cell.enabled = true;
            break;
        case LayoutTestVariant::NoChannel:
            cell.state = TalkbackDockCellState::NoChannel;
            cell.reason = "no private channel. Key All talent instead, or "
                          "assign channels again.";
            break;
        case LayoutTestVariant::NotInChannel:
            cell.state = TalkbackDockCellState::NotInChannel;
            cell.enabled = true;
            break;
        case LayoutTestVariant::NoTalkback:
            cell.state = TalkbackDockCellState::Unreachable;
            cell.reason = "on no channel at all. Assign channels for a shorter "
                          "list.";
            break;
        case LayoutTestVariant::Assigning:
            cell.state = TalkbackDockCellState::NoChannel;
            cell.reason = "no one has a channel yet";
            break;
        }
        // The real words, from the real helpers, so the instrument cannot show
        // a line the product does not.
        cell.state_line = talkback_dock_cell_state_line(
            cell.state,
            variant == LayoutTestVariant::Assigning ? assigning : done, false);
        cell.hint = talkback_dock_cell_hint(cell.state, false);
        cells.push_back(std::move(cell));
    }
    return cells;
}

// Who the banner should call ON AIR: the person whose cell is in the OnAir
// state, so the banner and the red cell agree the way the product's own
// "red means the director is audible" rule requires. Falls back to the
// all-talent sentinel when the cast is too small to contain one.
inline std::string talkback_layout_test_live_target(
    const std::vector<TalkbackDockCell> &cells)
{
    for (const auto &cell : cells)
        if (cell.state == TalkbackDockCellState::OnAir) return cell.target;
    return kTalkbackAllTalentTarget;
}
