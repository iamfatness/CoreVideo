#include "talkback-controller.h"
#include "zoom-engine-client.h"

#include <obs-module.h>
#include <util/platform.h>

static uint64_t now_ms() { return os_gettime_ns() / 1000000ULL; }

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
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key OPEN to \"%s\" via \"%s\"",
         participant.c_str(), source.c_str());
    return true;
}

void TalkbackController::key_off()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (!m_key.open) return;
    m_key.open = false;
    m_tap.close();                                  // sends talkback_close
    ZoomEngineClient::instance().talkback_stop();
    blog(LOG_INFO, "[obs-zoom-plugin] talkback: key CLOSED");
}

void TalkbackController::renew()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_key.open) m_key.last_renewal_ms = now_ms();
}

void TalkbackController::evaluate()
{
    TalkbackKeyAction action = TalkbackKeyAction::None;
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
    }
    // The lock above is released before key_off() is called below. key_off()
    // takes m_mtx itself; holding it across the call would self-deadlock this
    // non-recursive mutex on the Qt main thread (the timer callback), freezing
    // the whole OBS UI. Do not fold this back into the block above.
    if (action == TalkbackKeyAction::Close) {
        blog(LOG_WARNING, "[obs-zoom-plugin] talkback: key closed by the "
                          "dead-man switch (audio stopped, engine gone, or the "
                          "meeting ended)");
        key_off();
    }
}

std::string TalkbackController::status_json() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    return std::string(R"({"open":)") + (m_key.open ? "true" : "false") +
           R"(,"participant":")" + m_participant +
           R"(","source":")" + m_source +
           R"(","tap_open":)" + (m_tap.is_open() ? "true" : "false") + "}";
}
