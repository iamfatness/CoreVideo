#pragma once
#include "layout-template.h"
#include <QObject>
#include <QVector>

class TemplateManager : public QObject {
    Q_OBJECT
public:
    static TemplateManager &instance();

    void loadBuiltIn();               // load from Qt resources
    void loadDirectory(const QString &path); // load from filesystem dir

    const QVector<LayoutTemplate> &templates() const { return m_templates; }
    const LayoutTemplate *findById(const QString &id) const;

    bool save(const LayoutTemplate &tmpl, const QString &dir);

signals:
    void templatesChanged();

private:
    TemplateManager() = default;
    void loadFile(const QString &path);
    QVector<LayoutTemplate> m_templates;
};
