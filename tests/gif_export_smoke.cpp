#include "GifExporter.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    rocket::GifExporter exporter;
    rocket::GifExportSettings settings;
    settings.playbackFps = 8;
    settings.durationSeconds = 1;
    settings.solverStepsPerFrame = 12;
    settings.warmupSteps = 200;
    settings.fieldMask = (1u << static_cast<uint32_t>(rocket::FieldView::Schlieren)) |
                         (1u << static_cast<uint32_t>(rocket::FieldView::Mach));
    if (!exporter.start(rocket::Parameters{}, settings)) return 1;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    rocket::GifExportStatus status;
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        status = exporter.status();
    } while (status.running && std::chrono::steady_clock::now() < deadline);

    bool filesValid = status.outputPaths.size() == 2;
    for (const std::string& path : status.outputPaths)
        filesValid &= std::filesystem::exists(path) && std::filesystem::file_size(path) > 100;
    const bool valid = !status.running && status.progress >= 1.0f && filesValid;
    std::cout << status.message << " outputs=" << status.outputPaths.size() << " valid=" << valid << '\n';
    return valid ? 0 : 1;
}
