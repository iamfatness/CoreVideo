// Pins the half of the engine-restart fix that is NOT about shared memory.
//
// tests/shm-engine-restart-test.cpp pins why every source must hand back its
// mapping when a new ZoomObsEngine process comes up. That release is a
// precondition for the new engine's first region create — but a precondition is
// not a recovery. Something still has to ask the new engine for the feed.
//
// Video is asked for: ZoomOutputManager::resubscribe_all() runs on every
// successful (re)join and re-subscribes by intent. It iterates ZoomSource, and
// the dedicated CoreVideo audio sources (src/zoom-participant-audio-source.cpp)
// are not in it. Their only re-subscribe path is maybe_resubscribe_for_roster(),
// which used to read a `subscribed` flag left over from the DEAD engine as
// "already done" and return. Net effect of the release on its own: Participant-
// and Audience-kind audio sources went quiet at the restart, released their
// mapping, and never spoke again for the rest of the session. ActiveSpeaker
// escaped only when the resolved speaker happened to change afterwards, which
// is luck, not recovery.
//
// The fix is to treat a new engine process as "subscribed to nothing", so the
// next roster tick re-subscribes. src/audio-subscription-state.h holds that
// decision; this file pins it.
//
// WHAT IS NOT COVERED, stated plainly. Nothing here constructs a
// CoreVideoAudioSource, a ZoomEngineClient or an engine — they need OBS, Qt and
// a live meeting. These tests pin the state arithmetic. They do NOT pin the
// wiring: deleting forget_subscription_for_new_engine() from the
// on_new_engine_process callback, or deleting the roster callback registration,
// would fail nothing here. Nor do they pin that the new engine publishes a
// roster at all — that is EngineParticipants::attach() in engine/src/main.cpp,
// on MEETING_STATUS_INMEETING, and proving it needs a live join.

#include "audio-subscription-state.h"

#include <iostream>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

constexpr uint32_t kAlice = 1001;
constexpr uint32_t kBob   = 1002;

// A source that is live on air and has an accepted subscription to `id`.
AudioSubscriptionState subscribed_to(uint32_t id)
{
    return AudioSubscriptionState{true, id};
}

// ── The defect, in the exact shape it reached air ───────────────────────────
// Carrying the dead engine's `subscribed` across the restart makes the next
// roster tick a no-op, for both kinds that cannot re-point their way out of it.
void test_a_stale_subscription_silences_the_next_roster_tick()
{
    const AudioSubscriptionState stale = subscribed_to(kAlice);

    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, true, stale,
                                   kAlice) == AudioResubscribeAction::None,
          "a Participant source pinned to Alice does nothing on a roster tick "
          "while it still believes it is subscribed to Alice — this is the "
          "silence, and it never ends because nothing else clears the flag");

    check(audio_resubscribe_action(CoreVideoAudioKind::Audience, true,
                                   subscribed_to(0), 0) ==
              AudioResubscribeAction::None,
          "an Audience source does nothing either: it follows no participant, "
          "so a re-point can never rescue it");

    // ActiveSpeaker only escaped by accident. Same stale state, and the SAME
    // speaker still talking, is the same dead end.
    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, true,
                                   stale, kAlice) ==
              AudioResubscribeAction::None,
          "ActiveSpeaker is not exempt — it recovers only if the speaker "
          "CHANGES, so Alice holding the floor across the restart leaves it as "
          "silent as the other two");
}

// ── The fix ────────────────────────────────────────────────────────────────
// A new engine process has been sent nothing, so that is what the plugin must
// believe about it. Every kind then subscribes on the next tick.
void test_a_new_engine_process_makes_the_next_roster_tick_resubscribe()
{
    const AudioSubscriptionState fresh = audio_state_for_new_engine_process();
    check(!fresh.subscribed,
          "a new engine process holds no subscription for this source");
    check(fresh.participant_id == 0,
          "…and no participant, so nothing is carried across the boundary");

    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, true, fresh,
                                   kAlice) == AudioResubscribeAction::Subscribe,
          "the Participant source subscribes on the new engine's first roster");
    check(audio_resubscribe_action(CoreVideoAudioKind::Audience, true, fresh,
                                   0) == AudioResubscribeAction::Subscribe,
          "the Audience source subscribes on the new engine's first roster");
    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, true,
                                   fresh, kAlice) ==
              AudioResubscribeAction::Subscribe,
          "the ActiveSpeaker source subscribes on the new engine's first "
          "roster, without waiting for the speaker to change");

    // It is a plain Subscribe, never UnsubscribeThenSubscribe. Sending an
    // unsubscribe for a subscription that died with the old process would name
    // a uuid the new engine has never heard of.
    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, true, fresh,
                                   kAlice) !=
              AudioResubscribeAction::UnsubscribeThenSubscribe,
          "and it does not try to cancel a subscription the new engine never "
          "had");
}

// ── The clearing must not become an unconditional re-subscribe ─────────────
// Everything else about the roster tick has to keep working, or "always
// subscribe" would pass the two tests above while re-subscribing thousands of
// times a show.
void test_the_ordinary_roster_tick_is_unchanged()
{
    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, true,
                                   subscribed_to(kAlice), kBob) ==
              AudioResubscribeAction::UnsubscribeThenSubscribe,
          "a re-point to a different participant still cancels first — the "
          "explicit unsubscribe is what makes the engine rebuild the audio "
          "target on a fresh region name");
    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, true,
                                   subscribed_to(kAlice), kBob) ==
              AudioResubscribeAction::UnsubscribeThenSubscribe,
          "…and so does an active-speaker cut");

    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, true,
                                   subscribed_to(kAlice), kAlice) ==
              AudioResubscribeAction::None,
          "the same speaker on a later tick is left alone, so a show's "
          "thousands of roster updates cost nothing");
    check(audio_resubscribe_action(CoreVideoAudioKind::Audience, true,
                                   subscribed_to(0), 0) ==
              AudioResubscribeAction::None,
          "a subscribed Audience source is left alone too");

    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, true,
                                   audio_state_for_new_engine_process(), 0) ==
              AudioResubscribeAction::None,
          "an unresolved target (nobody speaking yet) waits rather than "
          "subscribing to participant 0");
    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, true,
                                   subscribed_to(kAlice), 0) ==
              AudioResubscribeAction::None,
          "…and an unresolved target never tears down a working subscription");
}

// ── An inactive source stays out of it ─────────────────────────────────────
// OBS calls activate()/deactivate() as scenes change, and audio_activate()
// subscribes. A roster tick must not subscribe a source OBS is not showing,
// including right after a restart.
void test_an_inactive_source_never_subscribes_on_a_roster_tick()
{
    const AudioSubscriptionState fresh = audio_state_for_new_engine_process();
    check(audio_resubscribe_action(CoreVideoAudioKind::Participant, false,
                                   fresh, kAlice) ==
              AudioResubscribeAction::None,
          "an inactive Participant source is left to audio_activate()");
    check(audio_resubscribe_action(CoreVideoAudioKind::Audience, false, fresh,
                                   0) == AudioResubscribeAction::None,
          "an inactive Audience source is left to audio_activate()");
    check(audio_resubscribe_action(CoreVideoAudioKind::ActiveSpeaker, false,
                                   subscribed_to(kAlice), kBob) ==
              AudioResubscribeAction::None,
          "and an inactive source does not re-point either");
}

}  // namespace

int main()
{
    test_a_stale_subscription_silences_the_next_roster_tick();
    test_a_new_engine_process_makes_the_next_roster_tick_resubscribe();
    test_the_ordinary_roster_tick_is_unchanged();
    test_an_inactive_source_never_subscribes_on_a_roster_tick();

    if (g_failures == 0)
        std::cout << "audio-subscription-state: all tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
