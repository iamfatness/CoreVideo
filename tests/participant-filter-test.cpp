// tests/participant-filter-test.cpp
// Which participants a VIDEO-assignment picker offers.
//
// The operator asked for this after a 16-person meeting with 5 cameras on: the
// pickers listed all 16, and the 11 without video cannot go into an output or a
// tile at all. The filter is a display convenience, but one rule in it is not:
// the participant a picker is CURRENTLY pointed at is never hidden, even with
// their camera off. A picker that dropped its own current value would lose the
// selection on the next roster tick and silently unbind the source -- which is
// how a camera being switched off mid-show would turn into a black output
// nobody asked for.

#include "participant-filter.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static std::vector<ParticipantInfo> sample_roster()
{
    std::vector<ParticipantInfo> roster;
    ParticipantInfo a; a.user_id = 101; a.display_name = "Camera On";  a.has_video = true;
    ParticipantInfo b; b.user_id = 202; b.display_name = "Camera Off"; b.has_video = false;
    ParticipantInfo c; c.user_id = 303; c.display_name = "Also On";    c.has_video = true;
    roster.push_back(a); roster.push_back(b); roster.push_back(c);
    return roster;
}

int main()
{
    // --- Filter off: the list is untouched, in order ---
    {
        const auto out = visible_for_video_assignment(sample_roster(), false, 0);
        check(out.size() == 3, "the disabled filter dropped somebody");
        check(out.size() == 3 && out[0].user_id == 101 &&
                  out[1].user_id == 202 && out[2].user_id == 303,
              "the disabled filter reordered the roster");
    }

    // --- Filter on: camera-off participants are dropped, order preserved ---
    {
        const auto out = visible_for_video_assignment(sample_roster(), true, 0);
        check(out.size() == 2, "the filter kept a camera-off participant");
        check(out.size() == 2 && out[0].user_id == 101 && out[1].user_id == 303,
              "the filter did not preserve roster order");
    }

    // --- The currently assigned participant survives with their camera off ---
    {
        const auto out = visible_for_video_assignment(sample_roster(), true, 202);
        check(out.size() == 3,
              "the picker's own current selection was filtered out -- it would "
              "lose the selection on the next roster tick and unbind the source");
        check(out.size() == 3 && out[1].user_id == 202,
              "the kept participant was moved out of roster order");
    }

    // --- keep_user_id naming somebody absent from the roster adds nobody ---
    {
        const auto out = visible_for_video_assignment(sample_roster(), true, 999);
        check(out.size() == 2,
              "an absent keep_user_id conjured a participant into the list");
    }

    // --- Empty roster stays empty either way ---
    {
        const std::vector<ParticipantInfo> empty;
        check(visible_for_video_assignment(empty, true, 0).empty(),
              "an empty roster produced entries with the filter on");
        check(visible_for_video_assignment(empty, false, 0).empty(),
              "an empty roster produced entries with the filter off");
    }

    // --- Everybody camera-off yields an empty list rather than falling back to
    // the full roster: an empty picker is the honest answer ---
    {
        std::vector<ParticipantInfo> roster;
        ParticipantInfo a; a.user_id = 101; a.has_video = false;
        ParticipantInfo b; b.user_id = 202; b.has_video = false;
        roster.push_back(a); roster.push_back(b);
        check(visible_for_video_assignment(roster, true, 0).empty(),
              "a roster with no cameras on fell back to showing everybody");
    }

    if (failures == 0)
        std::cout << "participant-filter: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
