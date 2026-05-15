#include "macros-panel.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QSizePolicy>

// ─────────────────────────────────────────────────────────────────────────────
// MacroButton
// ─────────────────────────────────────────────────────────────────────────────

MacroButton::MacroButton(const Macro &macro, QWidget *parent)
    : QWidget(parent)
    , m_macro(macro)
{
    setFixedSize(118, 82);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
}

void MacroButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect().adjusted(1, 1, -1, -1);
    const qreal radius = 10.0;

    // Background color — parse accent, darken based on state
    QColor accent(m_macro.color);
    QColor bg = m_pressed  ? accent.darker(300)
              : m_hovered  ? accent.darker(400)
                           : accent.darker(500);

    // Border width
    const qreal borderWidth = m_hovered ? 2.0 : 1.5;

    // Draw card background
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    p.fillPath(path, bg);

    // Draw border
    p.setPen(QPen(accent, borderWidth));
    p.drawPath(path);

    // ── Icon (top 55% of card) ────────────────────────────────────────────
    const int iconAreaHeight = static_cast<int>(height() * 0.55);
    const QRectF iconRect(0, 0, width(), iconAreaHeight);

    QFont iconFont = p.font();
    iconFont.setPixelSize(24);
    p.setFont(iconFont);
    p.setPen(Qt::white);
    p.drawText(iconRect, Qt::AlignHCenter | Qt::AlignVCenter, m_macro.icon);

    // ── Label (bottom 22px) ───────────────────────────────────────────────
    const QRectF labelRect(0, height() - 22, width(), 22);

    QFont labelFont = p.font();
    labelFont.setPixelSize(10);   // ~10.5px
    labelFont.setWeight(QFont::DemiBold);
    p.setFont(labelFont);
    p.setPen(Qt::white);
    p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter, m_macro.label);
}

void MacroButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        update();
    }
    QWidget::mousePressEvent(event);
}

void MacroButton::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (rect().contains(event->pos()))
            emit triggered(m_macro);
        m_pressed = false;
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void MacroButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void MacroButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    m_pressed = false;
    update();
    QWidget::leaveEvent(event);
}

// ─────────────────────────────────────────────────────────────────────────────
// MacrosPanel
// ─────────────────────────────────────────────────────────────────────────────

MacrosPanel::MacrosPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Header
    auto *header = new QLabel("Macros", this);
    header->setObjectName("sectionHeader");
    vbox->addWidget(header);

    // Grid container
    m_grid = new QWidget(this);
    auto *grid = new QGridLayout(m_grid);
    grid->setContentsMargins(10, 0, 10, 8);
    grid->setSpacing(8);
    vbox->addWidget(m_grid);

    vbox->addStretch();

    loadMacros(Macro::defaults());
}

void MacrosPanel::loadMacros(const QVector<Macro> &macros)
{
    // Remove and delete all existing buttons
    for (MacroButton *btn : m_buttons) {
        btn->hide();
        btn->deleteLater();
    }
    m_buttons.clear();

    // Clear the grid layout
    QGridLayout *grid = qobject_cast<QGridLayout *>(m_grid->layout());
    if (!grid)
        return;

    // Remove all items from the layout
    while (grid->count() > 0) {
        QLayoutItem *item = grid->takeAt(0);
        delete item;
    }

    // Rebuild grid with 2 columns
    constexpr int cols = 2;
    for (int i = 0; i < macros.size(); ++i) {
        auto *btn = new MacroButton(macros[i], m_grid);
        connect(btn, &MacroButton::triggered, this, [this](const Macro &macro) {
            emit macroTriggered(macro);
        });
        grid->addWidget(btn, i / cols, i % cols);
        m_buttons.append(btn);
    }
}
