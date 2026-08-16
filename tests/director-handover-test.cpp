// tests/director-handover-test.cpp
// How long the hidden director-preview subscription is kept alive after a cut.
//
// The incident this guards (2026-08-16, live show): the cut released the main
// source's SHM mapping, asked the engine for a new one on the main uuid, and
// unsubscribed the preview in the same breath. The engine takes 735-1277 ms to
// rebuild the region, and on_director_preview_frame() -- the thing that had
// been publishing the new speaker to air -- stops firing the moment the preview
// is unsubscribed. Nothing published for the best part of a second, 18 times in
// one show, and it read on air as the speaker flashing.
//
// The rule: hold the preview until the MAIN subscription delivers a frame for
// the participant we cut to, then release it. Abandon on a timeout so a main
// subscription that never delivers cannot pin the preview (and its SHM slot,
// one of kMaxShmSources) for the rest of the session.

#include "director-handover.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main()
{
    // --- Not in a handover: the rule must not touch anything ---
    check(director_handover_action(false, 0, 67109888, 0,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::None,
          "a source that is not mid-handover was acted on");

    // --- The main subscription delivered the participant we cut to. This is
    // the whole point: release the preview now, and only now ---
    check(director_handover_action(true, 67109888, 67109888, 100,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::Complete,
          "the main subscription delivered the new speaker but the preview was "
          "not released -- it would pin an SHM slot indefinitely");

    // --- Still waiting: the main subscription has delivered nothing yet, or
    // has delivered a stale frame for the previous speaker. Hold the preview,
    // because it is the only thing putting a picture on air ---
    check(director_handover_action(true, 67109888, 0, 100,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::Hold,
          "the preview was released before the main subscription delivered -- "
          "this is the gap that flashed on air");

    check(director_handover_action(true, 67109888, 50345984, 100,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::Hold,
          "a late frame for the PREVIOUS speaker completed the handover -- the "
          "main mapping is not ready and releasing now reopens the gap");

    // --- The main subscription never delivered. Give the preview back anyway:
    // holding it forever costs one of the 32 shared-memory slots ---
    check(director_handover_action(true, 67109888, 0,
                                   kDirectorHandoverTimeoutMs,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::AbandonOnTimeout,
          "a main subscription that never delivered pinned the preview past the "
          "timeout");

    // --- A delivered frame wins over the timeout: if both are true we would
    // rather complete cleanly than abandon ---
    check(director_handover_action(true, 67109888, 67109888,
                                   kDirectorHandoverTimeoutMs * 2,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::Complete,
          "a late but correct frame was treated as a timeout abandonment");

    // --- A target of 0 is not a participant. Never complete on it, or a
    // handover would finish against nothing and reopen the gap ---
    check(director_handover_action(true, 0, 0, 100,
                                   kDirectorHandoverTimeoutMs) ==
              DirectorHandoverAction::Hold,
          "a handover completed against a zero target");

    if (failures == 0)
        std::cout << "director-handover: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
