#pragma once
#include "show-theme.h"
#include <QWidget>
#include <QVector>

class ThemeCard;

// Right-panel section: Show Themes grid
class ThemePanel : public QWidget {
    Q_OBJECT
public:
    explicit ThemePanel(QWidget *parent = nullptr);

    void loadThemes(const QVector<ShowTheme> &themes);
    QString selectedId() const { return m_selectedId; }

signals:
    void themeSelected(const ShowTheme &theme);

private:
    QString m_selectedId;
    QVector<ThemeCard *> m_cards;
    QWidget *m_grid = nullptr;
    void selectCard(ThemeCard *card, const ShowTheme &theme);
};

// ── ThemeCard ─────────────────────────────────────────────────────────────────
// Custom widget that paints a colour-preview of a ShowTheme.
class ThemeCard : public QWidget {
    Q_OBJECT
public:
    explicit ThemeCard(const ShowTheme &theme, QWidget *parent = nullptr);
    void setSelected(bool s);
    const ShowTheme &theme() const { return m_theme; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *)      override;

private:
    ShowTheme m_theme;
    bool m_selected = false;
    bool m_hovered  = false;
};
