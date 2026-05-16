#include "look-render-plan.h"

#include <iostream>
#include <QtGlobal>

static int fail(const char *message)
{
    std::cerr << message << "\n";
    return 1;
}

int main()
{
    LookRenderConfig portrait;
    portrait.canvasWidth = 1080.0;
    portrait.canvasHeight = 1920.0;
    portrait.normalizeBroadcastCanvas();
    if (!qFuzzyCompare(portrait.canvasWidth, 1920.0)
        || !qFuzzyCompare(portrait.canvasHeight, 1080.0)) {
        return fail("portrait canvas was not normalized to 16:9 landscape");
    }

    LookRenderConfig invalid;
    invalid.canvasWidth = 0.0;
    invalid.canvasHeight = -1.0;
    invalid.normalizeBroadcastCanvas();
    if (!qFuzzyCompare(invalid.canvasWidth, 1920.0)
        || !qFuzzyCompare(invalid.canvasHeight, 1080.0)) {
        return fail("invalid canvas was not normalized to 1920x1080");
    }

    LookRenderConfig cfg;
    cfg.sourcePattern = "Zoom Participant %1";
    cfg.fallbackSceneName = "CoreVideo Main";

    Look grid;
    grid.name = "Panel / Talk: Show?";
    grid.templateId = "4-up-grid";
    grid.tmpl.id = "4-up-grid";
    grid.tmpl.name = "4-up-grid";
    for (int i = 0; i < 4; ++i)
        grid.tmpl.slotList.append({i, (i % 2) * 0.5, (i / 2) * 0.5, 0.5, 0.5, QString("Guest %1").arg(i + 1), false});
    grid.tileStyle.borderWidth = 3.0;
    grid.tileStyle.dropShadow = true;
    grid.tileStyle.opacity = 0.75;
    grid.tileStyle.showNameTag = true;
    grid.backgroundImagePath = "C:/show/background.png";
    Overlay liveBug;
    liveBug.id = "bug-live";
    liveBug.type = Overlay::Bug;
    liveBug.text1 = "LIVE";
    liveBug.x = 0.9;
    liveBug.y = 0.04;
    liveBug.w = 0.08;
    liveBug.h = 0.04;
    grid.overlays.append(liveBug);

    const auto gridPlan = renderPlanForLook(cfg, grid, true, {"Alex", "Sam"});
    if (!gridPlan.valid)
        return fail("grid render plan was invalid");
    if (gridPlan.sceneName != "CoreVideo - Panel - Talk- Show-")
        return fail("scene name was not OBS-safe and deterministic");
    if (gridPlan.sourceNames != QStringList({"Zoom Participant 1", "Zoom Participant 2", "Zoom Participant 3", "Zoom Participant 4"}))
        return fail("grid source names did not match slot count");
    if (gridPlan.slotLabels != QStringList({"Alex", "Sam", "Guest 3", "Guest 4"}))
        return fail("slot labels did not prefer participant labels then template labels");
    if (!gridPlan.makeProgram)
        return fail("program intent was not preserved");
    if (!gridPlan.hasBackgroundImage())
        return fail("background image was not carried into render plan");
    if (!gridPlan.designLayerNames.contains("CoreVideo Canvas - CoreVideo - Panel - Talk- Show-")
        || !gridPlan.designLayerNames.contains("CoreVideo Background - CoreVideo - Panel - Talk- Show-")
        || !gridPlan.designLayerNames.contains("CoreVideo Shadow - CoreVideo - Panel - Talk- Show- - Slot 1")
        || !gridPlan.designLayerNames.contains("CoreVideo Border - CoreVideo - Panel - Talk- Show- - Slot 1")
        || !gridPlan.designLayerNames.contains("CoreVideo Dim - CoreVideo - Panel - Talk- Show- - Slot 1")
        || !gridPlan.designLayerNames.contains("CoreVideo Name - CoreVideo - Panel - Talk- Show- - Slot 1")) {
        return fail("design layers do not reflect enabled tile styling");
    }

    Look share;
    share.name = "Speaker and Share";
    share.templateId = "speaker-screenshare";
    share.tmpl.id = "speaker-screenshare";
    share.tmpl.name = "Speaker and Share";
    share.tmpl.slotList.append({0, 0.0, 0.0, 0.35, 1.0, "Speaker", false});
    share.tmpl.slotList.append({1, 0.35, 0.0, 0.65, 1.0, "Share", true});
    const auto sharePlan = renderPlanForLook(cfg, share, false);
    if (sharePlan.sourceNames != QStringList({"Zoom Participant 1", "Zoom Screen Share"}))
        return fail("speaker screenshare look did not map to participant plus share sources");
    if (sharePlan.makeProgram)
        return fail("preview intent was not preserved");

    return 0;
}
