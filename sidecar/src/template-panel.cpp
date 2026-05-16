#include "template-panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>

// ── TemplateThumbnail ─────────────────────────────────────────────────────────

TemplateThumbnail::TemplateThumbnail(const LayoutTemplate &tmpl, QWidget *parent)
    : QWidget(parent), m_tmpl(tmpl)
{
    setFixedSize(118, 82);
    setCursor(Qt::PointingHandCursor);
}

void TemplateThumbnail::setSelected(bool s)
{
    m_selected = s;
    update();
}

void TemplateThumbnail::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor bg     = m_selected ? QColor(0x10, 0x10, 0x2a)
                        : m_hovered  ? QColor(0x18, 0x18, 0x28)
                                     : QColor(0x1a, 0x1a, 0x24);
    const QColor border = m_selected ? QColor(0x29, 0x79, 0xff)
                        : m_hovered  ? QColor(0x40, 0x60, 0xe0)
                                     : QColor(0x25, 0x25, 0x35);

    // Card background
    QPainterPath card;
    card.addRoundedRect(rect(), 10, 10);
    p.fillPath(card, bg);
    p.setPen(QPen(border, m_selected ? 2.0 : 1.5));
    p.drawPath(card);

    // Schematic diagram area — leaves room for label at bottom
    const QRectF diag(8, 8, width() - 16, height() - 28);
    const QColor slotFill  = m_selected ? QColor(0x1e, 0x3a, 0xdf, 80)
                                        : QColor(0x30, 0x30, 0x48);
    const QColor slotBdr   = m_selected ? QColor(0x29, 0x79, 0xff, 180)
                                        : QColor(0x3a, 0x3a, 0x58);
    const float  gap = 2.5f;

    for (const auto &slot : m_tmpl.slotList) {
        QRectF r(
            diag.left() + slot.x * diag.width()  + gap,
            diag.top()  + slot.y * diag.height() + gap,
            slot.width  * diag.width()  - gap * 2,
            slot.height * diag.height() - gap * 2
        );
        QPainterPath sp;
        sp.addRoundedRect(r, 3, 3);
        p.fillPath(sp, slotFill);
        p.setPen(QPen(slotBdr, 1));
        p.drawPath(sp);
    }

    // Label
    p.setPen(m_selected ? QColor(0xff, 0xff, 0xff)
                        : QColor(0x90, 0x90, 0xb8));
    QFont f = p.font();
    f.setPointSizeF(9.5);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.drawText(QRectF(0, height() - 20, width(), 18),
               Qt::AlignHCenter | Qt::AlignVCenter, m_tmpl.name);
}

void TemplateThumbnail::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) emit clicked();
}

void TemplateThumbnail::enterEvent(QEnterEvent *) { m_hovered = true;  update(); }
void TemplateThumbnail::leaveEvent(QEvent *)       { m_hovered = false; update(); }

// ── TemplatePanel ─────────────────────────────────────────────────────────────

TemplatePanel::TemplatePanel(QWidget *parent) : QWidget(parent)
{
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(12, 10, 8, 6);
    auto *hdr = new QLabel("Quick Layout Templates", this);
    hdr->setObjectName("sectionHeader");
    hdr->setStyleSheet("QLabel { padding: 0; }");
    headerRow->addWidget(hdr, 1);
    vl->addLayout(headerRow);

    m_grid = new QWidget(this);
    vl->addWidget(m_grid);
    vl->addStretch(1);
}

void TemplatePanel::loadTemplates(const QVector<LayoutTemplate> &templates)
{
    // Clear old
    for (auto *t : m_thumbs) t->deleteLater();
    m_thumbs.clear();

    auto *grid = new QGridLayout(m_grid);
    grid->setContentsMargins(10, 0, 10, 8);
    grid->setSpacing(8);

    int col = 0, row = 0;
    for (const auto &tmpl : templates) {
        auto *thumb = new TemplateThumbnail(tmpl, m_grid);
        m_thumbs.append(thumb);
        grid->addWidget(thumb, row, col);
        connect(thumb, &TemplateThumbnail::clicked, this, [this, thumb]() {
            selectThumb(thumb, thumb->tmpl());
        });
        if (++col >= 2) { col = 0; ++row; }
    }

    // Select first template by default
    if (!m_thumbs.isEmpty())
        selectThumb(m_thumbs.first(), m_thumbs.first()->tmpl());
}

void TemplatePanel::selectThumb(TemplateThumbnail *t, const LayoutTemplate &tmpl)
{
    m_selectedId = tmpl.id;
    for (auto *th : m_thumbs)
        th->setSelected(th == t);
    emit templateSelected(tmpl);
}
