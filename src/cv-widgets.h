#pragma once
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include "zoom-types.h"

// ── CvStatusDot ──────────────────────────────────────────────────────────────
// A QPainter-drawn status indicator that replaces the "●" unicode label.
// Animates with a sinusoidal pulse glow during transitional states
// (Joining, Leaving, Recovering).
class CvStatusDot : public QWidget {
    Q_OBJECT
public:
    explicit CvStatusDot(QWidget *parent = nullptr);
    void setState(MeetingState s);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    MeetingState m_state = MeetingState::Idle;
    float        m_phase = 0.0f;  // 0..1 for pulse cycle
    QTimer      *m_timer = nullptr;
    QColor       dotColor() const;
    bool         isAnimated() const;
};

// ── CvBanner ─────────────────────────────────────────────────────────────────
// A compact notice strip for first-run prompts, warnings, or status messages.
// Shows an optional underlined action button on the right.
enum class CvBannerKind { Info, Warning, Error };

class CvBanner : public QFrame {
    Q_OBJECT
public:
    explicit CvBanner(CvBannerKind kind, const QString &text,
                      QWidget *parent = nullptr);
    void setText(const QString &text);
    void setActionText(const QString &label);
signals:
    void actionClicked();
private:
    QLabel      *m_label      = nullptr;
    QPushButton *m_action_btn = nullptr;
};
