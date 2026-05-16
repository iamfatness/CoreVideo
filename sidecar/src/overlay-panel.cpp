#include "overlay-panel.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

// ── OverlayPresetCard ─────────────────────────────────────────────────────────
// Clickable card showing the overlay type, its primary text, and a small
// schematic strip rendered with the same colors used on the canvas. Click
// fires overlayRequested.
class OverlayPresetCard : public QWidget {
    Q_OBJECT
public:
    explicit OverlayPresetCard(const Overlay &overlay, QWidget *parent = nullptr)
        : QWidget(parent), m_ov(overlay)
    {
        setFixedHeight(58);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    const Overlay &overlay() const { return m_ov; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QColor accent = m_ov.accent.isValid() ? m_ov.accent
                                                    : QColor(0x29, 0x79, 0xff);
        const QColor bg     = m_hovered ? QColor(0x18, 0x18, 0x26)
                                        : QColor(0x12, 0x12, 0x1c);
        const QColor border = m_hovered ? QColor(0x40, 0x40, 0x58)
                                        : QColor(0x22, 0x22, 0x30);

        QPainterPath card;
        card.addRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
        p.fillPath(card, bg);
        p.setPen(QPen(border, 1));
        p.drawPath(card);

        // Accent stripe
        p.fillRect(QRectF(2, 2, 4, height() - 4), accent);

        // Type label
        p.setPen(accent);
        QFont f = p.font();
        f.setPointSizeF(8.5);
        f.setWeight(QFont::Bold);
        p.setFont(f);
        p.drawText(QRectF(14, 6, width() - 20, 14),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   Overlay::typeToString(m_ov.type).toUpper());

        // Text1 (primary)
        p.setPen(QColor(0xe8, 0xe8, 0xf6));
        f.setPointSizeF(10.5);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.drawText(QRectF(14, 20, width() - 50, 18),
                   Qt::AlignLeft | Qt::AlignVCenter, m_ov.text1);

        // Text2 (secondary)
        if (!m_ov.text2.isEmpty()) {
            p.setPen(QColor(0x80, 0x80, 0xa0));
            f.setPointSizeF(8.5);
            f.setWeight(QFont::Normal);
            p.setFont(f);
            p.drawText(QRectF(14, 38, width() - 50, 14),
                       Qt::AlignLeft | Qt::AlignVCenter, m_ov.text2);
        }

        // "+" affordance on the right
        p.setPen(QColor(0x60, 0x60, 0x80));
        f.setPointSizeF(16);
        f.setWeight(QFont::Light);
        p.setFont(f);
        p.drawText(QRectF(width() - 32, 0, 24, height()),
                   Qt::AlignCenter, "+");
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) emit clicked();
    }
    void enterEvent(QEnterEvent *) override { m_hovered = true; update(); }
    void leaveEvent(QEvent *)      override { m_hovered = false; update(); }

private:
    Overlay m_ov;
    bool    m_hovered = false;
};

// ── OverlayActiveRow ──────────────────────────────────────────────────────────
class OverlayActiveRow : public QWidget {
    Q_OBJECT
public:
    explicit OverlayActiveRow(const Overlay &overlay, int activeIndex,
                              QWidget *parent = nullptr)
        : QWidget(parent), m_ov(overlay), m_idx(activeIndex)
    {
        setFixedHeight(28);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(8, 2, 4, 2);

        auto *lbl = new QLabel(Overlay::humanLabel(overlay), this);
        lbl->setStyleSheet("color: #c8c8e0; font-size: 11px;");
        row->addWidget(lbl, 1);

        auto *rm = new QPushButton("×", this);
        rm->setFixedSize(20, 20);
        rm->setStyleSheet(
            "QPushButton { color: #8080a0; background: transparent; "
            "border: none; font-size: 16px; font-weight: 700; }"
            "QPushButton:hover { color: #ff4060; }");
        row->addWidget(rm);

        connect(rm, &QPushButton::clicked, this, [this]() {
            emit removeRequested(m_idx);
        });
    }

signals:
    void removeRequested(int activeIndex);

private:
    Overlay m_ov;
    int     m_idx;
};

// ── OverlayPanel ──────────────────────────────────────────────────────────────

OverlayPanel::OverlayPanel(QWidget *parent) : QWidget(parent)
{
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    auto *hdr = new QLabel("Broadcast Overlays", this);
    hdr->setObjectName("sectionHeader");
    hdr->setStyleSheet("QLabel { padding: 10px 12px 6px 12px; }");
    vl->addWidget(hdr);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *inner = new QWidget(scroll);
    auto *innerV = new QVBoxLayout(inner);
    innerV->setContentsMargins(10, 0, 10, 10);
    innerV->setSpacing(6);

    buildPresets(innerV);

    auto *div = new QLabel("ACTIVE ON PVW", inner);
    div->setStyleSheet(
        "QLabel { color: #6a6a8a; font-size: 9px; font-weight: 800; "
        "letter-spacing: 0.12em; padding: 12px 2px 4px 2px; "
        "background: transparent; }");
    innerV->addWidget(div);

    auto *activeWrap = new QWidget(inner);
    m_activeLayout = new QVBoxLayout(activeWrap);
    m_activeLayout->setContentsMargins(0, 0, 0, 0);
    m_activeLayout->setSpacing(2);
    innerV->addWidget(activeWrap);

    auto *clearBtn = new QPushButton("Clear all overlays", inner);
    clearBtn->setFixedHeight(28);
    clearBtn->setStyleSheet(
        "QPushButton { color: #c0c0d8; background: #181822; "
        "border: 1px solid #25253a; border-radius: 6px; font-size: 11px; }"
        "QPushButton:hover { color: #ff6080; border-color: #40202a; }");
    innerV->addWidget(clearBtn);
    connect(clearBtn, &QPushButton::clicked,
            this, &OverlayPanel::clearOverlaysRequested);

    innerV->addStretch(1);

    scroll->setWidget(inner);
    vl->addWidget(scroll, 1);

    rebuildActiveList({});
}

void OverlayPanel::buildPresets(QVBoxLayout *outer)
{
    auto *hdr = new QLabel("PRESETS", this);
    hdr->setStyleSheet(
        "QLabel { color: #6a6a8a; font-size: 9px; font-weight: 800; "
        "letter-spacing: 0.12em; padding: 6px 2px 4px 2px; "
        "background: transparent; }");
    outer->addWidget(hdr);

    for (const auto &ov : Overlay::builtInPresets()) {
        auto *card = new OverlayPresetCard(ov, this);
        outer->addWidget(card);
        connect(card, &OverlayPresetCard::clicked, this, [this, card]() {
            emit overlayRequested(card->overlay());
        });
    }
}

void OverlayPanel::setActiveOverlays(const QVector<Overlay> &overlays)
{
    rebuildActiveList(overlays);
}

void OverlayPanel::rebuildActiveList(const QVector<Overlay> &overlays)
{
    for (auto *r : m_activeRows) r->deleteLater();
    m_activeRows.clear();

    if (overlays.isEmpty()) {
        auto *empty = new QLabel("(none)", this);
        empty->setStyleSheet("QLabel { color: #50506a; font-size: 11px; "
                             "padding: 6px 4px; background: transparent; }");
        m_activeLayout->addWidget(empty);
        m_activeRows.append(empty);
        return;
    }

    for (int i = 0; i < overlays.size(); ++i) {
        auto *row = new OverlayActiveRow(overlays[i], i, this);
        connect(row, &OverlayActiveRow::removeRequested,
                this, &OverlayPanel::removeOverlayRequested);
        m_activeLayout->addWidget(row);
        m_activeRows.append(row);
    }
}

#include "overlay-panel.moc"
