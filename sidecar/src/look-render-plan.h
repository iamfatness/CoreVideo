#pragma once

#include "look.h"
#include <QStringList>

struct LookRenderConfig {
    QString sourcePattern;
    QString fallbackSceneName;
    double canvasWidth = 1920.0;
    double canvasHeight = 1080.0;

    void normalizeBroadcastCanvas();
};

struct LookRenderPlan {
    QString sceneName;
    QStringList sourceNames;
    QStringList designLayerNames;
    QStringList slotLabels;
    QVector<Overlay> overlays;
    QString backgroundImagePath;
    TileStyle tileStyle;
    LayoutTemplate tmpl;
    double canvasWidth = 1920.0;
    double canvasHeight = 1080.0;
    bool makeProgram = false;
    bool valid = false;
    bool hasBackgroundImage() const { return !backgroundImagePath.trimmed().isEmpty(); }
};

QStringList sourceNamesForSlots(const LookRenderConfig &config, int slotCount);
QStringList sourceNamesForLook(const LookRenderConfig &config, const Look &look);
QString sceneNameForLook(const LookRenderConfig &config, const Look &look);
QStringList designLayerSourceNames(const LookRenderConfig &config, const Look &look);
QStringList slotLabelsForLook(const Look &look, const QStringList &slotLabels = {});
LookRenderPlan renderPlanForLook(const LookRenderConfig &config,
                                 const Look &look,
                                 bool makeProgram,
                                 const QStringList &slotLabels = {});
