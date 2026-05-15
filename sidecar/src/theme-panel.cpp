#include "theme-panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFrame>
#include <QScrollArea>

// ── ThemeCard ─────────────────────────────────────────────────────────────────

ThemeCard::ThemeCard(const ShowTheme &theme, QWidget *parent)
    : QWidget(parent), m_theme(theme)
{
    setFixedSize(130, 90);
    setCursor(Qt::PointingHandCursor);
}

void ThemeCard::setSelected(bool s)
{
    m_selected = s;
    update();
}

void ThemeCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card background
    QPainterPath card;
    card.addRoundedRect(rect(), 10, 10);
    p.fillPath(card, m_theme.panelBg);

    // Border: selected → accent 2px, hovered → lighter border 1.5px, default → dark 1.5px
    QColor border;
    qreal  borderWidth;
    if (m_selected) {
        border      = m_theme.accent;
        borderWidth = 2.0;
    } else if (m_hovered) {
        border      = m_theme.accent.lighter(140);
        borderWidth = 1.5;
    } else {
        border      = QColor(0x20, 0x20, 0x2c);
        borderWidth = 1.5;
    }
    p.setPen(QPen(border, borderWidth));
    p.drawPath(card);

    // Two accent strips in the top half
    const int    stripTop    = 10;
    const int    stripH      = 18;
    const int    stripW      = (width() - 24) / 2 - 4;  // half width with gap
    const int    stripLeft1  = 10;
    const int    stripLeft2  = stripLeft1 + stripW + 6;
    const int    stripRadius = 4;

    QPainterPath strip1;
    strip1.addRoundedRect(QRectF(stripLeft1, stripTop, stripW, stripH),
                          stripRadius, stripRadius);
    p.fillPath(strip1, m_theme.accent);

    QPainterPath strip2;
    strip2.addRoundedRect(QRectF(stripLeft2, stripTop, stripW, stripH),
                          stripRadius, stripRadius);
    p.fillPath(strip2, m_theme.accentAlt);

    // Name label at bottom
    p.setPen(m_selected ? QColor(0xff, 0xff, 0xff) : m_theme.textSecondary);
    QFont f = p.font();
    f.setPointSizeF(9.5);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.drawText(QRectF(0, height() - 24, width(), 20),
               Qt::AlignHCenter | Qt::AlignVCenter, m_theme.name);
}

void ThemeCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) emit clicked();
}

void ThemeCard::enterEvent(QEnterEvent *) { m_hovered = true;  update(); }
void ThemeCard::leaveEvent(QEvent *)       { m_hovered = false; update(); }

// ── ThemePanel ────────────────────────────────────────────────────────────────

ThemePanel::ThemePanel(QWidget *parent) : QWidget(parent)
{
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // Section header
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(12, 10, 8, 6);
    auto *hdr = new QLabel("Show Themes", this);
    hdr->setObjectName("sectionHeader");
    hdr->setStyleSheet("QLabel { padding: 0; }");
    headerRow->addWidget(hdr, 1);
    vl->addLayout(headerRow);

    // Scrollable area containing the grid
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setWidgetResizable(true);

    m_grid = new QWidget(scrollArea);
    scrollArea->setWidget(m_grid);

    vl->addWidget(scrollArea, 1);
}

void ThemePanel::loadThemes(const QVector<ShowTheme> &themes)
{
    // Clear existing cards and layout
    for (auto *c : m_cards) c->deleteLater();
    m_cards.clear();

    // Delete the old layout if one exists
    if (auto *old = m_grid->layout()) {
        QLayoutItem *item;
        while ((item = old->takeAt(0)) != nullptr) delete item;
        delete old;
    }

    auto *grid = new QGridLayout(m_grid);
    grid->setContentsMargins(10, 0, 10, 8);
    grid->setSpacing(8);

    int col = 0, row = 0;
    for (const auto &theme : themes) {
        auto *card = new ThemeCard(theme, m_grid);
        m_cards.append(card);
        grid->addWidget(card, row, col);
        connect(card, &ThemeCard::clicked, this, [this, card]() {
            selectCard(card, card->theme());
        });
        if (++col >= 2) { col = 0; ++row; }
    }

    // Auto-select first theme
    if (!m_cards.isEmpty())
        selectCard(m_cards.first(), m_cards.first()->theme());
}

void ThemePanel::selectCard(ThemeCard *card, const ShowTheme &theme)
{
    m_selectedId = theme.id;
    for (auto *c : m_cards)
        c->setSelected(c == card);
    emit themeSelected(theme);
}
