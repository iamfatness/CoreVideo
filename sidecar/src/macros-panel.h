#pragma once
#include "macro.h"
#include <QWidget>
#include <QVector>

class MacroButton;

class MacrosPanel : public QWidget {
    Q_OBJECT
public:
    explicit MacrosPanel(QWidget *parent = nullptr);

    void loadMacros(const QVector<Macro> &macros);

signals:
    void macroTriggered(const Macro &macro);

private:
    QWidget *m_grid = nullptr;
    QVector<MacroButton *> m_buttons;
};

// ── MacroButton ───────────────────────────────────────────────────────────────
class MacroButton : public QWidget {
    Q_OBJECT
public:
    explicit MacroButton(const Macro &macro, QWidget *parent = nullptr);
    const Macro &macro() const { return m_macro; }

signals:
    void triggered(const Macro &macro);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    Macro m_macro;
    bool  m_hovered  = false;
    bool  m_pressed  = false;
};
