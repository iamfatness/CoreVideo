#include "cv-widgets.h"
#include <QPainter>
#include <QHBoxLayout>
#include <cmath>

// ── CvStatusDot ──────────────────────────────────────────────────────────────

CvStatusDot::CvStatusDot(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(14, 14);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);

    m_timer = new QTimer(this);
    m_timer->setInterval(50);  // ~20 fps — smooth but cheap
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_phase += 0.05f;
        if (m_phase > 1.0f) m_phase -= 1.0f;
        update();
    });
}

void CvStatusDot::setState(MeetingState s)
{
    if (m_state == s) return;
    m_state = s;
    m_phase = 0.0f;

    if (isAnimated()) {
        if (!m_timer->isActive()) m_timer->start();
    } else {
        m_timer->stop();
    }
    update();
}

QSize CvStatusDot::sizeHint()        const { return {14, 14}; }
QSize CvStatusDot::minimumSizeHint() const { return {14, 14}; }

QColor CvStatusDot::dotColor() const
{
    switch (m_state) {
    case MeetingState::InMeeting:  return {0x22, 0xcc, 0x44};
    case MeetingState::Joining:
    case MeetingState::Leaving:
    case MeetingState::Recovering: return {0xf0, 0xa0, 0x00};
    case MeetingState::Failed:     return {0xee, 0x33, 0x33};
    default:                       return {0x66, 0x66, 0x66};
    }
}

bool CvStatusDot::isAnimated() const
{
    return m_state == MeetingState::Joining ||
           m_state == MeetingState::Leaving ||
           m_state == MeetingState::Recovering;
}

void CvStatusDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor color = dotColor();
    const QRectF inner = QRectF(2, 2, width() - 4, height() - 4);

    if (isAnimated()) {
        // Outer pulse ring that fades in/out
        const float pulse = 0.5f + 0.5f * std::sin(m_phase * 2.0f * 3.14159f);
        QColor glow = color;
        glow.setAlphaF(0.28f * pulse);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(QRectF(0, 0, width(), height()));
    }

    // Inner solid dot
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(inner);
}

// ── CvBanner ─────────────────────────────────────────────────────────────────

static void apply_banner_style(QFrame *frame, QLabel *label,
                                QPushButton *btn, CvBannerKind kind)
{
    const char *bg, *border, *accent;
    switch (kind) {
    case CvBannerKind::Warning:
        bg = "rgba(240,160,0,0.10)"; border = "rgba(240,160,0,0.40)";
        accent = "#e0a020"; break;
    case CvBannerKind::Error:
        bg = "rgba(220,50,50,0.12)"; border = "rgba(220,50,50,0.45)";
        accent = "#ee5555"; break;
    case CvBannerKind::Info:
    default:
        bg = "rgba(29,109,194,0.12)"; border = "rgba(29,109,194,0.50)";
        accent = "#5aabee"; break;
    }

    frame->setStyleSheet(QString(
        "QFrame { background-color: %1; border: 1px solid %2; border-radius: 4px; }"
        "QLabel { color: %3; background: transparent; border: none; font-size: 12px; }"
    ).arg(bg, border, accent));

    const QString btn_style = QString(
        "QPushButton { color: %1; background: transparent; border: none; "
        "text-decoration: underline; padding: 0; min-height: 0; font-size: 12px; }"
        "QPushButton:hover { color: white; }"
    ).arg(accent);
    btn->setStyleSheet(btn_style);
    Q_UNUSED(label)
}

CvBanner::CvBanner(CvBannerKind kind, const QString &text, QWidget *parent)
    : QFrame(parent)
{
    setContentsMargins(0, 0, 0, 0);

    m_label      = new QLabel(text, this);
    m_label->setWordWrap(true);

    m_action_btn = new QPushButton(this);
    m_action_btn->setVisible(false);
    m_action_btn->setFlat(true);
    connect(m_action_btn, &QPushButton::clicked, this, &CvBanner::actionClicked);

    apply_banner_style(this, m_label, m_action_btn, kind);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 7, 10, 7);
    row->setSpacing(8);
    row->addWidget(m_label, 1);
    row->addWidget(m_action_btn);
}

void CvBanner::setText(const QString &text)      { m_label->setText(text); }

void CvBanner::setActionText(const QString &label)
{
    m_action_btn->setText(label);
    m_action_btn->setVisible(!label.isEmpty());
}
