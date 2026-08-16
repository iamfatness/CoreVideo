#pragma once

// What the plugin believes about a dedicated CoreVideo audio source's
// engine-side subscription, and the two decisions that belief drives.
//
// Extracted from src/zoom-participant-audio-source.cpp so it can be tested
// without OBS, Qt or a live engine. The source still owns the state (as
// atomics, because it is read from the OBS UI thread, the engine reader thread
// and the thread that calls ZoomEngineClient::start()); this header owns the
// arithmetic.
//
// THE DEFECT THIS EXISTS FOR. `subscribed` is a claim about a conversation with
// one ZoomObsEngine process. When that process is replaced, the claim is stale:
// the new engine has never been told about this source. Nothing used to clear
// it — unsubscribe_audio() is only reached from audio_deactivate() and
// audio_destroy(), and ZoomOutputManager::resubscribe_all(), which the reconnect
// path calls, iterates ZoomSource only and has never seen these sources. So the
// restart released the source's shared-memory mapping (commit 9026010) and then
// maybe_resubscribe_for_roster() read the stale `subscribed` as "already done"
// and returned, leaving Participant- and Audience-kind sources silent for the
// rest of the session. ActiveSpeaker-kind escaped only by accident, and only
// when the resolved speaker happened to CHANGE afterwards.
//
// The lesson is the one ZoomOutputManager::resubscribe_all() already records in
// its own comment after the 2026-08-09 incident: after an engine restart,
// re-subscribe by INTENT, never by a flag that outlived the process it
// described. This header is the audio-source half of that rule.

#include <cstdint>

// Which participant a dedicated CoreVideo audio source follows. Participant is
// a fixed id chosen by the operator, ActiveSpeaker resolves through the speaker
// director on every roster tick, and Audience is the whole-meeting mix and has
// no participant id at all.
enum class CoreVideoAudioKind {
    Participant,
    ActiveSpeaker,
    Audience,
};

struct AudioSubscriptionState {
    // True once a subscribe was accepted for delivery to the engine currently
    // serving us. Never true across an engine process boundary — see
    // audio_state_for_new_engine_process().
    bool subscribed = false;
    // The participant the accepted subscribe named. Always 0 for Audience,
    // which subscribes to the mix rather than to a person.
    uint32_t participant_id = 0;
};

// The state to adopt when a NEW ZoomObsEngine process comes up.
//
// A default-constructed state, i.e. "we are subscribed to nothing" — which is
// the literal truth about a process that has just been launched and has been
// sent no commands at all.
//
// Deliberately not an unsubscribe. The subscription died with the old process;
// there is nothing to cancel, and the callback this runs from
// (SourceCallbacks::on_new_engine_process) must not talk to the engine.
inline AudioSubscriptionState audio_state_for_new_engine_process()
{
    return AudioSubscriptionState{};
}

enum class AudioResubscribeAction {
    // Leave the subscription alone.
    None,
    // Send a subscribe. Nothing is subscribed, so nothing has to be cancelled.
    Subscribe,
    // Cancel the subscription we hold and place a new one. Used only when the
    // source has re-pointed at a different participant: the explicit
    // unsubscribe is what makes the engine destroy and rebuild the audio
    // target, which is what lets the region move to a fresh generation name.
    UnsubscribeThenSubscribe,
    // Cancel the subscription we hold and place nothing in its place. Used when
    // the participant we follow has left the meeting: the region they occupied
    // is one of kMaxShmSources (engine-ipc.h), and a long show with roster
    // churn will exhaust that budget if departures never release. The OBS
    // source stays exactly where the operator put it, silent, and re-subscribes
    // through the ordinary Subscribe path if the participant returns.
    Unsubscribe,
};

// Decides what a roster tick should do to this source's subscription.
//
// `target` is the participant the source resolves to right now (0 when nothing
// resolves, and always 0 for Audience — the caller does not even ask, because
// resolving an ActiveSpeaker target also ticks the SpeakerDirector).
//
// The rules, in the order they are applied:
//   * An inactive source subscribes to nothing. audio_activate() subscribes it
//     when OBS shows it, so a roster tick must not do that work early.
//   * Audience follows no participant, so the only question is whether it is
//     subscribed at all.
//   * A subscription whose participant has left the roster is released. The
//     region is one of kMaxShmSources; departures that never release exhaust
//     the budget over a long show.
//   * A target of 0 means nobody has been resolved yet (no active speaker, or
//     the operator has not picked a participant). Leave whatever we have; the
//     next roster tick asks again.
//   * Otherwise subscribe if we hold nothing, and re-point if we hold a
//     subscription to somebody else.
//
// `held_participant_present` says whether the participant this source currently
// holds a subscription to is still in the roster. It is meaningless when we
// hold nothing, and callers pass true in that case.
inline AudioResubscribeAction audio_resubscribe_action(
    CoreVideoAudioKind kind, bool active, const AudioSubscriptionState &state,
    uint32_t target, bool held_participant_present)
{
    if (!active) return AudioResubscribeAction::None;

    if (kind == CoreVideoAudioKind::Audience) {
        return state.subscribed ? AudioResubscribeAction::None
                                : AudioResubscribeAction::Subscribe;
    }

    // A subscription whose participant has left the roster is released, not
    // held. This is deliberately keyed on the presence of the participant we
    // HOLD, not on `target == 0`: an ActiveSpeaker source resolves to 0 in
    // every gap between speakers, and releasing on that would destroy and
    // rebuild the shared-memory region every time the room goes quiet.
    if (state.subscribed && !held_participant_present)
        return AudioResubscribeAction::Unsubscribe;

    if (target == 0) return AudioResubscribeAction::None;
    if (!state.subscribed) return AudioResubscribeAction::Subscribe;
    if (target != state.participant_id)
        return AudioResubscribeAction::UnsubscribeThenSubscribe;
    return AudioResubscribeAction::None;
}
