#pragma once
#include "layout-template.h"
#include <QWidget>
#include <QVector>
#include <QColor>

// Renders a broadcast preview panel — draws participant slots according to
// the active LayoutTemplate using mock or live thumbnail data.
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

protected:
    void paintEvent(QPaintEvent *) override;

private:
    LayoutTemplate       m_tmpl;
    QVector<Participant> m_parts;

    void drawSlot(QPainter &p, const QRectF &rect, int index) const;
    void drawAvatar(QPainter &p, QPointF center, float r,
                    const Participant &part) const;
    QRectF slotRect(const TemplateSlot &s) const;
};
