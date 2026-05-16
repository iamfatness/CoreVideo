#include "look-render-plan.h"

#include <QRegularExpression>
#include <utility>

void LookRenderConfig::normalizeBroadcastCanvas()
{
    if (canvasWidth <= 0.0)
        canvasWidth = 1920.0;
    if (canvasHeight <= 0.0)
        canvasHeight = 1080.0;
    if (canvasHeight > canvasWidth)
        std::swap(canvasWidth, canvasHeight);
}

QStringList sourceNamesForSlots(const LookRenderConfig &config, int slotCount)
{
    QStringList sources;
    for (int i = 0; i < slotCount; ++i)
        sources << config.sourcePattern.arg(i + 1);
    return sources;
}

QStringList sourceNamesForLook(const LookRenderConfig &config, const Look &look)
{
    if (look.templateId == QStringLiteral("speaker-screenshare")
        || look.tmpl.id == QStringLiteral("speaker-screenshare")) {
        return {
            config.sourcePattern.arg(1),
            QStringLiteral("Zoom Screen Share"),
        };
    }

    return sourceNamesForSlots(config, look.tmpl.slotList.size());
}

QString sceneNameForLook(const LookRenderConfig &config, const Look &look)
{
    QString base = look.name.trimmed();
    if (base.isEmpty()) base = look.tmpl.name.trimmed();
    if (base.isEmpty()) base = config.fallbackSceneName.trimmed();
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    base.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo - %1").arg(base.left(64));
}

static QString safeDesignSceneName(const QString &sceneName, int maxLen)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return safe.left(maxLen);
}

QStringList designLayerSourceNames(const LookRenderConfig &config, const Look &look)
{
    const QString scene = sceneNameForLook(config, look);
    QStringList names;
    names << QStringLiteral("CoreVideo Canvas - %1").arg(safeDesignSceneName(scene, 72));
    if (!look.backgroundImagePath.trimmed().isEmpty())
        names << QStringLiteral("CoreVideo Background - %1").arg(safeDesignSceneName(scene, 72));

    const bool showBorder = look.tileStyle.borderWidth > 0.0;
    const bool showShadow = look.tileStyle.dropShadow;
    const bool showDim = look.tileStyle.opacity < 0.99;
    for (const auto &slot : look.tmpl.slotList) {
        if (showShadow) {
            names << QStringLiteral("CoreVideo Shadow - %1 - Slot %2")
                         .arg(safeDesignSceneName(scene, 58))
                         .arg(slot.index + 1);
        }
        if (showBorder) {
            names << QStringLiteral("CoreVideo Border - %1 - Slot %2")
                         .arg(safeDesignSceneName(scene, 58))
                         .arg(slot.index + 1);
        }
        if (showDim) {
            names << QStringLiteral("CoreVideo Dim - %1 - Slot %2")
                         .arg(safeDesignSceneName(scene, 61))
                         .arg(slot.index + 1);
        }
        if (look.tileStyle.showNameTag) {
            names << QStringLiteral("CoreVideo Name - %1 - Slot %2")
                         .arg(safeDesignSceneName(scene, 60))
                         .arg(slot.index + 1);
        }
    }
    names.removeDuplicates();
    return names;
}

QStringList slotLabelsForLook(const Look &look, const QStringList &slotLabels)
{
    QStringList labels;
    labels.reserve(look.tmpl.slotList.size());
    for (const auto &slot : look.tmpl.slotList) {
        if (slot.index >= 0 && slot.index < slotLabels.size() && !slotLabels.value(slot.index).trimmed().isEmpty()) {
            labels << slotLabels.value(slot.index);
        } else if (!slot.label.trimmed().isEmpty()) {
            labels << slot.label.trimmed();
        } else {
            labels << QStringLiteral("Slot %1").arg(slot.index + 1);
        }
    }
    return labels;
}

LookRenderPlan renderPlanForLook(const LookRenderConfig &config,
                                 const Look &look,
                                 bool makeProgram,
                                 const QStringList &slotLabels)
{
    LookRenderConfig normalized = config;
    normalized.normalizeBroadcastCanvas();

    LookRenderPlan plan;
    if (!look.tmpl.isValid())
        return plan;

    plan.sceneName = sceneNameForLook(normalized, look);
    plan.sourceNames = sourceNamesForLook(normalized, look);
    plan.designLayerNames = designLayerSourceNames(normalized, look);
    plan.slotLabels = slotLabelsForLook(look, slotLabels);
    plan.overlays = look.overlays;
    plan.backgroundImagePath = look.backgroundImagePath;
    plan.tileStyle = look.tileStyle;
    plan.tmpl = look.tmpl;
    plan.canvasWidth = normalized.canvasWidth;
    plan.canvasHeight = normalized.canvasHeight;
    plan.makeProgram = makeProgram;
    plan.valid = true;
    return plan;
}
