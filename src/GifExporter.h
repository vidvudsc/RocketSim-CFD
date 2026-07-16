#pragma once

#include "FlowSolver.h"
#include "BakeManager.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rocket {

struct GifExportSettings {
    int playbackFps = 30;
    int durationSeconds = 4;
    int solverStepsPerFrame = 20;
    int warmupSteps = 8000;
    uint32_t fieldMask = (1u << static_cast<uint32_t>(FieldView::Schlieren)) |
                         (1u << static_cast<uint32_t>(FieldView::Mach));
};

struct GifExportStatus {
    bool running = false;
    float progress = 0.0f;
    std::string message;
    std::vector<std::string> outputPaths;
};

class GifExporter {
public:
    GifExporter() = default;
    ~GifExporter();
    GifExporter(const GifExporter&) = delete;
    GifExporter& operator=(const GifExporter&) = delete;

    bool start(const Parameters& parameters, GifExportSettings settings);
    bool startFromBake(std::shared_ptr<const BakeResult> bake, GifExportSettings settings,
                       int startFrame, int endFrame);
    [[nodiscard]] GifExportStatus status() const;

private:
    std::atomic<bool> running_{false};
    std::atomic<float> progress_{0.0f};
    mutable std::mutex statusMutex_;
    std::string message_ = "Ready to export";
    std::vector<std::string> outputPaths_;
    std::jthread worker_;

    void setMessage(std::string message, std::vector<std::string> paths = {});
};

} // namespace rocket
