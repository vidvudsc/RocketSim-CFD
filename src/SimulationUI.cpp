#include "SimulationUI.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace rocket {
namespace {
void metric(const char* label, const char* value, ImVec4 color = ImVec4(0.76f, 0.84f, 0.94f, 1.0f)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(color, "%s", value);
}

bool rangeSliderInt(const char* label, int* first, int* last, int maximum) {
    maximum = std::max(maximum, 0);
    *first = std::clamp(*first, 0, maximum);
    *last = std::clamp(*last, *first, maximum);
    ImGui::TextUnformatted(label);
    const ImGuiID id = ImGui::GetID(label);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
    constexpr float height = 24.0f;
    ImGui::PushID(label);
    ImGui::InvisibleButton("##range", ImVec2(width, height));
    ImGui::PopID();

    const float left = origin.x + 10.0f;
    const float right = origin.x + width - 10.0f;
    const float centerY = origin.y + height * 0.5f;
    auto position = [&](int value) {
        return left + (right - left) * static_cast<float>(value) / std::max(maximum, 1);
    };
    static ImGuiID activeRange = 0;
    static bool draggingLast = false;
    if (ImGui::IsItemClicked()) {
        activeRange = id;
        draggingLast = std::abs(ImGui::GetIO().MousePos.x - position(*last)) <
                       std::abs(ImGui::GetIO().MousePos.x - position(*first));
    }

    bool changed = false;
    if (activeRange == id && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - left) / std::max(right - left, 1.0f), 0.0f, 1.0f);
        const int value = static_cast<int>(std::round(t * maximum));
        if (draggingLast) {
            const int next = std::max(value, *first);
            changed |= next != *last;
            *last = next;
        } else {
            const int next = std::min(value, *last);
            changed |= next != *first;
            *first = next;
        }
    }
    if (activeRange == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) activeRange = 0;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(left, centerY), ImVec2(right, centerY),
                  ImGui::GetColorU32(ImGuiCol_FrameBg), 7.0f);
    draw->AddLine(ImVec2(position(*first), centerY), ImVec2(position(*last), centerY),
                  ImGui::GetColorU32(ImGuiCol_SliderGrab), 7.0f);
    draw->AddCircleFilled(ImVec2(position(*first), centerY), 7.0f,
                          ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    draw->AddCircleFilled(ImVec2(position(*last), centerY), 7.0f,
                          ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    ImGui::Text("Frames %d - %d", *first, *last);
    return changed;
}
} // namespace

void applyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(9.0f, 9.0f);
    style.ScrollbarSize = 12.0f;
    style.ScaleAllSizes(scale);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.53f, 0.64f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.025f, 0.032f, 0.046f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.037f, 0.048f, 0.069f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.05f, 0.075f, 0.99f);
    c[ImGuiCol_Border] = ImVec4(0.13f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.055f, 0.075f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.08f, 0.13f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.18f, 0.29f, 1.00f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_CheckMark] = ImVec4(0.24f, 0.74f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.58f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.32f, 0.76f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.08f, 0.18f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.12f, 0.28f, 0.44f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.38f, 0.61f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.07f, 0.14f, 0.23f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.10f, 0.23f, 0.37f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.12f, 0.30f, 0.49f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.11f, 0.19f, 0.28f, 1.00f);
}

UiActions drawSimulationUi(FlowSolver& solver, Parameters& p, FieldView& view,
                           const GifExportStatus& exportStatus, const BakeStatus& bakeStatus,
                           ImTextureID liveFieldTexture, const std::vector<int>& liveHistorySteps) {
    UiActions actions;
    static GifExportSettings gifSettings;
    static BakeSettings bakeSettings;
    static uint32_t outputFieldMask = (1u << static_cast<uint32_t>(FieldView::Schlieren)) |
                                      (1u << static_cast<uint32_t>(FieldView::Mach));
    static int recoveryCaptureEverySteps = 40;
    static bool showBakedTimeline = false;
    static int bakedFrame = 0;
    static int bakedStartFrame = 0;
    static int bakedEndFrame = 0;
    static int historyStartFrame = 0;
    static int historyEndFrame = 0;
    static int firstHistoryStep = -1;
    static int historySize = 0;
    static FieldView bakedView = FieldView::Mach;
    static const BakeResult* lastBake = nullptr;
    static const char* fieldLabels[] = {"Schlieren", "Temperature", "Mach number", "Pressure", "Density", "Exhaust fraction", "Velocity"};
    if (!liveHistorySteps.empty()) {
        const int newSize = static_cast<int>(liveHistorySteps.size());
        if (firstHistoryStep != liveHistorySteps.front()) {
            firstHistoryStep = liveHistorySteps.front();
            historyStartFrame = 0;
            historyEndFrame = newSize - 1;
        } else if (newSize != historySize && historyEndFrame >= historySize - 1) {
            historyEndFrame = newSize - 1;
        }
        historySize = newSize;
    }
    if (bakeStatus.result && bakeStatus.result.get() != lastBake) {
        lastBake = bakeStatus.result.get();
        bakedFrame = 0;
        bakedStartFrame = 0;
        bakedEndFrame = std::max(0, bakeStatus.result->frameCount - 1);
        if (!bakeStatus.result->fields.empty()) bakedView = bakeStatus.result->fields.front().view;
        showBakedTimeline = true;
    }
    const BakedField* bakedField = bakeStatus.result ? bakeStatus.result->findField(bakedView) : nullptr;
    if (!bakedField && bakeStatus.result && !bakeStatus.result->fields.empty()) {
        bakedView = bakeStatus.result->fields.front().view;
        bakedField = &bakeStatus.result->fields.front();
    }
    const bool displayingBake = showBakedTimeline && bakeStatus.result && bakedField;
    const Parameters& displayParameters = displayingBake ? bakeStatus.result->parameters : p;
    const float equivalentGifFrame = static_cast<float>(solver.diagnostics().iteration) /
                                     std::max(gifSettings.solverStepsPerFrame, 1);
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float panelWidth = std::clamp(viewport->Size.x * 0.29f, 360.0f, 460.0f);
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x - panelWidth, viewport->Size.y));
    ImGui::Begin("Flow field", nullptr, fixed | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(ImVec4(0.33f, 0.77f, 1.0f, 1.0f), "ROCKETSIM / EULER FLOW LAB");
    ImGui::SameLine();
    ImGui::TextDisabled("  deterministic finite-volume field  |  Vulkan");
    if (displayingBake) {
        const int cfdStep = bakeStatus.result->cfdSteps[static_cast<size_t>(bakedFrame)];
        ImGui::TextColored(ImVec4(0.58f, 0.86f, 1.0f, 1.0f),
                           "BAKED FRAME %d / %d   |   CFD STEP %d   |   mouse wheel scrubs timeline",
                           bakedFrame + 1, bakeStatus.result->frameCount, cfdStep);
    } else {
        ImGui::TextColored(ImVec4(0.58f, 0.86f, 1.0f, 1.0f),
                           "CFD STEP %d   |   GIF FRAME %.1f @ %d steps/frame",
                           solver.diagnostics().iteration, equivalentGifFrame, gifSettings.solverStepsPerFrame);
    }
    ImGui::Separator();

    const ImVec2 availablePos = ImGui::GetCursorScreenPos();
    const ImVec2 availableSize = ImGui::GetContentRegionAvail();
    const float worldWidth = solver.worldXMax() - solver.worldXMin();
    const float worldHeight = solver.worldYMax() - solver.worldYMin();
    const float pixelsPerMeter = std::min(availableSize.x / worldWidth, availableSize.y / worldHeight);
    const ImVec2 canvasSize(worldWidth * pixelsPerMeter, worldHeight * pixelsPerMeter);
    const ImVec2 canvasPos(availablePos.x + 0.5f * (availableSize.x - canvasSize.x),
                           availablePos.y + 0.5f * (availableSize.y - canvasSize.y));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(availablePos, ImVec2(availablePos.x + availableSize.x, availablePos.y + availableSize.y), IM_COL32(3, 6, 11, 255));
    draw->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(4, 7, 12, 255));

    const float cw = canvasSize.x / static_cast<float>(solver.width());
    if (displayingBake) {
        const int pw = bakeStatus.result->previewWidth;
        const int ph = bakeStatus.result->previewHeight;
        const float pcw = canvasSize.x / pw;
        const float pch = canvasSize.y / ph;
        const size_t frameOffset = static_cast<size_t>(bakedFrame) * pw * ph * 3;
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const size_t offset = frameOffset + static_cast<size_t>((y * pw + x) * 3);
                const ImVec2 a(canvasPos.x + x * pcw, canvasPos.y + y * pch);
                const ImVec2 b(a.x + pcw + 0.7f, a.y + pch + 0.7f);
                draw->AddRectFilled(a, b, IM_COL32(bakedField->previewRgb[offset],
                                                   bakedField->previewRgb[offset + 1],
                                                   bakedField->previewRgb[offset + 2], 255));
            }
        }
    } else {
        draw->AddImage(ImTextureRef(liveFieldTexture), canvasPos,
                       ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y));
    }

    // Draw the physical nozzle wall over the finite-volume field.
    ImVec2 previousTop{};
    ImVec2 previousBottom{};
    ImVec2 previousTopOuter{};
    ImVec2 previousBottomOuter{};
    bool hasPrevious = false;
    for (int x = 0; x < solver.width(); ++x) {
        const float radius = solver.nozzleRadius(solver.worldX(x), displayParameters);
        if (radius <= 0.0f) { hasPrevious = false; continue; }
        constexpr float visualWallThicknessM = 0.030f;
        const float topY = (1.0f - (radius - solver.worldYMin()) / worldHeight) * canvasSize.y;
        const float bottomY = (1.0f - (-radius - solver.worldYMin()) / worldHeight) * canvasSize.y;
        const float topOuterY = (1.0f - (radius + visualWallThicknessM - solver.worldYMin()) / worldHeight) * canvasSize.y;
        const float bottomOuterY = (1.0f - (-radius - visualWallThicknessM - solver.worldYMin()) / worldHeight) * canvasSize.y;
        const ImVec2 top(canvasPos.x + (x + 0.5f) * cw, canvasPos.y + topY);
        const ImVec2 bottom(canvasPos.x + (x + 0.5f) * cw, canvasPos.y + bottomY);
        const ImVec2 topOuter(top.x, canvasPos.y + topOuterY);
        const ImVec2 bottomOuter(bottom.x, canvasPos.y + bottomOuterY);
        if (hasPrevious) {
            draw->AddQuadFilled(previousTopOuter, topOuter, top, previousTop, IM_COL32(125, 143, 166, 255));
            draw->AddQuadFilled(previousBottom, bottom, bottomOuter, previousBottomOuter, IM_COL32(125, 143, 166, 255));
            draw->AddLine(previousTop, top, IM_COL32(208, 221, 236, 255), 2.2f);
            draw->AddLine(previousBottom, bottom, IM_COL32(208, 221, 236, 255), 2.2f);
        }
        previousTop = top;
        previousBottom = bottom;
        previousTopOuter = topOuter;
        previousBottomOuter = bottomOuter;
        hasPrevious = true;
    }
    draw->AddLine(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y * 0.5f),
                  ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y * 0.5f),
                  IM_COL32(130, 160, 190, 30), 1.0f);

    // Sealed chamber head. The maintained reservoir is a solver condition,
    // not a decorative injector overlay.
    const float chamberStart = -0.10f - displayParameters.chamberLengthM;
    const float capHalfWidth = 0.025f / worldWidth * canvasSize.x;
    const float capHalfHeight = (displayParameters.chamberRadiusM + 0.030f) / worldHeight * canvasSize.y;
    const float capX = canvasPos.x + (chamberStart - solver.worldXMin()) / worldWidth * canvasSize.x;
    draw->AddRectFilled(ImVec2(capX - capHalfWidth, canvasPos.y + 0.5f * canvasSize.y - capHalfHeight),
                        ImVec2(capX + capHalfWidth, canvasPos.y + 0.5f * canvasSize.y + capHalfHeight),
                        IM_COL32(125, 143, 166, 255));
    ImGui::Dummy(availableSize);
    if (displayingBake && ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            bakedFrame = std::clamp(bakedFrame - static_cast<int>(std::copysign(1.0f, wheel)),
                                    0, bakeStatus.result->frameCount - 1);
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - panelWidth, viewport->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, viewport->Size.y));
    ImGui::Begin("Controls", nullptr, fixed);
    ImGui::TextColored(ImVec4(0.33f, 0.77f, 1.0f, 1.0f), "ENGINE CONTROL");
    ImGui::TextDisabled("Second-order HLLC compressible flow");
    ImGui::Separator();

    const Diagnostics& d = solver.diagnostics();
    if (ImGui::BeginTable("metrics", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        char text[64];
        snprintf(text, sizeof(text), "%.1f kN", d.thrustKN); metric("Estimated thrust", text, ImVec4(0.33f, 0.88f, 0.63f, 1.0f));
        snprintf(text, sizeof(text), "%.1f kg/s", d.massFlowKgPerS); metric("Mass flow", text);
        snprintf(text, sizeof(text), "%.1f s", d.specificImpulseS); metric("Estimated Isp", text);
        snprintf(text, sizeof(text), "%.2f", d.exitMach); metric("Exit Mach", text);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (solver.paused()) {
        if (ImGui::Button("Resume", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 3.0f, 0.0f))) solver.setPaused(false);
    } else {
        if (ImGui::Button("Pause", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 3.0f, 0.0f))) solver.setPaused(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(-1.0f, 0.0f))) actions.reset = true;
    ImGui::BeginDisabled(!solver.paused());
    if (ImGui::Button("Single step", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 3.0f, 0.0f))) actions.singleStep = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Advance 5,000", ImVec2(-1.0f, 0.0f))) actions.developPlume = true;

    ImGui::SeparatorText("View");
    int selected = static_cast<int>(view);
    if (ImGui::Combo("Field", &selected, fieldLabels, IM_ARRAYSIZE(fieldLabels))) view = static_cast<FieldView>(selected);

    if (ImGui::CollapsingHeader("Environment and solver")) {
        ImGui::SliderFloat("Ambient pressure", &p.ambientPressureKPa, 0.5f, 110.0f, "%.2f kPa", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Ambient temperature", &p.ambientTemperatureK, 180.0f, 330.0f, "%.0f K");
        ImGui::SliderFloat("CFL number", &p.cfl, 0.10f, 0.42f, "%.2f");
        ImGui::SliderFloat("Time scale", &p.timeScale, 0.05f, 3.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("Steps per screen update", &p.liveStepBudget, 1, 12);
        ImGui::TextDisabled("Grid %d x %d | t = %.3f ms | %.1f ms/frame", solver.width(), solver.height(),
                            d.simulationTimeMs, d.stepMilliseconds);
        ImGui::TextDisabled("This changes UI responsiveness, not bake or export fidelity.");
    }
    }

    if (ImGui::CollapsingHeader("Engine", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SeparatorText("Chamber gas");
        ImGui::SliderFloat("Pressure##chamber", &p.chamberPressureMPa, 1.0f, 25.0f, "%.2f MPa", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Temperature##chamber", &p.chamberTemperatureK, 1800.0f, 4200.0f, "%.0f K");
        ImGui::SliderFloat("Gas molar mass", &p.molarMassGPerMol, 12.0f, 35.0f, "%.1f g/mol");
        ImGui::SliderFloat("Heat capacity ratio", &p.gamma, 1.10f, 1.40f, "%.3f");
        ImGui::TextDisabled("Reset starts the chamber ambient; the head reservoir supplies hot gas.");

        ImGui::SeparatorText("Nozzle");
        actions.geometryChanged |= ImGui::SliderFloat("Chamber radius", &p.chamberRadiusM, 0.25f, 0.80f, "%.3f m");
        actions.geometryChanged |= ImGui::SliderFloat("Throat radius", &p.throatRadiusM, 0.06f, 0.28f, "%.3f m");
        actions.geometryChanged |= ImGui::SliderFloat("Exit radius", &p.exitRadiusM, 0.20f, 0.85f, "%.3f m");
        actions.geometryChanged |= ImGui::SliderFloat("Chamber length", &p.chamberLengthM, 0.30f, 0.90f, "%.3f m");
        actions.geometryChanged |= ImGui::SliderFloat("Converging length", &p.convergingLengthM, 0.18f, 0.70f, "%.3f m");
        actions.geometryChanged |= ImGui::SliderFloat("Diverging length", &p.divergingLengthM, 0.40f, 1.45f, "%.3f m");
        const float expansion = (p.exitRadiusM * p.exitRadiusM) / (p.throatRadiusM * p.throatRadiusM);
        ImGui::Text("Area expansion ratio  %.2f", expansion);
        ImGui::TextDisabled("Geometry edits reset the flow automatically.");
    }

    if (ImGui::CollapsingHeader("Output fields", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Each selected field produces its own synchronized GIF.");
        for (int field = 0; field < IM_ARRAYSIZE(fieldLabels); ++field) {
            bool enabled = (outputFieldMask & (1u << field)) != 0;
            if (ImGui::Checkbox((std::string(fieldLabels[field]) + "##output").c_str(), &enabled)) {
                if (enabled) outputFieldMask |= 1u << field;
                else outputFieldMask &= ~(1u << field);
            }
            if ((field % 2) == 0 && field + 1 < IM_ARRAYSIZE(fieldLabels)) ImGui::SameLine(190.0f);
        }
    }

    bakeSettings.fieldMask = outputFieldMask;
    gifSettings.fieldMask = outputFieldMask;

    if (ImGui::CollapsingHeader("Bake timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* resolutionLabels[] = {"1x  (768 x 288)", "2x  (1536 x 576)", "3x  (2304 x 864)"};
        int scaleIndex = bakeSettings.resolutionScale - 1;
        if (ImGui::Combo("Bake resolution", &scaleIndex, resolutionLabels, IM_ARRAYSIZE(resolutionLabels)))
            bakeSettings.resolutionScale = scaleIndex + 1;
        const int maximumBakeSteps = bakeSettings.resolutionScale == 3 ? 200000 : 100000;
        bakeSettings.totalSteps = std::min(bakeSettings.totalSteps, maximumBakeSteps);
        ImGui::SliderInt("Bake CFD steps", &bakeSettings.totalSteps, 500, maximumBakeSteps);
        ImGui::SliderInt("Capture interval", &bakeSettings.captureEverySteps, 5, 250, "%d steps/frame");
        const int estimatedFrames = (bakeSettings.totalSteps + bakeSettings.captureEverySteps - 1) /
                                    bakeSettings.captureEverySteps;
        int bakeFieldCount = 0;
        for (int field = 0; field < IM_ARRAYSIZE(fieldLabels); ++field)
            bakeFieldCount += (bakeSettings.fieldMask & (1u << field)) != 0;
        const double estimatedCacheGB = static_cast<double>(estimatedFrames) *
            (768 * bakeSettings.resolutionScale) * (288 * bakeSettings.resolutionScale) *
            3.0 * bakeFieldCount * 0.15 / (1024.0 * 1024.0 * 1024.0);
        ImGui::TextDisabled("%d cached frames; estimated %.2f GB plus safety reserve", estimatedFrames, estimatedCacheGB);
        if (bakeStatus.running) {
            ImGui::ProgressBar(bakeStatus.progress, ImVec2(-1.0f, 0.0f));
            const int eta = std::max(0, static_cast<int>(std::round(bakeStatus.etaSeconds)));
            ImGui::TextWrapped("%s", bakeStatus.message.c_str());
            ImGui::Text("ETA %02d:%02d", eta / 60, eta % 60);
        } else {
            ImGui::BeginDisabled(bakeSettings.fieldMask == 0 || exportStatus.running);
            if (ImGui::Button("Start reusable bake", ImVec2(-1.0f, 0.0f))) {
                actions.startBake = true;
                actions.bakeSettings = bakeSettings;
            }
            ImGui::EndDisabled();
            ImGui::InputInt("Recovery capture interval", &recoveryCaptureEverySteps, 1, 10);
            recoveryCaptureEverySteps = std::clamp(recoveryCaptureEverySteps, 1, 500);
            if (ImGui::Button("Recover latest interrupted bake", ImVec2(-1.0f, 0.0f))) {
                actions.recoverBake = true;
                actions.recoveryCaptureEverySteps = recoveryCaptureEverySteps;
            }
        }

        if (bakeStatus.result) {
            ImGui::SeparatorText("Baked timeline");
            ImGui::Checkbox("Show baked timeline", &showBakedTimeline);
            if (ImGui::BeginCombo("Baked field", fieldLabels[static_cast<int>(bakedView)])) {
                for (const BakedField& field : bakeStatus.result->fields) {
                    const bool selected = field.view == bakedView;
                    if (ImGui::Selectable(fieldLabels[static_cast<int>(field.view)], selected)) bakedView = field.view;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SliderInt("Timeline frame", &bakedFrame, 0, bakeStatus.result->frameCount - 1);
            bakedFrame = std::clamp(bakedFrame, 0, bakeStatus.result->frameCount - 1);
            ImGui::Text("Frame %d | CFD step %d", bakedFrame,
                        bakeStatus.result->cfdSteps[static_cast<size_t>(bakedFrame)]);
            rangeSliderInt("GIF export range", &bakedStartFrame, &bakedEndFrame,
                           bakeStatus.result->frameCount - 1);
            ImGui::TextDisabled("Mouse wheel over the viewport also scrubs frames.");
            ImGui::TextDisabled("Viewport proxy: %d x %d. GIF export uses the full lossless bake cache.",
                                bakeStatus.result->previewWidth, bakeStatus.result->previewHeight);
        }
    }

    if (ImGui::CollapsingHeader("GIF export")) {
        ImGui::SliderInt("Playback FPS", &gifSettings.playbackFps, 12, 60);
        ImGui::SliderInt("Motion speed", &gifSettings.solverStepsPerFrame, 4, 60, "%d CFD steps/frame");
        if (liveHistorySteps.size() >= 2) {
            rangeSliderInt("Recorded live range", &historyStartFrame, &historyEndFrame,
                           static_cast<int>(liveHistorySteps.size()) - 1);
            const int firstStep = liveHistorySteps[static_cast<size_t>(historyStartFrame)];
            const int lastStep = liveHistorySteps[static_cast<size_t>(historyEndFrame)];
            const int frames = std::max(1, (lastStep - firstStep + gifSettings.solverStepsPerFrame - 1) /
                                               gifSettings.solverStepsPerFrame + 1);
            ImGui::TextDisabled("CFD steps %d - %d | about %.2f seconds", firstStep, lastStep,
                                static_cast<float>(frames) / gifSettings.playbackFps);
            ImGui::TextDisabled("Rolling history: %d checkpoints spanning %d CFD steps",
                                static_cast<int>(liveHistorySteps.size()),
                                liveHistorySteps.back() - liveHistorySteps.front());
        } else {
            ImGui::TextDisabled("Run the simulation to build a rolling past timeline.");
        }
        ImGui::TextDisabled("The replay runs on a copy; the live simulation is unchanged.");
        if (exportStatus.running) {
            ImGui::ProgressBar(exportStatus.progress, ImVec2(-1.0f, 0.0f));
            ImGui::TextWrapped("%s", exportStatus.message.c_str());
        } else {
            ImGui::BeginDisabled(gifSettings.fieldMask == 0 || liveHistorySteps.size() < 2 ||
                                 historyStartFrame >= historyEndFrame);
            if (ImGui::Button("Export selected past range", ImVec2(-1.0f, 0.0f))) {
                actions.exportGif = true;
                actions.gifSettings = gifSettings;
                actions.historyStartFrame = historyStartFrame;
                actions.historyEndFrame = historyEndFrame;
            }
            ImGui::EndDisabled();
            if (bakeStatus.result) {
                uint32_t availableMask = 0;
                for (const BakedField& field : bakeStatus.result->fields)
                    availableMask |= 1u << static_cast<uint32_t>(field.view);
                ImGui::BeginDisabled((gifSettings.fieldMask & availableMask) == 0);
                if (ImGui::Button("Export selected baked timeline range", ImVec2(-1.0f, 0.0f))) {
                    actions.exportBakedGif = true;
                    actions.gifSettings = gifSettings;
                    actions.bakedStartFrame = bakedStartFrame;
                    actions.bakedEndFrame = bakedEndFrame;
                }
                ImGui::EndDisabled();
                const int rangeFrames = bakedEndFrame - bakedStartFrame + 1;
                ImGui::TextDisabled("Baked range: %d frames, %.2f s at %d FPS (no CFD rebake)",
                                    rangeFrames, static_cast<float>(rangeFrames) / gifSettings.playbackFps,
                                    gifSettings.playbackFps);
            }
            if (!exportStatus.outputPaths.empty()) {
                ImGui::TextWrapped("%s", exportStatus.message.c_str());
                for (const std::string& path : exportStatus.outputPaths)
                    ImGui::TextDisabled("%s", path.c_str());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("%s", solver.status().c_str());
    ImGui::TextDisabled("Axisymmetric inviscid frozen-gamma model. Exploratory, not design certification.");
    ImGui::End();
    return actions;
}

} // namespace rocket
