#include "participant-panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>

// ── ParticipantCard ───────────────────────────────────────────────────────────

ParticipantCard::ParticipantCard(const ParticipantInfo &info, QWidget *parent)
    : QWidget(parent), m_info(info)
{
    setFixedHeight(56);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(10);

    // Avatar placeholder — painted in paintEvent; we add spacer for its area
    row->addSpacing(40);

    // Name + ID column
    auto *col = new QVBoxLayout;
    col->setSpacing(1);

    auto *nameLabel = new QLabel(info.name, this);
    nameLabel->setStyleSheet("color: #e0e0f0; font-size: 13px; font-weight: 600; background: transparent;");

    auto *idLabel   = new QLabel(QString("ID %1").arg(info.id), this);
    idLabel->setStyleSheet("color: #5a5a7a; font-size: 10px; background: transparent;");

    col->addWidget(nameLabel);
    col->addWidget(idLabel);
    row->addLayout(col, 1);

    // Assign button
    auto *assignBtn = new QPushButton(
        info.slotAssign >= 0 ? QString("Slot %1").arg(info.slotAssign + 1) : "Assign",
        this);
    assignBtn->setObjectName("assignBtn");
    assignBtn->setFixedSize(56, 26);
    row->addWidget(assignBtn);

    connect(assignBtn, &QPushButton::clicked, this, [this]() {
        emit assignClicked(m_info.id);
    });
}

void ParticipantCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        m_dragStart = e->pos();
    QWidget::mousePressEvent(e);
}

void ParticipantCard::mouseMoveEvent(QMouseEvent *e)
{
    if (!(e->buttons() & Qt::LeftButton)) return;
    if ((e->pos() - m_dragStart).manhattanLength() < QApplication::startDragDistance()) return;

    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData("application/x-cv-participant",
                  QByteArray::number(m_info.id));
    drag->setMimeData(mime);

    // Render card as drag pixmap
    QPixmap px(size());
    px.fill(Qt::transparent);
    render(&px);
    drag->setPixmap(px.scaled(px.size() * 0.85, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    drag->setHotSpot(e->pos() * 85 / 100);

    drag->exec(Qt::CopyAction);
}

void ParticipantCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card background
    QPainterPath card;
    card.addRoundedRect(rect(), 8, 8);
    p.fillPath(card, QColor(0x16, 0x16, 0x22));

    // Avatar circle (left side, vertically centered)
    const QPointF center{33.0, height() / 2.0};
    const float r = 16.0f;

    if (m_info.isTalking) {
        QRadialGradient glow(center, r + 5);
        glow.setColorAt(0, QColor(0x20, 0x90, 0xff, 70));
        glow.setColorAt(1, Qt::transparent);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(center, r + 5, r + 5);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(m_info.color);
    p.drawEllipse(center, r, r);

    // White ring
    p.setPen(QPen(QColor(255, 255, 255, 50), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, r, r);

    // Initials
    if (!m_info.initials.isEmpty()) {
        QFont f = p.font();
        f.setPointSizeF(9);
        f.setWeight(QFont::Bold);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(center.x() - r, center.y() - r, 2 * r, 2 * r),
                   Qt::AlignCenter, m_info.initials);
    }

    // Talking indicator dot (bottom-right of avatar)
    if (m_info.isTalking) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x20, 0xd4, 0x60));
        p.drawEllipse(QPointF(center.x() + r - 4, center.y() + r - 4), 5, 5);
    }
}

// ── ParticipantPanel ──────────────────────────────────────────────────────────

ParticipantPanel::ParticipantPanel(QWidget *parent) : QWidget(parent)
{
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // Header
    auto *header = new QWidget(this);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(12, 10, 8, 6);

    auto *hdr = new QLabel("Participants", this);
    hdr->setObjectName("sectionHeader");
    hdr->setStyleSheet("QLabel { color: #c0c0e0; font-size: 11px; font-weight: 700; "
                       "letter-spacing: 0.08em; text-transform: uppercase; padding: 0; }");
    hl->addWidget(hdr, 1);

    m_countLabel = new QLabel("0", this);
    m_countLabel->setStyleSheet("color: #5a5a7a; font-size: 11px; background: transparent;");
    hl->addWidget(m_countLabel);

    vl->addWidget(header);

    // Assign-mode banner — hidden unless setAssignTarget() activates it
    m_assignBanner = new QWidget(this);
    m_assignBanner->setObjectName("assignBanner");
    m_assignBanner->setStyleSheet(
        "#assignBanner { background: #1a2a4a; border: 1px solid #2979ff; "
        "border-radius: 6px; }");
    auto *bl = new QHBoxLayout(m_assignBanner);
    bl->setContentsMargins(10, 6, 6, 6);
    bl->setSpacing(6);
    m_assignLabel = new QLabel("", m_assignBanner);
    m_assignLabel->setStyleSheet(
        "color: #cfe0ff; font-size: 12px; font-weight: 600; background: transparent;");
    auto *cancel = new QToolButton(m_assignBanner);
    cancel->setText("✕");
    cancel->setToolTip("Cancel assignment");
    cancel->setStyleSheet(
        "QToolButton { color: #cfe0ff; background: transparent; border: none; "
        "font-size: 13px; padding: 2px 6px; }"
        "QToolButton:hover { color: #ffffff; }");
    connect(cancel, &QToolButton::clicked, this, [this]() { setAssignTarget(-1); });
    bl->addWidget(m_assignLabel, 1);
    bl->addWidget(cancel);
    m_assignBanner->hide();

    auto *bannerWrap = new QWidget(this);
    auto *bwl = new QHBoxLayout(bannerWrap);
    bwl->setContentsMargins(8, 0, 8, 6);
    bwl->addWidget(m_assignBanner);
    vl->addWidget(bannerWrap);

    // Scrollable list
    auto *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }"
                          "QScrollBar:vertical { width: 4px; background: transparent; }"
                          "QScrollBar::handle:vertical { background: #2a2a3a; border-radius: 2px; }"
                          "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    m_listArea = new QWidget(scroll);
    m_listArea->setStyleSheet("background: transparent;");
    m_listLayout = new QVBoxLayout(m_listArea);
    m_listLayout->setContentsMargins(8, 0, 8, 8);
    m_listLayout->setSpacing(4);
    m_listLayout->addStretch(1);

    scroll->setWidget(m_listArea);
    vl->addWidget(scroll, 1);
}

void ParticipantPanel::setParticipants(const QVector<ParticipantInfo> &participants)
{
    for (auto *c : m_cards) c->deleteLater();
    m_cards.clear();

    // Remove all but the stretch
    while (m_listLayout->count() > 1)
        delete m_listLayout->takeAt(0);

    for (const auto &info : participants) {
        auto *card = new ParticipantCard(info, m_listArea);
        m_cards.append(card);
        m_listLayout->insertWidget(m_listLayout->count() - 1, card);
        connect(card, &ParticipantCard::assignClicked, this, [this](int pid) {
            emit participantClicked(pid);
            if (m_assignTarget >= 0) {
                emit assignRequested(pid, m_assignTarget);
                setAssignTarget(-1);
            }
        });
    }

    m_countLabel->setText(QString::number(participants.size()));
}

void ParticipantPanel::setAssignTarget(int slotIndex)
{
    m_assignTarget = slotIndex;
    if (slotIndex < 0) {
        m_assignBanner->hide();
        return;
    }
    m_assignLabel->setText(
        QString("Click a participant to assign to Slot %1").arg(slotIndex + 1));
    m_assignBanner->show();
}
