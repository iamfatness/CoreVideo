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
    // Simulate a run of ticks that all report not-live, exactly the shape
    // evaluate() sees for a key that never got a confirmed channel: the
    // grace-period close happens on a DIFFERENT path (talkback-key.h /
    // key_off()'s unconditional close cue), never through this edge
    // function reporting Open.
    {
        bool prev = false;
        TalkbackCue seen = TalkbackCue::None;
        for (int i = 0; i < 5; ++i) {
            const TalkbackCue cue = talkback_cue_on_live_change(prev, false);
            if (cue != TalkbackCue::None) seen = cue;
            prev = false;
        }
        check(seen == TalkbackCue::None,
              "a session that never went live produced an OPEN cue somewhere "
              "in a run of not-live ticks");
    }

    // ── Repeated identical values yield nothing, whichever value repeats ─
    check(talkback_cue_on_live_change(true, true) == TalkbackCue::None,
          "repeated `true` yielded a cue");
    check(talkback_cue_on_live_change(false, false) == TalkbackCue::None,
          "repeated `false` yielded a cue");

    if (failures == 0)
        std::cout << "talkback-cue: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
