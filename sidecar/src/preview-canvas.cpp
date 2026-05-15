#include "preview-canvas.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <cmath>

static const QColor kSlotBg   {0x22, 0x22, 0x30};
static const QColor kSlotBdr  {0x32, 0x32, 0x48};
static const QColor kNameBg   {0x00, 0x00, 0x00, 160};
static const float  kGap = 4.0f;

PreviewCanvas::PreviewCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(240, 135);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PreviewCanvas::setTemplate(const LayoutTemplate &tmpl)
{
    m_tmpl = tmpl;
    update();
}

void PreviewCanvas::setParticipants(const QVector<Participant> &participants)
{
    m_parts = participants;
    update();
}

QRectF PreviewCanvas::slotRect(const TemplateSlot &s) const
{
    const float W = width()  - kGap;
    const float H = height() - kGap;
    return {
        kGap / 2 + s.x * W,
        kGap / 2 + s.y * H,
        s.width  * W - kGap,
        s.height * H - kGap
    };
}

void PreviewCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x0d, 0x0d, 0x12));

    if (!m_tmpl.isValid()) {
        p.setPen(QColor(0x40, 0x40, 0x60));
        p.drawText(rect(), Qt::AlignCenter, "No template selected");
        return;
    }

    for (int i = 0; i < m_tmpl.slots.size(); ++i)
        drawSlot(p, slotRect(m_tmpl.slots[i]), i);
}

void PreviewCanvas::drawSlot(QPainter &p, const QRectF &rect, int idx) const
{
    const bool hasPart = (idx < m_parts.size());
    const Participant &part = hasPart ? m_parts[idx] : Participant{};

    // Slot background
    QPainterPath path;
    path.addRoundedRect(rect, 10, 10);
    p.fillPath(path, kSlotBg);

    // Talking border
    if (hasPart && part.isTalking) {
        p.setPen(QPen(QColor(0x20, 0x90, 0xff), 2.5));
        p.drawPath(path);
    } else {
        p.setPen(QPen(kSlotBdr, 1));
        p.drawPath(path);
    }

    // Avatar
    const float avatarR = std::min(rect.width(), rect.height()) * 0.28f;
    const QPointF center{rect.center().x(), rect.center().y() - avatarR * 0.2f};
    drawAvatar(p, center, avatarR, part);

    // Name + resize strip at bottom
    if (hasPart && !part.name.isEmpty()) {
        const float stripH = 34;
        const QRectF strip{rect.left(), rect.bottom() - stripH,
                           rect.width(), stripH};
        QPainterPath bg;
        bg.addRoundedRect(strip, 0, 0);
        bg.addRoundedRect(strip.adjusted(0, 0, 0, -strip.height() / 2), 0, 0);
        // Simple gradient-like dark bottom
        QLinearGradient grad(strip.topLeft(), strip.bottomLeft());
        grad.setColorAt(0, QColor(0, 0, 0, 0));
        grad.setColorAt(1, QColor(0, 0, 0, 180));
        p.fillRect(strip, grad);

        p.setPen(Qt::white);
        QFont nameFont = p.font();
        nameFont.setPointSizeF(10);
        nameFont.setWeight(QFont::DemiBold);
        p.setFont(nameFont);
        p.drawText(strip.adjusted(8, 4, -8, -14), Qt::AlignLeft | Qt::AlignTop,
                   part.name);

        // "Resize" hint
        QFont smallFont = nameFont;
        smallFont.setPointSizeF(8);
        smallFont.setWeight(QFont::Normal);
        p.setFont(smallFont);
        p.setPen(QColor(0x80, 0x80, 0xa8));
        p.drawText(strip.adjusted(8, 0, -8, -4), Qt::AlignLeft | Qt::AlignBottom,
                   "Resize  ⤢");
    }
}

void PreviewCanvas::drawAvatar(QPainter &p, QPointF center,
                               float r, const Participant &part) const
{
    // Talking ring
    if (part.isTalking) {
        p.setPen(Qt::NoPen);
        QRadialGradient glow(center, r + 6);
        glow.setColorAt(0, QColor(0x20, 0x90, 0xff, 80));
        glow.setColorAt(1, Qt::transparent);
        p.setBrush(glow);
        p.drawEllipse(center, r + 6, r + 6);
    }

    // Avatar circle fill
    p.setPen(Qt::NoPen);
    p.setBrush(part.avatarColor);
    p.drawEllipse(center, r, r);

    // White ring around avatar
    p.setPen(QPen(QColor(255, 255, 255, 60), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, r, r);

    // Initials text
    if (!part.initials.isEmpty()) {
        QFont f = p.font();
        f.setPointSizeF(r * 0.55f);
        f.setWeight(QFont::Bold);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(center.x() - r, center.y() - r, 2 * r, 2 * r),
                   Qt::AlignCenter, part.initials);
    }
}
