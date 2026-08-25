// tests/talkback-channel-owner-test.cpp
// Who a CreateChannel response belongs to.
//
// Two subsystems now ask the Zoom SDK for talkback channels: the Milestone 1
// probe (creates one, sends a 3s tone, destroys it) and the talkback session
// (creates one and holds it open while a key is down). CreateChannel(1) does
// not return the id -- it arrives later in onCreateChannelResponse, which
// carries no indication of who asked.
//
// Get this wrong in one direction and the probe adopts the session's channel
// and destroys it three seconds later, cutting the director off mid-sentence.
// Get it wrong in the other and the session adopts the probe's channel, which
// tick() then destroys underneath it. Both are silent failures on a live show,
// so the decision is a pure function pinned here.
//
// The rule is deliberately strict: exactly ONE create may be outstanding. Both
// callers run on the engine's single command-loop thread, so serialising is
// free -- and a queue would only add a way for the two to interleave.
#include "talkback-channel-owner.h"

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
    // ── With nothing outstanding, either subsystem may ask ─────────────────
    check(talkback_may_request_create(TalkbackChannelOwner::None),
          "a create was refused while nothing was outstanding");

    // ── While one is outstanding, nobody else may ask ──────────────────────
    check(!talkback_may_request_create(TalkbackChannelOwner::Probe),
          "a second create was allowed while the probe's was outstanding");
    check(!talkback_may_request_create(TalkbackChannelOwner::Session),
          "a second create was allowed while the session's was outstanding");

    // ── The response goes to whoever is outstanding, and clears it ─────────
    check(talkback_claim_create(TalkbackChannelOwner::Probe) ==
              TalkbackChannelOwner::Probe,
          "the probe's create response was not routed to the probe");
    check(talkback_claim_create(TalkbackChannelOwner::Session) ==
              TalkbackChannelOwner::Session,
          "the session's create response was not routed to the session");

    // ── An UNEXPECTED response belongs to nobody ───────────────────────────
    // A late or duplicate response with nothing outstanding must NOT be
    // adopted. Adopting it would hand one subsystem a channel the other is
    // about to destroy -- and the SDK genuinely can redeliver.
    check(talkback_claim_create(TalkbackChannelOwner::None) ==
              TalkbackChannelOwner::None,
          "a create response arriving with nothing outstanding was adopted by "
          "somebody -- it must belong to nobody and be destroyed as a stray");

    if (failures == 0)
        std::cout << "talkback-channel-owner: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
