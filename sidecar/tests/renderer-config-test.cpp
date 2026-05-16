#include "obs-look-renderer.h"

#include <QtGlobal>

static int fail()
{
    return 1;
}

int main()
{
    OBSLookRenderer::Config portrait;
    portrait.canvasWidth = 1080.0;
    portrait.canvasHeight = 1920.0;
    portrait.normalizeBroadcastCanvas();
    if (!qFuzzyCompare(portrait.canvasWidth, 1920.0)
        || !qFuzzyCompare(portrait.canvasHeight, 1080.0)) {
        return fail();
    }

    OBSLookRenderer::Config invalid;
    invalid.canvasWidth = 0.0;
    invalid.canvasHeight = -1.0;
    invalid.normalizeBroadcastCanvas();
    if (!qFuzzyCompare(invalid.canvasWidth, 1920.0)
        || !qFuzzyCompare(invalid.canvasHeight, 1080.0)) {
        return fail();
    }

    return 0;
}
