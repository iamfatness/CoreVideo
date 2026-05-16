#pragma once
#include "look.h"
#include <QWidget>
#include <QVector>

class LookCard;
class QVBoxLayout;

// Right-panel section: gallery of broadcast-ready Looks, grouped by
// category. Clicking a card stages the Look on PVW via lookSelected().
class LookPanel : public QWidget {
    Q_OBJECT
public:
    explicit LookPanel(QWidget *parent = nullptr);

    void loadLooks(const QVector<Look> &looks);
    QString selectedId() const { return m_selectedId; }

signals:
    void lookSelected(const Look &look);
    void createLookRequested();
    void setBackgroundRequested();
    void designLookRequested();

private:
    QString m_selectedId;
    QVector<LookCard *> m_cards;
    QVBoxLayout *m_listLayout = nullptr;
    void selectCard(LookCard *card, const Look &look);
    void clearCards();
};

// LookCard — single Look entry: schematic thumbnail + name + category.
class LookCard : public QWidget {
    Q_OBJECT
public:
    explicit LookCard(const Look &look, QWidget *parent = nullptr);
    void setSelected(bool s);
    const Look &look() const { return m_look; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *)   override;

private:
    Look m_look;
    bool m_selected = false;
    bool m_hovered  = false;
};
