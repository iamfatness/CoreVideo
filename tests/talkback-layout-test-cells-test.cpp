// tests/talkback-layout-test-cells-test.cpp
//
// The cast behind COREVIDEO_TALKBACK_LAYOUT_TEST -- the Talkback dock's
// layout-verification instrument (src/talkback-layout-test-cells.h).
//
// WHY AN INSTRUMENT NEEDS TESTS. The instrument's whole value is a claim
// somebody acts on without checking: "open OBS with this set and you are
// looking at EVERY state the grid can paint, including the long names and the
// cell that was cut in half on 2026-08-30". If it silently stopped emitting one
// of the six states, the verification it exists for would pass while the
// unrendered state stayed broken -- which is the same shape as the three
// offscreen certifications that already failed this feature, one level up.
// So the coverage claim is pinned here rather than asserted in a comment.
//
// Nothing here renders anything: this file pins the DATA the panel is handed.
// Whether Qt then lays it out without clipping is what the operator's own OBS
// answers, and this project has learned the hard way that nothing else can.
#include "talkback-layout-test-cells.h"

#include <algorithm>
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

static bool has_state_line(const std::vector<TalkbackDockCell> &cells,
                           const std::string &line)
{
    for (const auto &cell : cells)
        if (cell.state_line == line) return true;
    return false;
}

static bool has_state(const std::vector<TalkbackDockCell> &cells,
                      TalkbackDockCellState state)
{
    for (const auto &cell : cells)
        if (cell.state == state) return true;
    return false;
}

int main()
{
    // ── The shape of the cast ─────────────────────────────────────────────
    {
        const auto cells = talkback_layout_test_cells(8);
        check(cells.size() == 9,
              "eight requested people did not produce eight cells plus the "
              "all-talent key");
        check(cells.front().all_talent &&
                  cells.front().target == kTalkbackAllTalentTarget,
              "the all-talent key was not the first cell");
        // The exact cell the operator's screenshot cut across its state line.
        check(cells.front().state_line == "assigning...",
              "the all-talent cell did not reproduce the two-line "
              "'assigning...' state the 2026-08-30 render clipped");

        for (const auto &cell : cells) {
            check(!cell.label.empty(), "a cell had no name to render");
            check(!cell.target.empty(), "a cell had no target");
            check(!cell.state_line.empty(),
                  "a cell had no state line -- the second line is half of what "
                  "the instrument exists to size");
        }

        std::vector<std::string> targets;
        for (const auto &cell : cells) targets.push_back(cell.target);
        std::sort(targets.begin(), targets.end());
        check(std::adjacent_find(targets.begin(), targets.end()) == targets.end(),
              "two cells shared a target -- the panel matches specs to cells by "
              "target and would paint one with the other's state");
    }

    // ── EVERY state, which is the coverage claim ──────────────────────────
    {
        const auto cells = talkback_layout_test_cells(8);
        check(has_state_line(cells, "ready"), "no cell rendered 'ready'");
        check(has_state_line(cells, "ON AIR"), "no cell rendered 'ON AIR'");
        check(has_state_line(cells, "no channel"),
              "no cell rendered 'no channel'");
        check(has_state_line(cells, "not in channel"),
              "no cell rendered 'not in channel'");
        check(has_state_line(cells, "no talkback"),
              "no cell rendered 'no talkback'");
        check(has_state_line(cells, "assigning..."),
              "no cell rendered 'assigning...'");

        check(has_state(cells, TalkbackDockCellState::Ready), "no Ready cell");
        check(has_state(cells, TalkbackDockCellState::OnAir), "no OnAir cell");
        check(has_state(cells, TalkbackDockCellState::NoChannel),
              "no NoChannel cell");
        check(has_state(cells, TalkbackDockCellState::NotInChannel),
              "no NotInChannel cell");
        check(has_state(cells, TalkbackDockCellState::Unreachable),
              "no Unreachable cell");

        // EXACTLY ONE, at any cast size: only one talkback key can be open, and
        // this dock's rule is that red means the director is audible to that
        // one person. An instrument showing two red cells would be rendering a
        // state the product cannot reach.
        for (int people : {2, 8, 24, 64}) {
            int on_air = 0;
            for (const auto &cell : talkback_layout_test_cells(people))
                if (cell.state == TalkbackDockCellState::OnAir) ++on_air;
            check(on_air == 1, "the cast did not contain exactly one ON AIR cell");
        }

        // A disabled cell still has to say WHY, because the instrument is also
        // how the tooltip's two-line shape gets looked at.
        for (const auto &cell : cells)
            if (!cell.enabled)
                check(!cell.reason.empty(),
                      "a disabled cell carried no refusal reason");
    }

    // ── The names that have actually broken this grid ─────────────────────
    {
        const auto cells = talkback_layout_test_cells(8);
        std::size_t longest = 0;
        for (const auto &cell : cells)
            longest = std::max(longest, cell.label.size());
        check(longest >= 38,
              "no long name in the cast -- one 28-character display name is "
              "what built the 400 px tower this grid replaced");

        bool has_non_ascii = false;
        for (const auto &cell : cells)
            for (unsigned char ch : cell.label)
                if (ch > 0x7F) has_non_ascii = true;
        check(has_non_ascii,
              "no non-ASCII name in the cast -- the real one that broke this "
              "grid was Norwegian");
    }

    // ── The edges of the count ────────────────────────────────────────────
    {
        check(talkback_layout_test_cells(0).size() == 1,
              "zero people did not leave exactly the all-talent key");
        check(talkback_layout_test_cells(-4).size() == 1,
              "a negative count was not clamped to nothing");
        check(talkback_layout_test_cells(100000).size() ==
                  static_cast<std::size_t>(kTalkbackLayoutTestMaxPeople) + 1,
              "an absurd count was not clamped to the ceiling");
        // Past a lap of the name pool the labels repeat but the TARGETS must
        // not.
        const auto many = talkback_layout_test_cells(24);
        std::vector<std::string> targets;
        for (const auto &cell : many) targets.push_back(cell.target);
        std::sort(targets.begin(), targets.end());
        check(std::adjacent_find(targets.begin(), targets.end()) == targets.end(),
              "a 24-person cast reused a target after lapping the name pool");
    }

    // ── The banner agrees with the red cell ───────────────────────────────
    {
        const auto cells = talkback_layout_test_cells(8);
        const std::string live = talkback_layout_test_live_target(cells);
        bool matched = false;
        for (const auto &cell : cells)
            if (cell.target == live)
                matched = cell.state == TalkbackDockCellState::OnAir;
        check(matched,
              "the banner's ON AIR target was not the cell painted ON AIR -- "
              "red means the director is audible, on the strip and on the cell");
        // With nobody to be on air, it must still name something the panel can
        // render rather than an empty headline.
        check(talkback_layout_test_live_target(talkback_layout_test_cells(0)) ==
                  kTalkbackAllTalentTarget,
              "an empty cast left the banner with no target at all");
    }

    // ── The fake plan report ──────────────────────────────────────────────
    {
        const auto report =
            talkback_dock_nomination_report(talkback_layout_test_plan());
        check(report.warn,
              "the instrument's plan did not exercise the warned (shortfall) "
              "rendering of the report block");
        check(!report.headline.empty() && report.lines.size() >= 3,
              "the instrument's plan did not produce a multi-line report -- the "
              "block's own height is part of what is being verified");
        check(report.tooltip.size() > report.headline.size(),
              "the instrument's plan did not exercise the report's name "
              "elision, whose full list lives in the tooltip");
    }

    if (failures == 0)
        std::cout << "talkback-layout-test-cells-test: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
