// tests/tile-fill-test.cpp
// Covers resolve_tile_assignments(), which decides who appears on the tile
// wall and in what order. The stability rule (case 1) is the one that matters
// most on air: without it, any roster reordering by the SDK moves every face.

#include "zoom-tile-fill.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

ParticipantInfo person(uint32_t id, bool has_video)
{
    ParticipantInfo p;
    p.user_id = id;
    p.display_name = "User " + std::to_string(id);
    p.has_video = has_video;
    return p;
}

bool expect(const char *name, const std::vector<uint32_t> &got,
            const std::vector<uint32_t> &want)
{
    if (got == want) return true;
    std::cerr << name << ": expected [";
    for (uint32_t id : want) std::cerr << id << " ";
    std::cerr << "], got [";
    for (uint32_t id : got) std::cerr << id << " ";
    std::cerr << "]\n";
    return false;
}

TileFillParams auto_params()
{
    TileFillParams p;
    p.mode = TileFillMode::Auto;
    p.max_tiles = 9;
    return p;
}

// A newcomer must land at the end. If the resolver rebuilt from roster order
// instead, 30 would sort into the middle and two faces would swap positions.
bool test_auto_keeps_incumbent_order()
{
    const std::vector<uint32_t> previous{20, 10};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("incumbent order",
                  resolve_tile_assignments(previous, roster, auto_params()),
                  {20, 10, 30});
}

bool test_auto_drops_camera_off_and_closes_gap()
{
    const std::vector<uint32_t> previous{10, 20, 30};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, false), person(30, true)};
    return expect("camera off drops",
                  resolve_tile_assignments(previous, roster, auto_params()),
                  {10, 30});
}

// Incumbents win the contest for scarce slots; the newest arrival is cut.
bool test_auto_honors_max_tiles()
{
    TileFillParams params = auto_params();
    params.max_tiles = 2;
    const std::vector<uint32_t> previous{10, 20};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("max tiles",
                  resolve_tile_assignments(previous, roster, params), {10, 20});
}

bool test_auto_honors_excludes_and_ignores_zeros()
{
    TileFillParams params = auto_params();
    params.excluded = {20, 0, 0};  // empty exclude slots are zeros
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, true), person(30, true)};
    return expect("excludes", resolve_tile_assignments({}, roster, params),
                  {10, 30});
}

// A departed participant must not linger just because they were on the wall.
bool test_auto_empty_roster_clears_the_wall()
{
    return expect("empty roster",
                  resolve_tile_assignments({10, 20}, {}, auto_params()), {});
}

bool test_manual_passes_through_and_drops_holes()
{
    TileFillParams params;
    params.mode = TileFillMode::Manual;
    params.max_tiles = 9;
    params.manual = {30, 0, 10, 0};  // 0 == "None" in the dropdowns
    const std::vector<ParticipantInfo> roster{person(10, true), person(30, true)};
    return expect("manual passthrough",
                  resolve_tile_assignments({}, roster, params), {30, 10});
}

// Casting is a decision, not a liveness query: a cast participant with their
// camera off keeps the slot the operator gave them.
bool test_manual_keeps_camera_off_participant()
{
    TileFillParams params;
    params.mode = TileFillMode::Manual;
    params.max_tiles = 9;
    params.manual = {10, 20};
    const std::vector<ParticipantInfo> roster{
        person(10, true), person(20, false)};
    return expect("manual keeps camera-off",
                  resolve_tile_assignments({}, roster, params), {10, 20});
}

// Two dropdowns pointed at the same person must not open two subscriptions.
bool test_duplicates_collapse_in_both_modes()
{
    TileFillParams manual;
    manual.mode = TileFillMode::Manual;
    manual.max_tiles = 9;
    manual.manual = {10, 10, 20};
    if (!expect("manual duplicates",
                resolve_tile_assignments({}, {}, manual), {10, 20}))
        return false;

    const std::vector<ParticipantInfo> roster{person(10, true)};
    return expect("auto duplicates",
                  resolve_tile_assignments({10, 10}, roster, auto_params()),
                  {10});
}

}  // namespace

int main()
{
    if (!test_auto_keeps_incumbent_order()) return 1;
    if (!test_auto_drops_camera_off_and_closes_gap()) return 1;
    if (!test_auto_honors_max_tiles()) return 1;
    if (!test_auto_honors_excludes_and_ignores_zeros()) return 1;
    if (!test_auto_empty_roster_clears_the_wall()) return 1;
    if (!test_manual_passes_through_and_drops_holes()) return 1;
    if (!test_manual_keeps_camera_off_participant()) return 1;
    if (!test_duplicates_collapse_in_both_modes()) return 1;

    std::cout << "tile-fill: all tests passed\n";
    return 0;
}
