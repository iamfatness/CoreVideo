#pragma once
#include "overlay.h"
#include <QWidget>
#include <QVector>

class QVBoxLayout;

// Right-panel page: fire / clear broadcast overlays on PVW.
// Preset cards add the overlay to the staged Look; the active list lets
// the operator remove individual overlays or clear them all.
class OverlayPanel : public QWidget {
    Q_OBJECT
public:
    explicit OverlayPanel(QWidget *parent = nullptr);

    // Replace the "active on PVW" rendering with the supplied list.
    void setActiveOverlays(const QVector<Overlay> &overlays);

signals:
    // Operator clicked a preset card. MainWindow adds it to m_working and
    // re-stages on PVW.
    void overlayRequested(const Overlay &overlay);
    // Remove a specific overlay (by index into the active list).
    void removeOverlayRequested(int activeIndex);
    void clearOverlaysRequested();

private:
    QVBoxLayout *m_activeLayout = nullptr;
    QVector<QWidget *> m_activeRows;

    void buildPresets(QVBoxLayout *outer);
    void rebuildActiveList(const QVector<Overlay> &overlays);
};
