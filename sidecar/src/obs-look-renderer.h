#pragma once

#include "look.h"
#include "obs-client.h"
#include <QString>
#include <QStringList>
#include <QVector>

class OBSLookRenderer {
public:
    struct Config {
        QString sourcePattern;
        QString fallbackSceneName;
        double canvasWidth = 1920.0;
        double canvasHeight = 1080.0;
    };

    OBSLookRenderer(OBSClient *client, Config config);

    QStringList sourceNamesForSlots(int slotCount) const;
    QStringList sourceNamesForLook(const Look &look) const;
    QString sceneNameForLook(const Look &look) const;
    QStringList sceneNamesForLooks(const QVector<Look> &looks) const;
    QStringList designLayerSourceNames(const Look &look) const;

    void provisionPlaceholders(int slotCount = 8) const;
    void provisionLooks(const QVector<Look> &looks) const;
    void renderLook(const Look &look,
                    bool makeProgram,
                    const QStringList &slotLabels = {}) const;

private:
    OBSClient *m_client = nullptr;
    Config m_config;
};
