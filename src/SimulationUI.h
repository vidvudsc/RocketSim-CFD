#pragma once

#include "FlowSolver.h"
#include "GifExporter.h"
#include "BakeManager.h"
#include "imgui.h"

#include <vector>

namespace rocket {

struct UiActions {
    bool reset = false;
    bool singleStep = false;
    bool geometryChanged = false;
    bool developPlume = false;
    bool exportGif = false;
    bool startBake = false;
    bool exportBakedGif = false;
    bool recoverBake = false;
    GifExportSettings gifSettings{};
    BakeSettings bakeSettings{};
    int bakedStartFrame = 0;
    int bakedEndFrame = 0;
    int historyStartFrame = 0;
    int historyEndFrame = 0;
    int recoveryCaptureEverySteps = 40;
};

void applyTheme(float scale);
UiActions drawSimulationUi(FlowSolver& solver, Parameters& parameters, FieldView& view,
                           const GifExportStatus& exportStatus, const BakeStatus& bakeStatus,
                           ImTextureID liveFieldTexture, const std::vector<int>& liveHistorySteps);

} // namespace rocket
