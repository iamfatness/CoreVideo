#include "obs-look-renderer.h"
#include <QRegularExpression>
#include <utility>

OBSLookRenderer::OBSLookRenderer(OBSClient *client, Config config)
    : m_client(client), m_config(std::move(config))
{
    m_config.normalizeBroadcastCanvas();
}

QStringList OBSLookRenderer::sourceNamesForSlots(int slotCount) const
{
    QStringList sources;
    for (int i = 0; i < slotCount; ++i)
        sources << m_config.sourcePattern.arg(i + 1);
    return sources;
}

QStringList OBSLookRenderer::sourceNamesForLook(const Look &look) const
{
    if (look.templateId == QStringLiteral("speaker-screenshare")
        || look.tmpl.id == QStringLiteral("speaker-screenshare")) {
        return {
            m_config.sourcePattern.arg(1),
            QStringLiteral("Zoom Screen Share"),
        };
    }

    return sourceNamesForSlots(look.tmpl.slotList.size());
}

QString OBSLookRenderer::sceneNameForLook(const Look &look) const
{
    QString base = look.name.trimmed();
    if (base.isEmpty()) base = look.tmpl.name.trimmed();
    if (base.isEmpty()) base = m_config.fallbackSceneName.trimmed();
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    base.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return QStringLiteral("CoreVideo - %1").arg(base.left(64));
}

QStringList OBSLookRenderer::sceneNamesForLooks(const QVector<Look> &looks) const
{
    QStringList scenes;
    for (const auto &look : looks) {
        if (!look.tmpl.isValid())
            continue;
        scenes << sceneNameForLook(look);
    }
    scenes.removeDuplicates();
    return scenes;
}

static QString safeDesignSceneName(const QString &sceneName, int maxLen)
{
    QString safe = sceneName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("-"));
    safe.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return safe.left(maxLen);
}

QStringList OBSLookRenderer::designLayerSourceNames(const Look &look) const
{
    const QString scene = sceneNameForLook(look);
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

void OBSLookRenderer::provisionPlaceholders(int slotCount) const
{
    if (!m_client || !m_client->isConnected())
        return;
    m_client->ensureCoreVideoSources(QStringLiteral("CoreVideo Sources"),
                                     sourceNamesForSlots(slotCount));
}

void OBSLookRenderer::provisionLooks(const QVector<Look> &looks) const
{
    if (!m_client || !m_client->isConnected())
        return;

    provisionPlaceholders(8);
    for (const auto &look : looks) {
        if (!look.tmpl.isValid())
            continue;
        m_client->loadSceneTemplate(sceneNameForLook(look),
                                    look.tmpl,
                                    sourceNamesForLook(look),
                                    m_config.canvasWidth,
                                    m_config.canvasHeight,
                                    look.overlays,
                                    look.backgroundImagePath,
                                    look.tileStyle,
                                    {},
                                    false);
    }
}

void OBSLookRenderer::renderLook(const Look &look,
                                 bool makeProgram,
                                 const QStringList &slotLabels) const
{
    if (!m_client || !m_client->isConnected() || !look.tmpl.isValid())
        return;

    m_client->loadSceneTemplate(sceneNameForLook(look),
                                look.tmpl,
                                sourceNamesForLook(look),
                                m_config.canvasWidth,
                                m_config.canvasHeight,
                                look.overlays,
                                look.backgroundImagePath,
                                look.tileStyle,
                                slotLabels,
                                makeProgram);
    if (!makeProgram)
        m_client->setCurrentPreviewScene(sceneNameForLook(look));
}
