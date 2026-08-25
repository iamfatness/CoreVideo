#include "talkback-controller.h"
#include "zoom-engine-client.h"

#include <obs-module.h>
#include <util/platform.h>

// Built with Qt's JSON types rather than hand-rolled concatenation, matching
// the pattern zoom-diagnostics-dialog.cpp already uses for its status
// payloads: m_participant and m_source are a Zoom display name and an OBS
// source name, both operator/user-controlled text that can contain '"' or
// '\\'. Task 5 parses this string back into a QJsonObject, so an unescaped
// quote here is a parse failure in a different file, not a cosmetic glitch.
#include <QJsonDocument>
#include <QJsonObject>

static uint64_t now_ms() { return os_gettime_ns() / 1000000ULL; }

// F2 review-round fix: how long evaluate() waits for the engine's own
// {"cmd":"talkback_session","live":...} confirmation before treating
// "not yet live" as a failure. The engine's path from session_start() to
// live=true is one Zoom CreateChannel round-trip plus a couple of
// synchronous SDK calls (BeginBatchInviteUsers/AddUserToInvite/
// ExecuteBatchInviteUsers) -- normally well under a second, but the spec
// only commits to "a moment... the first second or so", and this value
// gives that one network round-trip real room without also leaving a
// genuinely dead key open for long: kTalkbackAudioGapMs (250ms) is what
// closes a key whose audio path is dead, so 1500ms here bounds the OTHER
// failure mode -- a key that never had a channel at all -- to firmly
// inside "the operator notices, not the audience".
constexpr uint64_t kTalkbackSessionGraceMs = 1500;

TalkbackController &TalkbackController::instance()
{
    static TalkbackController c;
    return c;
}

TalkbackController::TalkbackController()
{
    m_timer = new QTimer(this);
    // A tenth of the audio-gap window, so a dead path is noticed within a
    // couple of ticks rather than a couple of gaps.
    m_timer->setInterval(static_cast<int>(kTalkbackAudioGapMs / 10));
    connect(m_timer, &QTimer::timeout, this, &TalkbackController::evaluate);
    m_timer->start();
}

bool TalkbackController::key_on(const std::string &participant,
                                const std::string &source,
                                TalkbackKeyMode mode, bool needs_renewal,
                                std::string &error_out)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_key.open) { error_out = "A talkback key is already open"; return false; }
    if (participant.empty()) { error_out = "No participant named"; return false; }
    if (source.empty())      { error_out = "No OBS audio source chosen"; return false; }

    // Check the precondition BEFORE starting anything. talkback_start() is
    // fire-and-forget and a silent no-op when the engine isn't running or
    // isn't in a meeting -- m_tap.open() only needs the local OBS source, so
    // without this check key_on() would return true, the tap would start
    // publishing real audio into the ring, and the next evaluate() tick would
    // force-close it via the structural check below. That is a visible
    // open-then-retract flicker in exactly the case an operator hits most
    // often: keying before the meeting is joined. Fail closed means refusing
    // to open here, not opening and then retracting a tick later.
    if (!ZoomEngineClient::instance().is_running()) {
        error_out = "the Zoom engine is not running";
        return false;
    }
    if (ZoomEngineClient::instance().state() != MeetingState::InMeeting) {
        error_out = "not in a meeting";
        return false;
    }

    // Order matters: the engine must have a channel before audio arrives, and
    // the tap must be laid out before the engine maps its region. Start the
    // session first, then open the tap (which sends talkback_open itself).
    ZoomEngineClient::instance().talkback_start(participant);
    if (!m_tap.open(source, error_out)) {
        // The tap failed after the engine already stood up a channel with
        // nothing to feed it -- stop the session rather than leave a live
        // channel with no audio source behind it.
        ZoomEngineClient::instance().talkback_stop();
        return false;
    }

    m_participant = participant;
    m_source      = source;
    m_key.open            = true;
    m_key.mode            = mode;
    m_key.needs_renewal   = needs_renewal;
    m_key.last_audio_ms   = now_ms();
    m_key.last_renewal_ms = now_ms();
    // F2 review-round fix: anchor for evaluate()'s grace period. Set here,
    // under the same lock as m_key.open, so evaluate() can never observe
    // m_key.open == true with a stale value left over from a previous key.
    m_session_opened_at_ms = now_ms();
    // This session hasn't been confirmed live yet -- reset here (not just
    // relying on key_off()'s own reset) so key_on() is the single place a
    // fresh session always starts "not live", regardless of how the
    // previous key ended. See m_last_live's declaration.
    m_last_live = false;
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key OPEN to \"%s\" via \"%s\"",
         participant.c_str(), source.c_str());
    return true;
}

void TalkbackController::key_off()
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_key.open) return;
        // Flip the flag while still holding the lock so a second concurrent
        // key_off() (e.g. the dead-man switch racing an operator release)
        // sees m_key.open already false and returns above instead of running
        // the close path twice.
        m_key.open = false;
        // Reset so the NEXT key_on() starts from "not live" -- see
        // m_last_live's declaration for why a stale true here would swallow
        // that session's own OPEN cue.
        m_last_live = false;
    }
    // The CLOSE cue fires on every key_off() that actually closed something
    // -- unconditionally, not only when evaluate()'s live-edge detection
    // (talkback-cue.h) caught a true->false transition. A key that never
    // got a confirmed channel (closed by the session-state grace period, or
    // by the operator releasing before the engine ever answered) still
    // needs an audible CLOSE: the operator must never be left believing
    // they're still keyed. Requested outside the lock -- talkback_play_cue()
    // only spawns a thread and returns, so this costs nothing measurable,
    // but there's no reason to do it while holding m_mtx either.
    talkback_play_cue(TalkbackCue::Close);
    // m_tap.close() and talkback_stop() run OUTSIDE m_mtx, mirroring
    // TalkbackTap::close()'s own discipline one layer up: close() synchronises
    // with libobs's audio mutex against an in-flight on_audio() callback,
    // which can briefly block. Holding m_mtx across that join would stall
    // every other caller -- status_json(), renew(), key_on(), a concurrent
    // key_off() -- for the duration, on every routine dead-man close, not
    // just the rare ones.
    m_tap.close();                                  // sends talkback_close
    ZoomEngineClient::instance().talkback_stop();
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key CLOSED");
}

void TalkbackController::stop()
{
    // Called from shutdown_corevideo() while Qt's event loop is still
    // guaranteed alive -- see the header comment. Stop the timer first so no
    // further evaluate() tick can race this teardown, then fail closed: an
    // open key at unload still needs its OBS-side state (capture callback,
    // source ref, SHM region) released, not just the Zoom channel severed.
    if (m_timer) m_timer->stop();
    key_off();
    // AFTER key_off(), not before: key_off() may have just queued a final
    // CLOSE cue (talkback-cue.h), and talkback_cue_shutdown() is what
    // guarantees that cue actually gets to play before this plugin's DLL
    // can be unloaded -- see its doc comment for the crash this closes.
    // stop() is a shutdown path (not evaluate()'s Qt-timer path), so the
    // bounded block inside talkback_cue_shutdown() is acceptable here.
    talkback_cue_shutdown();
}

void TalkbackController::renew()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_key.open) m_key.last_renewal_ms = now_ms();
}

void TalkbackController::evaluate()
{
    TalkbackKeyAction action = TalkbackKeyAction::None;
    bool closed_by_session_state = false;
    // Which open/close cue (if any) this tick decided to play -- computed
    // under the lock below, PLAYED after it's released. talkback_play_cue()
    // doesn't block (see talkback-cue.cpp), but there's no reason to do OS
    // work while holding m_mtx, and it keeps this function's locking
    // discipline uniform with the action/key_off() split just below.
    TalkbackCue cue = TalkbackCue::None;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_key.open) return;
        // The dead-man switch reads the tap's own last-publish stamp, not a
        // value this class maintains: the tap is the thing that knows whether
        // audio is actually still flowing.
        m_key.last_audio_ms = m_tap.last_audio_ms();
        action = talkback_key_evaluate(
            m_key, now_ms(),
            ZoomEngineClient::instance().is_running(),
            ZoomEngineClient::instance().state() == MeetingState::InMeeting);

        // F2 review-round fix (CRITICAL): none of the checks above -- nor
        // talkback_key_evaluate() itself -- know whether the engine's Zoom
        // channel ever actually confirmed live. They only see local intent
        // (m_key.open) and the tap's own liveness (last_audio_ms), which
        // stays fresh even when create/invite failed outright, because the
        // tap keeps publishing into the ring regardless of whether anything
        // on the far end can hear it. Without this, "the tally reflects the
        // engine's confirmed state, never the plugin's intent" (the spec's
        // own requirement) does not hold: a key that failed to open stayed
        // shown as live and the operator's audio went nowhere.
        //
        // Close on the engine's own confirmed state too, once given a fair
        // chance to answer (kTalkbackSessionGraceMs -- see its comment):
        // either an EXPLICIT failure (reason non-empty while not live), or
        // still not live once the grace period has elapsed. A non-empty
        // reason with live == false is unambiguous -- talkback_start()
        // resets the engine-side status to (false, "") at the moment THIS
        // session was requested (see ZoomEngineClient::talkback_start()),
        // so a non-empty reason here can only be a report for this session,
        // never a stale one from a key that was already closed.
        if (action == TalkbackKeyAction::None) {
            const auto session = ZoomEngineClient::instance().talkback_session_status();

            // Edge-detect the engine's own confirmed-live state for the
            // open/close audio cue (talkback-cue.h). Deliberately reads the
            // SAME session_status() call the close-by-session-state check
            // just below already makes, rather than a second query: the
            // cue fires on the engine's confirmation, never at key_on()
            // (see talkback-cue.h's header comment) -- clipping the
            // director's first words underneath a cue that falsely claims
            // it's already safe to talk is the exact bug this feature
            // exists to fix, not reintroduce a moment later. Only computed
            // in the action==None branch: if a dead-man/structural close is
            // ALREADY happening this tick, key_off() below plays its own
            // CLOSE cue unconditionally, so there is nothing for this
            // edge-detector to usefully add here.
            cue = talkback_cue_on_live_change(m_last_live, session.live);
            m_last_live = session.live;

            if (!session.live) {
                const bool explicit_failure = !session.reason.empty();
                const bool grace_expired = talkback_elapsed_ms(
                    now_ms(), m_session_opened_at_ms) > kTalkbackSessionGraceMs;
                if (explicit_failure || grace_expired) {
                    action = TalkbackKeyAction::Close;
                    closed_by_session_state = true;
                }
            }
        }
    }
    // Played after the lock is released -- talkback_play_cue() doesn't block
    // (see talkback-cue.cpp), but nothing above needs it done under m_mtx.
    // If action ALSO turns out to be Close below (the live->false edge and
    // the session-state close it triggers can land in the same tick),
    // key_off() plays its own CLOSE cue too; both requests are CLOSE and
    // talkback-cue.cpp's REPLACE behaviour makes that harmless -- see its
    // header comment.
    if (cue != TalkbackCue::None) talkback_play_cue(cue);
    // The lock above is released before key_off() is called below. key_off()
    // takes m_mtx itself; holding it across the call would self-deadlock this
    // non-recursive mutex on the Qt main thread (the timer callback), freezing
    // the whole OBS UI. Do not fold this back into the block above.
    if (action == TalkbackKeyAction::Close) {
        if (closed_by_session_state) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] talkback: key closed -- the engine never "
                 "confirmed the Zoom channel opened (see the preceding "
                 "talkback_session log line for the reason)");
        } else {
            blog(LOG_WARNING, "[obs-zoom-plugin] talkback: key closed by the "
                              "dead-man switch (audio stopped, engine gone, or the "
                              "meeting ended)");
        }
        key_off();
    }
}

std::string TalkbackController::status_json() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    QJsonObject obj;
    obj["open"]        = m_key.open;      // local intent: the operator/API asked for this
    obj["participant"] = QString::fromStdString(m_participant);
    obj["source"]      = QString::fromStdString(m_source);
    obj["tap_open"]    = m_tap.is_open(); // local: the OBS capture path is attached
    // F2 review-round fix: the engine's own CONFIRMED state, clearly
    // distinguished from "open"/"tap_open" above -- both of those can be
    // true while the Zoom channel never opened at all (create/invite
    // failure, or the audio path being rejected after the channel WAS
    // live). See ZoomEngineClient::TalkbackSessionStatus's doc comment.
    const auto session = ZoomEngineClient::instance().talkback_session_status();
    obj["engine_live"]   = session.live;
    obj["engine_reason"] = QString::fromStdString(session.reason);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}
