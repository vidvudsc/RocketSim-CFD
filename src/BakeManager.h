#pragma once

#include "FlowSolver.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rocket {

struct BakeSettings {
    int totalSteps = 8000;
    int captureEverySteps = 40;
    int resolutionScale = 2;
    uint32_t fieldMask = (1u << static_cast<uint32_t>(FieldView::Schlieren)) |
                         (1u << static_cast<uint32_t>(FieldView::Mach));
};

struct BakedField {
    FieldView view = FieldView::Schlieren;
    std::string videoPath;
    std::vector<unsigned char> previewRgb;
};

struct BakeResult {
    Parameters parameters{};
    int solverWidth = 0;
    int solverHeight = 0;
    int previewWidth = 0;
    int previewHeight = 0;
    int captureEverySteps = 1;
    int frameCount = 0;
    float worldAspect = 1.0f;
    std::string directory;
    std::vector<int> cfdSteps;
    std::vector<BakedField> fields;

    [[nodiscard]] const BakedField* findField(FieldView view) const;
};

struct BakeStatus {
    bool running = false;
    float progress = 0.0f;
    float etaSeconds = 0.0f;
    std::string message;
    std::shared_ptr<const BakeResult> result;
};

class BakeManager {
public:
    BakeManager() = default;
    ~BakeManager();
    BakeManager(const BakeManager&) = delete;
    BakeManager& operator=(const BakeManager&) = delete;

    bool start(const Parameters& parameters, BakeSettings settings);
    bool recoverLatest(const Parameters& parameters, int captureEverySteps);
    [[nodiscard]] BakeStatus status() const;

private:
    std::atomic<bool> running_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<float> etaSeconds_{0.0f};
    mutable std::mutex mutex_;
    std::string message_ = "No bake available";
    std::shared_ptr<const BakeResult> result_;
    std::jthread worker_;

    void setMessage(std::string message);
};

} // namespace rocket
