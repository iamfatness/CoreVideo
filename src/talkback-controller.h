#pragma once
//
// talkback-controller.h — the plugin's talkback owner.
//
// Holds the three things that must agree for a key to be open: the OBS tap
// (audio is flowing), the engine session (a channel exists and someone is
// invited), and the keying state (the operator still wants it). Every surface
// -- control API today, Companion and a hotkey later -- goes through here, so
// there is exactly one place that can open or close a key.
//
// The dead-man switch lives in src/talkback-key.h and is evaluated on a timer
// here. Audio arriving IS the liveness signal: while a key is open the tap
// publishes continuously (including silence, because an active OBS source
// calls back whether or not anyone is talking), so a gap means the path is
// gone and the key closes with nothing having to NOTICE the failure.
//
#include "talkback-cue.h"
#include "talkback-key.h"
#include "talkback-tap.h"

#include <QObject>
#include <QTimer>

#include <mutex>
#include <string>

class TalkbackController : public QObject {
    Q_OBJECT
public:
    static TalkbackController &instance();

    // Opens a key. Returns false with a human-readable reason -- an operator
    // whose key did not open needs to know WHICH thing failed.
    bool key_on(const std::string &participant, const std::string &source,
                TalkbackKeyMode mode, bool needs_renewal,
                std::string &error_out);
    void key_off();
    // Re-assert an open key. The lost-release backstop: a key opened over the
    // control API stays open only while the controller keeps saying it is
    // still wanted. See src/talkback-key.h.
    void renew();

    // Explicit teardown for plugin unload. Call this from shutdown_corevideo()
    // / obs_module_unload(), the same place the sibling singletons (
    // ZoomControlServer, ZoomOscServer, ZoomIsoRecorder, ZoomEngineClient) are
    // stopped -- while Qt's event loop is still guaranteed alive. Left to
    // static destruction at process exit instead, a QObject held by a
    // function-local static can be torn down after QCoreApplication is gone,
    // a known Qt hazard for anything owning a live QTimer.
    void stop();

    std::string status_json() const;

private slots:
    void evaluate();

private:
    TalkbackController();

    mutable std::mutex m_mtx;
    TalkbackTap        m_tap;
    TalkbackKeyState   m_key{};
    std::string        m_participant;
    std::string        m_source;
    QTimer            *m_timer = nullptr;
    // F2 review-round fix: monotonic ms when key_on() opened THIS key.
    // evaluate()'s grace period (see there) measures from this, not from
    // any timestamp the engine reports, so it is correct even for the
    // very first evaluate() tick after key_on() -- before any engine
    // response, confirmed or not, could possibly have arrived.
    uint64_t           m_session_opened_at_ms = 0;
    // Last engine-confirmed `live` value evaluate() has seen, so it can
    // edge-detect the open/close audio cue (talkback-cue.h) instead of
    // re-firing OPEN on every tick a session stays live. Reset to false in
    // key_on(), so a NEW session's own live confirmation is always seen as
    // a fresh false->true transition, never swallowed by a stale true left
    // over from a previous key.
    bool                m_last_live = false;
};
