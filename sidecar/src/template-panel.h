#pragma once
#include "layout-template.h"
#include <QWidget>
#include <QVector>

class TemplateThumbnail;

// Right-panel section: Quick Layout Templates grid
class TemplatePanel : public QWidget {
    Q_OBJECT
public:
    explicit TemplatePanel(QWidget *parent = nullptr);

    void loadTemplates(const QVector<LayoutTemplate> &templates);
    QString selectedId() const { return m_selectedId; }

signals:
    void templateSelected(const LayoutTemplate &tmpl);

private:
    QString m_selectedId;
    QVector<TemplateThumbnail *> m_thumbs;
    QWidget *m_grid = nullptr;
    void selectThumb(TemplateThumbnail *t, const LayoutTemplate &tmpl);
};

// ── TemplateThumbnail ─────────────────────────────────────────────────────────
// Custom widget that draws a schematic diagram of the layout.
class TemplateThumbnail : public QWidget {
    Q_OBJECT
public:
    explicit TemplateThumbnail(const LayoutTemplate &tmpl, QWidget *parent = nullptr);
    void setSelected(bool s);
    const LayoutTemplate &tmpl() const { return m_tmpl; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *)   override;

private:
    LayoutTemplate m_tmpl;
    bool m_selected = false;
    bool m_hovered  = false;
};
