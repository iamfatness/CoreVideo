#include "obs-look-renderer.h"
#include <utility>

static LookRenderConfig toRenderConfig(const OBSLookRenderer::Config &config)
{
    LookRenderConfig c;
    c.sourcePattern = config.sourcePattern;
    c.fallbackSceneName = config.fallbackSceneName;
    c.canvasWidth = config.canvasWidth;
    c.canvasHeight = config.canvasHeight;
    c.normalizeBroadcastCanvas();
    return c;
}

OBSLookRenderer::OBSLookRenderer(OBSClient *client, Config config)
    : m_client(client), m_config(std::move(config))
{
    m_config.normalizeBroadcastCanvas();
}

QStringList OBSLookRenderer::sourceNamesForSlots(int slotCount) const
{
    return ::sourceNamesForSlots(toRenderConfig(m_config), slotCount);
}

QStringList OBSLookRenderer::sourceNamesForLook(const Look &look) const
{
    return ::sourceNamesForLook(toRenderConfig(m_config), look);
}

QString OBSLookRenderer::sceneNameForLook(const Look &look) const
{
    return ::sceneNameForLook(toRenderConfig(m_config), look);
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

QStringList OBSLookRenderer::designLayerSourceNames(const Look &look) const
{
    return ::designLayerSourceNames(toRenderConfig(m_config), look);
}

QStringList OBSLookRenderer::slotLabelsForLook(const Look &look, const QStringList &slotLabels) const
{
    return ::slotLabelsForLook(look, slotLabels);
}

OBSLookRenderer::RenderPlan OBSLookRenderer::renderPlanForLook(const Look &look,
                                                               bool makeProgram,
                                                               const QStringList &slotLabels) const
{
    return ::renderPlanForLook(toRenderConfig(m_config), look, makeProgram, slotLabels);
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
        const RenderPlan plan = renderPlanForLook(look, false);
        if (!plan.valid)
            continue;
        m_client->loadSceneTemplate(plan.sceneName,
                                    plan.tmpl,
                                    plan.sourceNames,
                                    plan.canvasWidth,
                                    plan.canvasHeight,
                                    plan.overlays,
                                    plan.backgroundImagePath,
                                    plan.tileStyle,
                                    plan.slotLabels,
                                    false);
    }
}

void OBSLookRenderer::renderLook(const Look &look,
                                 bool makeProgram,
                                 const QStringList &slotLabels) const
{
    if (!m_client || !m_client->isConnected() || !look.tmpl.isValid())
        return;

    const RenderPlan plan = renderPlanForLook(look, makeProgram, slotLabels);
    if (!plan.valid)
        return;

    m_client->loadSceneTemplate(plan.sceneName,
                                plan.tmpl,
                                plan.sourceNames,
                                plan.canvasWidth,
                                plan.canvasHeight,
                                plan.overlays,
                                plan.backgroundImagePath,
                                plan.tileStyle,
                                plan.slotLabels,
                                plan.makeProgram);
    if (!makeProgram)
        m_client->setCurrentPreviewScene(plan.sceneName);
}
