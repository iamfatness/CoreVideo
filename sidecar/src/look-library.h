#pragma once
#include "look.h"
#include <QVector>

// LookLibrary loads bundled "broadcast-ready" Look presets from the Qt
// resource system (:/looks/data/looks/*.json) and resolves each Look's
// templateId against the TemplateManager so callers receive a ready-to-
// stage Look.
//
// Singleton instance — load once at startup, query freely.
class LookLibrary {
public:
    static LookLibrary &instance();

    // Reads every JSON file under :/looks/data/looks/. Idempotent.
    void loadBuiltIn();

    const QVector<Look> &looks() const { return m_looks; }
    const Look *findById(const QString &id) const;

    // Categories in load order, deduped.
    QStringList categories() const;

private:
    LookLibrary() = default;
    QVector<Look> m_looks;
    bool          m_loaded = false;
};
