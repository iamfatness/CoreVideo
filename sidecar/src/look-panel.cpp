#include "look-panel.h"
#include "show-theme.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>

// ── LookCard ──────────────────────────────────────────────────────────────────

LookCard::LookCard(const Look &look, QWidget *parent)
    : QWidget(parent), m_look(look)
{
    setFixedHeight(72);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void LookCard::setSelected(bool s) { m_selected = s; update(); }

void LookCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Resolve the look's theme for the schematic accent — falls back to a
    // neutral blue if the theme id doesn't match a built-in.
    QColor accent(0x29, 0x79, 0xff);
    if (!m_look.themeId.isEmpty()) {
        for (const auto &t : ShowTheme::builtIns()) {
            if (t.id == m_look.themeId) { accent = t.accent; break; }
        }
    }

    const QColor bg     = m_selected ? QColor(0x14, 0x14, 0x22)
                        : m_hovered  ? QColor(0x18, 0x18, 0x26)
                                     : QColor(0x12, 0x12, 0x1c);
    const QColor border = m_selected ? accent
                        : m_hovered  ? QColor(0x40, 0x40, 0x58)
                                     : QColor(0x22, 0x22, 0x30);

    QPainterPath card;
    card.addRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    p.fillPath(card, bg);
    p.setPen(QPen(border, m_selected ? 2.0 : 1.0));
    p.drawPath(card);

    // Schematic on the left
    const QRectF diag(8, 8, 88, height() - 16);
    p.fillRect(diag, QColor(0x08, 0x08, 0x10));
    const float gap = 1.5f;
    for (const auto &slot : m_look.tmpl.slotList) {
        QRectF r(
            diag.left() + slot.x * diag.width()  + gap,
            diag.top()  + slot.y * diag.height() + gap,
            slot.width  * diag.width()  - gap * 2,
            slot.height * diag.height() - gap * 2
        );
        QPainterPath sp;
        sp.addRoundedRect(r, 2, 2);
        p.fillPath(sp, QColor(accent.red(), accent.green(), accent.blue(), 60));
        p.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 180), 1));
        p.drawPath(sp);
    }

    // Text column on the right
    const QRectF textArea(diag.right() + 10, 8,
                         width() - diag.right() - 18, height() - 16);

    p.setPen(m_selected ? QColor(0xff, 0xff, 0xff) : QColor(0xe0, 0xe0, 0xf0));
    QFont f = p.font();
    f.setPointSizeF(10.5);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.drawText(QRectF(textArea.left(), textArea.top(), textArea.width(), 18),
               Qt::AlignLeft | Qt::AlignVCenter, m_look.name);

    p.setPen(QColor(accent.red(), accent.green(), accent.blue(), 220));
    f.setPointSizeF(8.5);
    f.setWeight(QFont::Bold);
    p.setFont(f);
    p.drawText(QRectF(textArea.left(), textArea.top() + 18, textArea.width(), 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               m_look.category.toUpper());

    if (!m_look.description.isEmpty()) {
        p.setPen(QColor(0x80, 0x80, 0xa0));
        f.setPointSizeF(8.5);
        f.setWeight(QFont::Normal);
        p.setFont(f);
        p.drawText(QRectF(textArea.left(), textArea.top() + 34,
                          textArea.width(), 18),
                   Qt::AlignLeft | Qt::AlignVCenter, m_look.description);
    }
}

void LookCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) emit clicked();
}
void LookCard::enterEvent(QEnterEvent *) { m_hovered = true;  update(); }
void LookCard::leaveEvent(QEvent *)      { m_hovered = false; update(); }

// ── LookPanel ─────────────────────────────────────────────────────────────────

LookPanel::LookPanel(QWidget *parent) : QWidget(parent)
{
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    auto *hdr = new QLabel("Broadcast Looks", this);
    hdr->setObjectName("sectionHeader");
    hdr->setStyleSheet("QLabel { padding: 10px 12px 6px 12px; }");
    vl->addWidget(hdr);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *inner = new QWidget(scroll);
    m_listLayout = new QVBoxLayout(inner);
    m_listLayout->setContentsMargins(10, 0, 10, 10);
    m_listLayout->setSpacing(6);
    m_listLayout->addStretch(1);

    scroll->setWidget(inner);
    vl->addWidget(scroll, 1);
}

void LookPanel::clearCards()
{
    for (auto *c : m_cards) c->deleteLater();
    m_cards.clear();
    // Remove any category headers we added (everything except trailing stretch)
    while (m_listLayout->count() > 1) {
        auto *item = m_listLayout->takeAt(0);
        if (auto *w = item->widget()) w->deleteLater();
        delete item;
    }
}

void LookPanel::loadLooks(const QVector<Look> &looks)
{
    clearCards();

    // Group by category, preserving load order
    QStringList categories;
    for (const auto &l : looks) {
        const QString c = l.category.isEmpty() ? QStringLiteral("General")
                                               : l.category;
        if (!categories.contains(c)) categories.append(c);
    }

    auto insertAt = [this]() { return m_listLayout->count() - 1; };

    for (const QString &cat : categories) {
        auto *hdr = new QLabel(cat.toUpper(), this);
        hdr->setStyleSheet("QLabel { color: #6a6a8a; font-size: 9px; "
                           "font-weight: 800; letter-spacing: 0.12em; "
                           "padding: 8px 2px 2px 2px; background: transparent; }");
        m_listLayout->insertWidget(insertAt(), hdr);

        for (const auto &l : looks) {
            const QString lc = l.category.isEmpty() ? QStringLiteral("General")
                                                    : l.category;
            if (lc != cat) continue;
            auto *card = new LookCard(l, this);
            m_cards.append(card);
            m_listLayout->insertWidget(insertAt(), card);
            connect(card, &LookCard::clicked, this, [this, card]() {
                selectCard(card, card->look());
            });
        }
    }
}

void LookPanel::selectCard(LookCard *card, const Look &look)
{
    m_selectedId = look.id;
    for (auto *c : m_cards) c->setSelected(c == card);
    emit lookSelected(look);
}
