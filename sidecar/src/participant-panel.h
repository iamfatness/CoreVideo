#pragma once
#include <QWidget>
#include <QVector>
#include <QString>
#include <QColor>

struct ParticipantInfo {
    int     id;
    QString name;
    QString initials;
    QColor  color;
    bool    hasVideo   = false;
    bool    isTalking  = false;
    bool    isSharingScreen = false;
    int     slotAssign = -1;  // -1 = unassigned
};

class ParticipantCard;
class QVBoxLayout;
class QLabel;

class ParticipantPanel : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantPanel(QWidget *parent = nullptr);

    void setParticipants(const QVector<ParticipantInfo> &participants);

    // Drives the "Click a participant to assign to Slot N" banner. Pass -1
    // to clear it (default browse mode).
    void setAssignTarget(int slotIndex);

signals:
    void assignRequested(int participantId, int slotIndex);
    // Emitted whenever a participant card is clicked, regardless of mode.
    // MainWindow decides whether to consume it as a slot assignment.
    void participantClicked(int participantId);

private:
    QWidget      *m_listArea    = nullptr;
    QVBoxLayout  *m_listLayout  = nullptr;
    QLabel       *m_countLabel  = nullptr;
    QWidget      *m_assignBanner = nullptr;
    QLabel       *m_assignLabel  = nullptr;
    int           m_assignTarget = -1;
    QVector<ParticipantCard *> m_cards;
};

// ── ParticipantCard ───────────────────────────────────────────────────────────
class ParticipantCard : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantCard(const ParticipantInfo &info, QWidget *parent = nullptr);

signals:
    void assignClicked(int participantId);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    ParticipantInfo m_info;
    QPoint          m_dragStart;
};
