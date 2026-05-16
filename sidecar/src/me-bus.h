#pragma once
#include "look.h"
#include <QObject>
#include <QElapsedTimer>

class QTimer;

// MEBus — the broadcast switcher core. Owns Program (PGM, on-air) and
// Preview (PVW, staged next) Looks. All on-air state mutations route
// through here so the UI, OBS bridge, and remote control share one truth.
class MEBus : public QObject {
    Q_OBJECT
public:
    enum TransitionKind { None, Cut, Auto, FTBOn, FTBOff };
    Q_ENUM(TransitionKind)

    explicit MEBus(QObject *parent = nullptr);

    const Look &program() const { return m_pgm; }
    const Look &preview() const { return m_pvw; }
    float       pgmOpacity()      const { return m_pgmOpacity; }
    bool        isTransitioning() const { return m_kind != None; }
    bool        isFTBActive()     const { return m_ftbActive; }
    TransitionKind kind()         const { return m_kind; }

    // Stage a Look on PVW. Pure UI/state mutation — no OBS push.
    void stageLook(const Look &look);

    // Instant TAKE (cut). Commits PVW → PGM and fires tookProgram so
    // callers can push to OBS.
    void take();

    // Timed TAKE: fade PGM through black, commit at the midpoint (which
    // is also when tookProgram fires so OBS receives the new look while
    // the audience sees black), then fade back in.
    void autoTake(int durationMs = 1500);

    // Fade-to-black. If FTB is inactive, fade PGM out to black. If
    // active, fade back from black. Cleanest "kill the program" gesture.
    void ftbToggle(int durationMs = 500);

    // Swap PGM ↔ PVW without committing to OBS — useful for "what would
    // this look like on air" comparisons.
    void swap();

signals:
    void programChanged(const Look &pgm);
    void previewChanged(const Look &pvw);
    // Fires when PGM is committed (cut TAKE, or AUTO midpoint). OBS-push
    // logic should hook here.
    void tookProgram(const Look &pgm);
    // Drives canvas dimming during AUTO / FTB. 1.0 = fully visible,
    // 0.0 = black.
    void pgmOpacityChanged(float opacity);
    void transitionStarted(MEBus::TransitionKind k, int durationMs);
    void transitionProgress(float t);   // 0..1
    void transitionEnded();

private slots:
    void onTick();

private:
    Look           m_pgm;
    Look           m_pvw;
    TransitionKind m_kind        = None;
    bool           m_committed   = false;   // AUTO midpoint commit flag
    bool           m_ftbActive   = false;
    float          m_pgmOpacity  = 1.0f;
    int            m_durationMs  = 0;
    QElapsedTimer  m_elapsed;
    QTimer        *m_timer       = nullptr;

    void startTransition(TransitionKind k, int durationMs);
    void finishTransition();
    void commitPgmFromPvw();             // shared by Cut + AUTO midpoint
};
