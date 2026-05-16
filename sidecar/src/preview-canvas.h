#pragma once
#include "layout-template.h"
#include "overlay.h"
#include <QWidget>
#include <QVector>
#include <QColor>
#include <QPoint>

// Renders a broadcast preview panel — draws participant slots according to
// the active LayoutTemplate using mock or live thumbnail data.
// Accepts drops from participant cards (MIME: "application/x-cv-participant").
class PreviewCanvas : public QWidget {
    Q_OBJECT
public:
    struct Participant {
        QString name;
        QString initials;
        QColor  avatarColor = QColor(60, 80, 200);
        bool    isTalking   = false;
        bool    hasVideo    = false;
    };

    explicit PreviewCanvas(QWidget *parent = nullptr);

    void setTemplate(const LayoutTemplate &tmpl);
    void setParticipants(const QVector<Participant> &participants);
    void setOverlays(const QVector<Overlay> &overlays);
    // Accent color used when an overlay doesn't specify one — usually the
    // active theme accent. Defaults to a neutral blue.
    void setAccent(const QColor &c);

signals:
    void slotAssigned(int slotIndex, int participantId);
    void slotClicked(int slotIndex);

protected:
    void paintEvent(QPaintEvent *) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    LayoutTemplate       m_tmpl;
    QVector<Participant> m_parts;
    QVector<Overlay>     m_overlays;
    QColor               m_accent = QColor(0x29, 0x79, 0xff);
    int                  m_hoveredSlot = -1;
    int                  m_pressedSlot = -1;
    QPoint               m_pressPos;

    int    slotAtPoint(QPoint pt) const;
    void   drawSlot(QPainter &p, const QRectF &rect, int index) const;
    void   drawAvatar(QPainter &p, QPointF center, float r,
                      const Participant &part) const;
    QRectF slotRect(const TemplateSlot &s) const;
    void   drawOverlay(QPainter &p, const Overlay &ov, const QRectF &canvas) const;
};
