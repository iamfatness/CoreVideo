#include "preview-canvas.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QApplication>
#include <algorithm>
#include <cmath>

static const QColor kSlotBg   {0x22, 0x22, 0x30};
static const QColor kSlotBdr  {0x32, 0x32, 0x48};
static const QColor kNameBg   {0x00, 0x00, 0x00, 160};
static const float  kGap = 4.0f;
static const double kProgramAspect = 16.0 / 9.0;

PreviewCanvas::PreviewCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(240, 135);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(true);
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

void PreviewCanvas::setOverlays(const QVector<Overlay> &overlays)
{
    m_overlays = overlays;
    update();
}

void PreviewCanvas::setAccent(const QColor &c)
{
    if (c.isValid()) {
        m_accent = c;
        update();
    }
}

void PreviewCanvas::setOpacity(float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (qFuzzyCompare(clamped, m_opacity)) return;
    m_opacity = clamped;
    update();
}

// ── Drag-and-drop ─────────────────────────────────────────────────────────────

int PreviewCanvas::slotAtPoint(QPoint pt) const
{
    for (int i = 0; i < m_tmpl.slotList.size(); ++i) {
        if (slotRect(m_tmpl.slotList[i]).contains(pt))
            return i;
    }
    return -1;
}

void PreviewCanvas::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat("application/x-cv-participant"))
        e->acceptProposedAction();
}

void PreviewCanvas::dragMoveEvent(QDragMoveEvent *e)
{
    if (!e->mimeData()->hasFormat("application/x-cv-participant")) return;
    const int slot = slotAtPoint(e->position().toPoint());
    if (slot != m_hoveredSlot) {
        m_hoveredSlot = slot;
        update();
    }
    (slot >= 0) ? e->acceptProposedAction() : e->ignore();
}

void PreviewCanvas::dragLeaveEvent(QDragLeaveEvent *)
{
    m_hoveredSlot = -1;
    update();
}

void PreviewCanvas::dropEvent(QDropEvent *e)
{
    if (!e->mimeData()->hasFormat("application/x-cv-participant")) return;
    const int slot = slotAtPoint(e->position().toPoint());
    m_hoveredSlot = -1;
    update();
    if (slot < 0) { e->ignore(); return; }
    const int pid = QString::fromUtf8(
        e->mimeData()->data("application/x-cv-participant")).toInt();
    emit slotAssigned(slot, pid);
    e->acceptProposedAction();
}

void PreviewCanvas::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    m_pressedSlot = slotAtPoint(e->pos());
    m_pressPos    = e->pos();
    QWidget::mousePressEvent(e);
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_pressedSlot >= 0) {
        const int slotNow = slotAtPoint(e->pos());
        const int dist    = (e->pos() - m_pressPos).manhattanLength();
        // Treat as a click only if release lands on the same slot and the
        // pointer barely moved — anything larger is a drag/resize gesture.
        if (slotNow == m_pressedSlot && dist < QApplication::startDragDistance())
            emit slotClicked(m_pressedSlot);
    }
    m_pressedSlot = -1;
    QWidget::mouseReleaseEvent(e);
}

QRectF PreviewCanvas::canvasRect() const
{
    const QRectF outer = rect();
    if (outer.isEmpty()) return outer;

    double canvasW = outer.width();
    double canvasH = canvasW / kProgramAspect;
    if (canvasH > outer.height()) {
        canvasH = outer.height();
        canvasW = canvasH * kProgramAspect;
    }

    return QRectF(outer.left() + (outer.width() - canvasW) * 0.5,
                  outer.top() + (outer.height() - canvasH) * 0.5,
                  canvasW,
                  canvasH);
}

QRectF PreviewCanvas::slotRect(const TemplateSlot &s) const
{
    const QRectF canvas = canvasRect();
    const float W = canvas.width()  - kGap;
    const float H = canvas.height() - kGap;
    return {
        canvas.left() + kGap / 2 + s.x * W,
        canvas.top()  + kGap / 2 + s.y * H,
        s.width  * W - kGap,
        s.height * H - kGap
    };
}

void PreviewCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x0d, 0x0d, 0x12));
    const QRectF canvas = canvasRect();
    p.fillRect(canvas, QColor(0x10, 0x10, 0x18));
    p.setPen(QPen(QColor(0x2a, 0x2a, 0x3a), 1));
    p.drawRect(canvas.adjusted(0.5, 0.5, -0.5, -0.5));

    if (!m_tmpl.isValid()) {
        p.setPen(QColor(0x40, 0x40, 0x60));
        p.drawText(canvas, Qt::AlignCenter, "No template selected");
        return;
    }

    for (int i = 0; i < m_tmpl.slotList.size(); ++i)
        drawSlot(p, slotRect(m_tmpl.slotList[i]), i);

    // Overlays sit on top of the 16:9 program canvas. Overlay coords are
    // normalized 0..1 of the program output area.
    for (const auto &ov : m_overlays)
        drawOverlay(p, ov, canvas);

    // Dim layer for AUTO / FTB. Painted last so it sits above everything.
    if (m_opacity < 1.0f) {
        const int alpha = int(std::clamp((1.0f - m_opacity) * 255.0f,
                                         0.0f, 255.0f));
        p.fillRect(canvas, QColor(0, 0, 0, alpha));
    }
}

void PreviewCanvas::drawOverlay(QPainter &p, const Overlay &ov,
                                const QRectF &canvas) const
{
    const QColor accent = ov.accent.isValid() ? ov.accent : m_accent;
    const QRectF r(canvas.left() + ov.x * canvas.width(),
                   canvas.top()  + ov.y * canvas.height(),
                   ov.w * canvas.width(),
                   ov.h * canvas.height());
    if (r.width() < 2 || r.height() < 2) return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    switch (ov.type) {
    case Overlay::LowerThird: {
        // Two-band bar: accent stripe on the left, dark body on the right.
        const float radius = std::min(6.0, r.height() * 0.25);
        const float stripeW = std::min(8.0, r.width() * 0.04);

        QPainterPath body;
        body.addRoundedRect(r, radius, radius);
        p.fillPath(body, QColor(0x0a, 0x0a, 0x12, 230));
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawPath(body);

        // Accent stripe
        p.fillRect(QRectF(r.left() + 2, r.top() + 2,
                          stripeW, r.height() - 4), accent);

        const QRectF text = r.adjusted(stripeW + 12, 6, -10, -6);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSizeF(std::max(9.0, r.height() * 0.32));
        f.setWeight(QFont::Black);
        p.setFont(f);
        p.drawText(QRectF(text.left(), text.top(),
                          text.width(), text.height() * 0.6),
                   Qt::AlignLeft | Qt::AlignVCenter, ov.text1);

        if (!ov.text2.isEmpty()) {
            f.setPointSizeF(std::max(7.5, r.height() * 0.22));
            f.setWeight(QFont::Normal);
            p.setFont(f);
            p.setPen(QColor(0xc0, 0xc0, 0xe0));
            p.drawText(QRectF(text.left(), text.top() + text.height() * 0.55,
                              text.width(), text.height() * 0.45),
                       Qt::AlignLeft | Qt::AlignVCenter, ov.text2);
        }
        break;
    }

    case Overlay::Bug: {
        // Pill-shaped bug with accent fill.
        QPainterPath pill;
        const float radius = r.height() * 0.5;
        pill.addRoundedRect(r, radius, radius);
        p.fillPath(pill, accent);
        p.setPen(QPen(QColor(0, 0, 0, 80), 1));
        p.drawPath(pill);

        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSizeF(std::max(8.0, r.height() * 0.5));
        f.setWeight(QFont::Black);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, ov.text1);
        break;
    }

    case Overlay::Ticker: {
        // Full-width band: accent label on the left, scrolling text body.
        p.fillRect(r, QColor(0x08, 0x08, 0x12, 235));

        QFont f = p.font();
        f.setPointSizeF(std::max(8.0, r.height() * 0.55));
        f.setWeight(QFont::Black);
        p.setFont(f);

        // Static "LIVE"-style chip on the left (uses text up to em-dash)
        QString chip = ov.text1;
        QString body = QString();
        const int sep = ov.text1.indexOf(" — ");
        if (sep > 0) {
            chip = ov.text1.left(sep);
            body = ov.text1.mid(sep + 3);
        }

        const QFontMetrics fm(f);
        const int chipW = fm.horizontalAdvance(chip) + 16;
        const QRectF chipR(r.left(), r.top(), chipW, r.height());
        p.fillRect(chipR, accent);
        p.setPen(Qt::white);
        p.drawText(chipR, Qt::AlignCenter, chip);

        if (!body.isEmpty()) {
            p.setPen(QColor(0xe0, 0xe0, 0xf0));
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            p.drawText(QRectF(chipR.right() + 12, r.top(),
                              r.width() - chipR.width() - 16, r.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, body);
        }
        break;
    }

    case Overlay::TitleCard: {
        // Bold accent block with stacked text.
        p.fillRect(r, QColor(0x06, 0x06, 0x0c, 240));
        // Accent ribbon on the left third
        const QRectF ribbon(r.left(), r.top(), r.width() * 0.20, r.height());
        p.fillRect(ribbon, accent);

        const QRectF text = r.adjusted(ribbon.width() + 16, 8, -16, -8);

        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSizeF(std::max(12.0, r.height() * 0.42));
        f.setWeight(QFont::Black);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        p.setFont(f);
        p.drawText(QRectF(text.left(), text.top(),
                          text.width(), text.height() * 0.58),
                   Qt::AlignLeft | Qt::AlignVCenter, ov.text1.toUpper());

        if (!ov.text2.isEmpty()) {
            f.setPointSizeF(std::max(8.5, r.height() * 0.22));
            f.setWeight(QFont::Normal);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
            p.setFont(f);
            p.setPen(QColor(0xd0, 0xd0, 0xe8));
            p.drawText(QRectF(text.left(), text.top() + text.height() * 0.55,
                              text.width(), text.height() * 0.45),
                       Qt::AlignLeft | Qt::AlignVCenter, ov.text2);
        }
        break;
    }

    case Overlay::Bumper: {
        // Center-screen card with large stacked headline.
        p.fillRect(r, QColor(0x04, 0x04, 0x0a, 230));
        p.setPen(QPen(accent, 2));
        p.drawRect(r.adjusted(1, 1, -1, -1));

        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSizeF(std::max(14.0, r.height() * 0.30));
        f.setWeight(QFont::Black);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
        p.setFont(f);
        p.drawText(QRectF(r.left(), r.top() + r.height() * 0.20,
                          r.width(), r.height() * 0.40),
                   Qt::AlignCenter, ov.text1.toUpper());

        if (!ov.text2.isEmpty()) {
            f.setPointSizeF(std::max(10.0, r.height() * 0.16));
            f.setWeight(QFont::Normal);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
            p.setFont(f);
            p.setPen(QColor(0xd0, 0xd0, 0xe8));
            p.drawText(QRectF(r.left(), r.top() + r.height() * 0.58,
                              r.width(), r.height() * 0.30),
                       Qt::AlignCenter, ov.text2);
        }
        break;
    }
    }

    p.restore();
}

void PreviewCanvas::drawSlot(QPainter &p, const QRectF &rect, int idx) const
{
    const bool hasPart = (idx < m_parts.size());
    const Participant &part = hasPart ? m_parts[idx] : Participant{};

    // Slot background
    QPainterPath path;
    path.addRoundedRect(rect, 10, 10);
    p.fillPath(path, kSlotBg);

    // Border: drop-target highlight > talking ring > normal
    if (idx == m_hoveredSlot) {
        p.setPen(QPen(QColor(0x29, 0x79, 0xff), 3.0, Qt::SolidLine));
        p.drawPath(path);
    } else if (hasPart && part.isTalking) {
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
