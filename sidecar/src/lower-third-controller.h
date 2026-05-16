#pragma once

#include "look.h"
#include "participant-panel.h"
#include <QHash>

struct LowerThirdTemplate {
    QString id = QStringLiteral("clean-name");
    QString subtitle;
    double height = 0.10;
};

struct LowerThirdOverride {
    bool enabled = false;
    QString name;
    QString subtitle;
};

class LowerThirdController {
public:
    void setTemplate(const LowerThirdTemplate &tmpl) { m_template = tmpl; }
    void setOverride(int participantId, const LowerThirdOverride &value);
    void clearOverride(int participantId);

    QVector<Overlay> participantSyncedOverlays(
        const Look &look,
        const QVector<ParticipantInfo> &participants) const;

    static bool isAutoLowerThird(const Overlay &overlay);

private:
    LowerThirdTemplate m_template;
    QHash<int, LowerThirdOverride> m_overrides;
};
