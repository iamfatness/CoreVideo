#pragma once
#include "look.h"
#include <QObject>

// MEBus — the broadcast switcher core. Owns Program (PGM, on-air) and
// Preview (PVW, staged next) Looks. All on-air state mutations route
// through here so the UI, OBS bridge, and remote control share one truth.
//
// Slice 1: cut-only TAKE. AUTO/FTB and transition timing come in a later
// slice, as does pushing transforms to OBS (callers wire that to
// programChanged for now).
class MEBus : public QObject {
    Q_OBJECT
public:
    explicit MEBus(QObject *parent = nullptr) : QObject(parent) {}

    const Look &program() const { return m_pgm; }
    const Look &preview() const { return m_pvw; }

    // Replace the staged Look on PVW. Pure UI/state mutation — no OBS push.
    void stageLook(const Look &look)
    {
        m_pvw = look;
        emit previewChanged(m_pvw);
    }

    // Commit PVW to PGM (cut). Hardware-switcher convention: PVW keeps a
    // copy of the previous PGM so the operator can TAKE back. A future
    // "pflip-off" preference can flip this behavior.
    void take()
    {
        Look outgoing = m_pgm;
        m_pgm = m_pvw;
        m_pvw = outgoing.isValid() ? outgoing : m_pvw;
        emit programChanged(m_pgm);
        emit previewChanged(m_pvw);
        emit tookProgram(m_pgm);
    }

    // Swap PGM ↔ PVW without committing to OBS — useful for "what would
    // this look like on air" comparisons.
    void swap()
    {
        std::swap(m_pgm, m_pvw);
        emit programChanged(m_pgm);
        emit previewChanged(m_pvw);
    }

signals:
    void programChanged(const Look &pgm);
    void previewChanged(const Look &pvw);
    // Fires only on TAKE (not on programChanged from swap), so OBS-push
    // logic can hook here without firing on every swap.
    void tookProgram(const Look &pgm);

private:
    Look m_pgm;
    Look m_pvw;
};
