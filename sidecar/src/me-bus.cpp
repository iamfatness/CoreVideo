#include "me-bus.h"
#include <QTimer>
#include <algorithm>

MEBus::MEBus(QObject *parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(16); // ~60 Hz
    connect(m_timer, &QTimer::timeout, this, &MEBus::onTick);
}

void MEBus::stageLook(const Look &look)
{
    m_pvw = look;
    emit previewChanged(m_pvw);
}

void MEBus::commitPgmFromPvw()
{
    Look outgoing = m_pgm;
    m_pgm = m_pvw;
    // Hardware-switcher convention: PVW retains the prior PGM so the
    // operator can take back. Don't overwrite if PGM was empty.
    m_pvw = outgoing.isValid() ? outgoing : m_pvw;
    emit programChanged(m_pgm);
    emit previewChanged(m_pvw);
    emit tookProgram(m_pgm);
}

void MEBus::take()
{
    if (isTransitioning()) return;
    // Force-clear any FTB dim so a cut always lands at full opacity.
    if (m_ftbActive) {
        m_ftbActive  = false;
        m_pgmOpacity = 1.0f;
        emit pgmOpacityChanged(m_pgmOpacity);
    }
    commitPgmFromPvw();
}

void MEBus::autoTake(int durationMs)
{
    if (isTransitioning() || !m_pvw.isValid()) return;
    m_committed = false;
    startTransition(Auto, std::max(60, durationMs));
}

void MEBus::ftbToggle(int durationMs)
{
    if (isTransitioning()) return;
    startTransition(m_ftbActive ? FTBOff : FTBOn, std::max(60, durationMs));
}

void MEBus::swap()
{
    if (isTransitioning()) return;
    std::swap(m_pgm, m_pvw);
    emit programChanged(m_pgm);
    emit previewChanged(m_pvw);
}

void MEBus::startTransition(TransitionKind k, int durationMs)
{
    m_kind        = k;
    m_durationMs  = durationMs;
    m_elapsed.restart();
    emit transitionStarted(k, durationMs);
    onTick(); // emit an immediate first sample so UI updates without a 16ms gap
    m_timer->start();
}

void MEBus::onTick()
{
    const float t = std::clamp(float(m_elapsed.elapsed()) / float(m_durationMs),
                               0.0f, 1.0f);
    emit transitionProgress(t);

    switch (m_kind) {
    case Auto: {
        // Fade-through-black: opacity goes 1 → 0 in the first half, 0 → 1
        // in the second. Commit happens at the midpoint so OBS switches
        // while the audience sees black.
        m_pgmOpacity = (t < 0.5f) ? (1.0f - 2.0f * t) : (2.0f * t - 1.0f);
        emit pgmOpacityChanged(m_pgmOpacity);
        if (!m_committed && t >= 0.5f) {
            m_committed = true;
            commitPgmFromPvw();
        }
        break;
    }
    case FTBOn: {
        m_pgmOpacity = 1.0f - t;
        emit pgmOpacityChanged(m_pgmOpacity);
        break;
    }
    case FTBOff: {
        m_pgmOpacity = t;
        emit pgmOpacityChanged(m_pgmOpacity);
        break;
    }
    case Cut:
    case None:
        break;
    }

    if (t >= 1.0f) finishTransition();
}

void MEBus::finishTransition()
{
    m_timer->stop();

    switch (m_kind) {
    case FTBOn:
        m_ftbActive  = true;
        m_pgmOpacity = 0.0f;
        emit pgmOpacityChanged(m_pgmOpacity);
        break;
    case FTBOff:
        m_ftbActive  = false;
        m_pgmOpacity = 1.0f;
        emit pgmOpacityChanged(m_pgmOpacity);
        break;
    case Auto:
        m_pgmOpacity = 1.0f;
        emit pgmOpacityChanged(m_pgmOpacity);
        break;
    default:
        break;
    }

    m_kind = None;
    emit transitionEnded();
}
