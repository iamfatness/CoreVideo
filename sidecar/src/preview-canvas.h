#pragma once
#include "layout-template.h"
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

signals:
    void slotAssigned(int slotIndex, int participantId);

protected:
    void paintEvent(QPaintEvent *) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    LayoutTemplate       m_tmpl;
    QVector<Participant> m_parts;
    int                  m_hoveredSlot = -1;

    int    slotAtPoint(QPoint pt) const;
    void   drawSlot(QPainter &p, const QRectF &rect, int index) const;
    void   drawAvatar(QPainter &p, QPointF center, float r,
                      const Participant &part) const;
    QRectF slotRect(const TemplateSlot &s) const;
};
