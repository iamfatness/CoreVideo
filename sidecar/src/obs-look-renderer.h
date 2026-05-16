#pragma once

#include "look-render-plan.h"
#include "look.h"
#include "obs-client.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <utility>

class OBSLookRenderer {
public:
    using RenderPlan = LookRenderPlan;

    struct Config {
        QString sourcePattern;
        QString fallbackSceneName;
        double canvasWidth = 1920.0;
        double canvasHeight = 1080.0;

        void normalizeBroadcastCanvas()
        {
            if (canvasWidth <= 0.0)
                canvasWidth = 1920.0;
            if (canvasHeight <= 0.0)
                canvasHeight = 1080.0;
            if (canvasHeight > canvasWidth)
                std::swap(canvasWidth, canvasHeight);
        }
    };

    OBSLookRenderer(OBSClient *client, Config config);

    QStringList sourceNamesForSlots(int slotCount) const;
    QStringList sourceNamesForLook(const Look &look) const;
    QString sceneNameForLook(const Look &look) const;
    QStringList sceneNamesForLooks(const QVector<Look> &looks) const;
    QStringList designLayerSourceNames(const Look &look) const;
    QStringList slotLabelsForLook(const Look &look, const QStringList &slotLabels = {}) const;
    RenderPlan renderPlanForLook(const Look &look,
                                 bool makeProgram,
                                 const QStringList &slotLabels = {}) const;

    void provisionPlaceholders(int slotCount = 8) const;
    void provisionLooks(const QVector<Look> &looks) const;
    void renderLook(const Look &look,
                    bool makeProgram,
                    const QStringList &slotLabels = {}) const;

private:
    OBSClient *m_client = nullptr;
    Config m_config;
};
