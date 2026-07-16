#include "BakeManager.h"
#include "GifExporter.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

template <typename StatusFn>
auto waitFor(StatusFn statusFn) {
    auto status = statusFn();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    while (status.running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        status = statusFn();
    }
    return status;
}

int main() {
    rocket::BakeManager baker;
    rocket::BakeSettings bakeSettings;
    bakeSettings.totalSteps = 100;
    bakeSettings.captureEverySteps = 50;
    bakeSettings.resolutionScale = 1;
    bakeSettings.fieldMask = (1u << static_cast<uint32_t>(rocket::FieldView::Mach)) |
                             (1u << static_cast<uint32_t>(rocket::FieldView::Pressure));
    if (!baker.start(rocket::Parameters{}, bakeSettings)) return 1;
    const rocket::BakeStatus bake = waitFor([&] { return baker.status(); });
    bool bakeValid = !bake.running && bake.progress >= 1.0f && bake.result &&
                     bake.result->frameCount == 2 && bake.result->fields.size() == 2;
    if (bake.result) {
        for (const rocket::BakedField& field : bake.result->fields) {
            bakeValid &= std::filesystem::exists(field.videoPath) &&
                         std::filesystem::file_size(field.videoPath) > 100 &&
                         field.previewRgb.size() == static_cast<size_t>(2 * bake.result->previewWidth *
                                                                        bake.result->previewHeight * 3);
        }
    }

    rocket::GifExporter exporter;
    rocket::GifExportSettings gifSettings;
    gifSettings.playbackFps = 12;
    gifSettings.fieldMask = bakeSettings.fieldMask;
    if (!bakeValid || !exporter.startFromBake(bake.result, gifSettings, 0, 1)) return 1;
    const rocket::GifExportStatus exported = waitFor([&] { return exporter.status(); });
    bool exportValid = !exported.running && exported.progress >= 1.0f && exported.outputPaths.size() == 2;
    for (const std::string& path : exported.outputPaths)
        exportValid &= std::filesystem::exists(path) && std::filesystem::file_size(path) > 100;

    std::cout << bake.message << " frames=" << (bake.result ? bake.result->frameCount : 0)
              << " bakedExport=" << exported.message << " valid=" << (bakeValid && exportValid) << '\n';
    return bakeValid && exportValid ? 0 : 1;
}
