// tests/talkback-cue-test.cpp
// When the talkback open/close audio cue fires.
//
// The open cue marks the moment the Zoom channel's create/invite round trip
// actually finishes (engine-confirmed live), not the key press -- see
// src/talkback-cue.h's header comment for why cueing at key press would be
// worse than no cue at all. That means the decision is purely a function of
// the engine's own `live` transitions, extracted here the same way
// src/talkback-key.h and src/talkback-channel-owner.h extract their
// decisions, so it can be driven exhaustively with no engine, no meeting,
// and no sound card.
#include "talkback-cue.h"

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
    // ── false -> true yields OPEN exactly once ──────────────────────────
    check(talkback_cue_on_live_change(false, true) == TalkbackCue::Open,
          "a false->true transition did not yield the OPEN cue");

    // ── ...not on every subsequent tick while it stays true ────────────
    check(talkback_cue_on_live_change(true, true) == TalkbackCue::None,
          "a true->true tick (already live) yielded a cue -- OPEN must fire "
          "once, on the transition, not on every tick the session stays live");

    // ── true -> false yields CLOSE exactly once ─────────────────────────
    check(talkback_cue_on_live_change(true, false) == TalkbackCue::Close,
          "a true->false transition did not yield the CLOSE cue");

    // ── ...not on every subsequent tick while it stays false ───────────
    check(talkback_cue_on_live_change(false, false) == TalkbackCue::None,
          "a false->false tick yielded a cue");

    // ── A session that never goes live yields NO open cue ──────────────
    // This is exactly the false->false case already checked above: the
    // function is pure and stateless over a 2-boolean input, so "a run of N
    // not-live ticks" and "one not-live tick" exercise the identical branch
    // -- looping it added no coverage the check above didn't already give,
    // which is worse than not having the loop (a test that LOOKS like a
    // multi-tick simulation but isn't). The grace-period close for a key
    // that never got a confirmed channel happens on a DIFFERENT path
    // entirely (key_off()'s unconditional close cue in talkback-controller
    // .cpp), never through this edge function reporting Open -- that
    // integration is exercised by TalkbackController, not this pure-function
    // test.

    // ── Repeated identical values yield nothing, whichever value repeats ─
    check(talkback_cue_on_live_change(true, true) == TalkbackCue::None,
          "repeated `true` yielded a cue");
    check(talkback_cue_on_live_change(false, false) == TalkbackCue::None,
          "repeated `false` yielded a cue");

    if (failures == 0)
        std::cout << "talkback-cue: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
