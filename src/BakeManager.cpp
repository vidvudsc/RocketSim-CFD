#include "BakeManager.h"
#include "HeadlessVulkanContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

namespace rocket {
namespace {
std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

const char* fieldSlug(FieldView view) {
    switch (view) {
        case FieldView::Schlieren: return "schlieren";
        case FieldView::Temperature: return "temperature";
        case FieldView::Mach: return "mach";
        case FieldView::Pressure: return "pressure";
        case FieldView::Density: return "density";
        case FieldView::ExhaustFraction: return "exhaust";
        case FieldView::Velocity: return "velocity";
    }
    return "field";
}

bool fieldFromSlug(const std::string& slug, FieldView& view) {
    for (uint32_t index = 0; index < 7; ++index) {
        const FieldView candidate = static_cast<FieldView>(index);
        if (slug == fieldSlug(candidate)) { view = candidate; return true; }
    }
    return false;
}

struct ProbeResult { int width = 0; int height = 0; int frames = 0; };

ProbeResult probeVideo(const std::string& path) {
    const std::string command = "ffprobe -v error -count_frames -select_streams v:0 "
                                "-show_entries stream=width,height,nb_read_frames -of csv=p=0 " +
                                shellQuote(path) + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char line[256]{};
    const bool read = fgets(line, sizeof(line), pipe) != nullptr;
    pclose(pipe);
    ProbeResult result;
    if (read) std::sscanf(line, "%d,%d,%d", &result.width, &result.height, &result.frames);
    return result;
}

std::string bakeDirectory() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream name;
    name << "bake_" << std::put_time(&local, "%Y%m%d_%H%M%S");
    const std::filesystem::path directory = std::filesystem::current_path() / "bakes" / name.str();
    std::filesystem::create_directories(directory);
    return directory.string();
}
} // namespace

const BakedField* BakeResult::findField(FieldView view) const {
    for (const BakedField& field : fields) if (field.view == view) return &field;
    return nullptr;
}

BakeManager::~BakeManager() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void BakeManager::setMessage(std::string message) {
    std::scoped_lock lock(mutex_);
    message_ = std::move(message);
}

BakeStatus BakeManager::status() const {
    std::scoped_lock lock(mutex_);
    return {running_.load(), progress_.load(), etaSeconds_.load(), message_, result_};
}

bool BakeManager::start(const Parameters& parameters, BakeSettings settings) {
    settings.totalSteps = std::clamp(settings.totalSteps, 100, 200000);
    settings.captureEverySteps = std::clamp(settings.captureEverySteps, 1, 500);
    settings.resolutionScale = std::clamp(settings.resolutionScale, 1, 3);
    settings.fieldMask &= (1u << 7u) - 1u;
    int selectedFields = 0;
    for (uint32_t bit = 0; bit < 7; ++bit) selectedFields += (settings.fieldMask & (1u << bit)) != 0;
    const int estimatedFrames = (settings.totalSteps + settings.captureEverySteps - 1) / settings.captureEverySteps;
    const double estimatedBytes = static_cast<double>(estimatedFrames) * (768 * settings.resolutionScale) *
                                  (288 * settings.resolutionScale) * 3.0 * selectedFields * 0.15;
    std::error_code spaceError;
    const auto disk = std::filesystem::space(std::filesystem::current_path(), spaceError);
    constexpr uintmax_t safetyReserve = 512ull * 1024ull * 1024ull;
    if (!spaceError && disk.available < static_cast<uintmax_t>(estimatedBytes) + safetyReserve) {
        setMessage("Bake refused: estimated cache plus 512 MB safety reserve exceeds free disk space");
        return false;
    }
    if (settings.fieldMask == 0 || running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    progress_.store(0.0f);
    etaSeconds_.store(0.0f);
    setMessage("Preparing high-resolution bake...");

    worker_ = std::jthread([this, parameters, settings](std::stop_token stop) {
        auto result = std::make_shared<BakeResult>();
        result->parameters = parameters;
        result->solverWidth = 768 * settings.resolutionScale;
        result->solverHeight = 288 * settings.resolutionScale;
        result->previewWidth = 384;
        result->previewHeight = 112;
        result->captureEverySteps = settings.captureEverySteps;
        result->frameCount = (settings.totalSteps + settings.captureEverySteps - 1) / settings.captureEverySteps;
        result->directory = bakeDirectory();

        HeadlessVulkanContext gpu;
        FlowSolver solver(result->solverWidth, result->solverHeight);
        solver.enableGpu(gpu.physicalDevice(), gpu.device(), gpu.queue(), gpu.queueFamily());
        result->worldAspect = (solver.worldXMax() - solver.worldXMin()) /
                              (solver.worldYMax() - solver.worldYMin());
        result->cfdSteps.reserve(static_cast<size_t>(result->frameCount));

        struct Output {
            BakedField field;
            FILE* pipe = nullptr;
            std::vector<unsigned char> fullRgb;
        };
        std::vector<Output> outputs;
        for (uint32_t fieldIndex = 0; fieldIndex < 7; ++fieldIndex) {
            if ((settings.fieldMask & (1u << fieldIndex)) == 0) continue;
            const FieldView view = static_cast<FieldView>(fieldIndex);
            const std::string path = (std::filesystem::path(result->directory) /
                                      (std::string(fieldSlug(view)) + ".mkv")).string();
            std::ostringstream command;
            command << "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgb24 -video_size "
                    << result->solverWidth << 'x' << result->solverHeight
                    << " -framerate 30 -i - -c:v ffv1 -level 3 -g 1 " << shellQuote(path);
            FILE* pipe = popen(command.str().c_str(), "w");
            if (!pipe) {
                for (Output& output : outputs) pclose(output.pipe);
                running_.store(false);
                setMessage("Could not start FFmpeg for bake caching");
                return;
            }
            Output output;
            output.field.view = view;
            output.field.videoPath = path;
            output.field.previewRgb.reserve(static_cast<size_t>(result->frameCount) *
                                             result->previewWidth * result->previewHeight * 3);
            output.pipe = pipe;
            output.fullRgb.resize(static_cast<size_t>(result->solverWidth * result->solverHeight * 3));
            outputs.push_back(std::move(output));
        }

        const auto started = std::chrono::steady_clock::now();
        int completedSteps = 0;
        bool failed = false;
        for (int frame = 0; frame < result->frameCount && !stop.stop_requested(); ++frame) {
            const int stepCount = std::min(settings.captureEverySteps, settings.totalSteps - completedSteps);
            solver.advanceSteps(stepCount, parameters);
            completedSteps += stepCount;
            const float progress = static_cast<float>(completedSteps) / settings.totalSteps;
            progress_.store(progress);
            const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
            etaSeconds_.store(progress > 0.001f ? elapsed * (1.0f - progress) / progress : 0.0f);
            setMessage("GPU baking CFD step " + std::to_string(completedSteps) + " / " +
                       std::to_string(settings.totalSteps));
            if (stop.stop_requested()) break;
            result->cfdSteps.push_back(completedSteps);

            for (Output& output : outputs) {
                for (int y = 0; y < result->solverHeight; ++y) {
                    for (int x = 0; x < result->solverWidth; ++x) {
                        const auto color = solver.colorAt(x, result->solverHeight - 1 - y, output.field.view);
                        const size_t offset = static_cast<size_t>((y * result->solverWidth + x) * 3);
                        output.fullRgb[offset] = static_cast<unsigned char>(255.0f * std::clamp(color[0], 0.0f, 1.0f));
                        output.fullRgb[offset + 1] = static_cast<unsigned char>(255.0f * std::clamp(color[1], 0.0f, 1.0f));
                        output.fullRgb[offset + 2] = static_cast<unsigned char>(255.0f * std::clamp(color[2], 0.0f, 1.0f));
                    }
                }
                if (fwrite(output.fullRgb.data(), 1, output.fullRgb.size(), output.pipe) != output.fullRgb.size()) {
                    failed = true;
                    break;
                }
                for (int py = 0; py < result->previewHeight; ++py) {
                    const int sy = py * result->solverHeight / result->previewHeight;
                    for (int px = 0; px < result->previewWidth; ++px) {
                        const int sx = px * result->solverWidth / result->previewWidth;
                        const size_t source = static_cast<size_t>((sy * result->solverWidth + sx) * 3);
                        output.field.previewRgb.insert(output.field.previewRgb.end(),
                                                      output.fullRgb.begin() + static_cast<std::ptrdiff_t>(source),
                                                      output.fullRgb.begin() + static_cast<std::ptrdiff_t>(source + 3));
                    }
                }
            }
            if (failed) break;

        }

        bool encodeFailed = false;
        for (Output& output : outputs) {
            encodeFailed |= pclose(output.pipe) != 0;
            result->fields.push_back(std::move(output.field));
        }
        running_.store(false);
        etaSeconds_.store(0.0f);
        if (stop.stop_requested()) setMessage("Bake cancelled");
        else if (failed || encodeFailed) setMessage("Bake cache encoding failed");
        else {
            progress_.store(1.0f);
            {
                std::scoped_lock lock(mutex_);
                result_ = result;
                message_ = "Bake ready: " + std::to_string(result->frameCount) + " cached frames";
            }
        }
    });
    return true;
}

bool BakeManager::recoverLatest(const Parameters& parameters, int captureEverySteps) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    captureEverySteps = std::clamp(captureEverySteps, 1, 500);
    progress_.store(0.0f);
    etaSeconds_.store(0.0f);
    setMessage("Scanning interrupted bake caches...");

    worker_ = std::jthread([this, parameters, captureEverySteps](std::stop_token stop) {
        const std::filesystem::path root = std::filesystem::current_path() / "bakes";
        std::filesystem::path newest;
        auto newestTime = std::filesystem::file_time_type::min();
        std::error_code error;
        if (std::filesystem::exists(root, error)) {
            for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
                if (!entry.is_directory()) continue;
                bool hasVideo = false;
                for (const auto& file : std::filesystem::directory_iterator(entry.path(), error))
                    hasVideo |= file.path().extension() == ".mkv";
                const auto time = entry.last_write_time(error);
                if (hasVideo && !error && time > newestTime) { newest = entry.path(); newestTime = time; }
            }
        }
        if (newest.empty()) {
            running_.store(false);
            setMessage("No bake cache found under bakes/");
            return;
        }

        auto result = std::make_shared<BakeResult>();
        result->parameters = parameters;
        result->directory = newest.string();
        result->captureEverySteps = captureEverySteps;
        result->worldAspect = 13.32f / 2.70f;
        int minimumFrames = std::numeric_limits<int>::max();
        struct RecoverSource { FieldView view; std::string path; ProbeResult probe; };
        std::vector<RecoverSource> sources;
        for (const auto& file : std::filesystem::directory_iterator(newest, error)) {
            if (file.path().extension() != ".mkv") continue;
            FieldView view;
            if (!fieldFromSlug(file.path().stem().string(), view)) continue;
            const ProbeResult probe = probeVideo(file.path().string());
            if (probe.frames <= 0) continue;
            sources.push_back({view, file.path().string(), probe});
            minimumFrames = std::min(minimumFrames, probe.frames);
            if (result->solverWidth == 0) { result->solverWidth = probe.width; result->solverHeight = probe.height; }
        }
        if (sources.empty() || minimumFrames <= 0) {
            running_.store(false);
            setMessage("Latest bake contains no recoverable complete frames");
            return;
        }

        result->frameCount = minimumFrames;
        result->previewWidth = 256;
        result->previewHeight = std::max(1, result->previewWidth * result->solverHeight / result->solverWidth);
        result->cfdSteps.reserve(static_cast<size_t>(minimumFrames));
        for (int frame = 0; frame < minimumFrames; ++frame)
            result->cfdSteps.push_back((frame + 1) * captureEverySteps);

        const auto started = std::chrono::steady_clock::now();
        const size_t frameBytes = static_cast<size_t>(result->previewWidth * result->previewHeight * 3);
        size_t completedUnits = 0;
        const size_t totalUnits = static_cast<size_t>(minimumFrames) * sources.size();
        for (const RecoverSource& source : sources) {
            if (stop.stop_requested()) break;
            BakedField field;
            field.view = source.view;
            field.videoPath = source.path;
            field.previewRgb.reserve(frameBytes * static_cast<size_t>(minimumFrames));
            std::ostringstream command;
            command << "ffmpeg -hide_banner -loglevel error -i " << shellQuote(source.path)
                    << " -vf scale=" << result->previewWidth << ':' << result->previewHeight
                    << ":flags=area -pix_fmt rgb24 -f rawvideo - 2>/dev/null";
            FILE* pipe = popen(command.str().c_str(), "r");
            if (!pipe) continue;
            std::vector<unsigned char> frame(frameBytes);
            int decoded = 0;
            while (decoded < minimumFrames && !stop.stop_requested()) {
                size_t received = 0;
                while (received < frameBytes) {
                    const size_t count = fread(frame.data() + received, 1, frameBytes - received, pipe);
                    if (count == 0) break;
                    received += count;
                }
                if (received != frameBytes) break;
                field.previewRgb.insert(field.previewRgb.end(), frame.begin(), frame.end());
                ++decoded;
                ++completedUnits;
                const float progress = static_cast<float>(completedUnits) / totalUnits;
                progress_.store(progress);
                const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
                etaSeconds_.store(progress > 0.001f ? elapsed * (1.0f - progress) / progress : 0.0f);
            }
            pclose(pipe); // A missing Matroska trailer may return non-zero after all complete frames were decoded.
            if (decoded == minimumFrames) result->fields.push_back(std::move(field));
        }

        running_.store(false);
        etaSeconds_.store(0.0f);
        if (stop.stop_requested()) setMessage("Bake recovery cancelled");
        else if (result->fields.empty()) setMessage("Could not decode previews from interrupted bake");
        else {
            progress_.store(1.0f);
            std::scoped_lock lock(mutex_);
            result_ = result;
            message_ = "Recovered " + std::to_string(result->frameCount) + " complete frames from " +
                       std::to_string(result->fields.size()) + " fields";
        }
    });
    return true;
}

} // namespace rocket
