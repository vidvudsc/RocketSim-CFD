#include "GifExporter.h"
#include "HeadlessVulkanContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <iomanip>
#include <sstream>
#include <vector>

namespace rocket {
namespace {
std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

std::string exportDirectory(const char* kind) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream baseName;
    baseName << "rocketsim_" << std::put_time(&local, "%Y%m%d_%H%M%S") << '_' << kind;
    const std::filesystem::path root = std::filesystem::current_path() / "exports";
    std::filesystem::create_directories(root);
    std::filesystem::path directory = root / baseName.str();
    for (int suffix = 2; std::filesystem::exists(directory); ++suffix)
        directory = root / (baseName.str() + "_" + std::to_string(suffix));
    std::filesystem::create_directories(directory);
    return directory.string();
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
} // namespace

GifExporter::~GifExporter() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void GifExporter::setMessage(std::string message, std::vector<std::string> paths) {
    std::scoped_lock lock(statusMutex_);
    message_ = std::move(message);
    outputPaths_ = std::move(paths);
}

GifExportStatus GifExporter::status() const {
    std::scoped_lock lock(statusMutex_);
    return {running_.load(), progress_.load(), message_, outputPaths_};
}

bool GifExporter::startFromPast(const SolverSnapshot& source, int endIteration,
                                GifExportSettings settings) {
    SolverSnapshot snapshot = source;
    constexpr uint32_t allFieldsMask = (1u << 7u) - 1u;
    settings.fieldMask &= allFieldsMask;
    if (settings.fieldMask == 0) return false;
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();

    settings.playbackFps = std::clamp(settings.playbackFps, 8, 60);
    settings.solverStepsPerFrame = std::clamp(settings.solverStepsPerFrame, 1, 80);
    endIteration = std::max(endIteration, snapshot.diagnostics.iteration);
    progress_.store(0.0f);
    setMessage("Replaying selected live history for GIF export...");

    worker_ = std::jthread([this, settings, endIteration, snapshot = std::move(snapshot)](std::stop_token stop) {
        HeadlessVulkanContext gpu;
        FlowSolver exportSolver(snapshot.width, snapshot.height);
        if (!exportSolver.restoreSnapshot(snapshot)) {
            running_.store(false);
            setMessage("Could not copy the current CFD state");
            return;
        }
        exportSolver.enableGpu(gpu.physicalDevice(), gpu.device(), gpu.queue(), gpu.queueFamily());
        if (stop.stop_requested()) {
            running_.store(false);
            setMessage("GIF export cancelled");
            return;
        }

        const std::string directory = exportDirectory("live_history");
        const int width = exportSolver.width();
        const int height = exportSolver.height();
        const float worldAspect = (exportSolver.worldXMax() - exportSolver.worldXMin()) /
                                  (exportSolver.worldYMax() - exportSolver.worldYMin());
        constexpr int outputWidth = 1536;
        const int outputHeight = std::max(64, static_cast<int>(std::round(outputWidth / worldAspect)));
        const int stepRange = endIteration - snapshot.diagnostics.iteration;
        const int frameCount = std::max(1, (stepRange + settings.solverStepsPerFrame - 1) /
                                             settings.solverStepsPerFrame + 1);

        struct FieldOutput {
            FieldView view;
            std::string path;
            FILE* pipe = nullptr;
            std::vector<unsigned char> pixels;
        };
        std::vector<FieldOutput> outputs;
        std::vector<std::string> paths;
        for (uint32_t field = 0; field < 7; ++field) {
            if ((settings.fieldMask & (1u << field)) == 0) continue;
            const FieldView selected = static_cast<FieldView>(field);
            const std::string path = (std::filesystem::path(directory) /
                                      (std::string(fieldSlug(selected)) + ".gif")).string();
            std::ostringstream command;
            command << "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgb24 -video_size "
                    << width << 'x' << height << " -framerate " << settings.playbackFps
                    << " -i - -filter_complex \"scale=" << outputWidth << ':' << outputHeight
                    << ":flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=256[p];"
                       "[s1][p]paletteuse=dither=sierra2_4a\" -loop 0 " << shellQuote(path);
            FILE* pipe = popen(command.str().c_str(), "w");
            if (!pipe) {
                for (FieldOutput& output : outputs) pclose(output.pipe);
                running_.store(false);
                setMessage("Could not start ffmpeg; install it or add it to PATH");
                return;
            }
            paths.push_back(path);
            outputs.push_back({selected, path, pipe,
                               std::vector<unsigned char>(static_cast<size_t>(width * height * 3))});
        }

        setMessage("Rendering synchronized high-resolution GIFs...", paths);
        bool writeFailed = false;
        for (int frame = 0; frame < frameCount && !stop.stop_requested(); ++frame) {
            for (FieldOutput& output : outputs) {
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const auto color = exportSolver.colorAt(x, height - 1 - y, output.view);
                        const size_t offset = static_cast<size_t>((y * width + x) * 3);
                        output.pixels[offset + 0] = static_cast<unsigned char>(255.0f * std::clamp(color[0], 0.0f, 1.0f));
                        output.pixels[offset + 1] = static_cast<unsigned char>(255.0f * std::clamp(color[1], 0.0f, 1.0f));
                        output.pixels[offset + 2] = static_cast<unsigned char>(255.0f * std::clamp(color[2], 0.0f, 1.0f));
                    }
                }
                if (fwrite(output.pixels.data(), 1, output.pixels.size(), output.pipe) != output.pixels.size()) {
                    writeFailed = true;
                    break;
                }
            }
            if (writeFailed) break;
            progress_.store(static_cast<float>(frame + 1) / frameCount);
            if (frame + 1 < frameCount) {
                const int remaining = endIteration - exportSolver.diagnostics().iteration;
                exportSolver.advanceSteps(std::min(settings.solverStepsPerFrame, remaining), snapshot.parameters);
            }
        }
        bool encodeFailed = false;
        for (FieldOutput& output : outputs) encodeFailed |= pclose(output.pipe) != 0;
        running_.store(false);
        if (stop.stop_requested()) setMessage("GIF export cancelled", paths);
        else if (writeFailed || encodeFailed) setMessage("ffmpeg failed while encoding one or more GIFs", paths);
        else {
            progress_.store(1.0f);
            setMessage(std::to_string(outputs.size()) + (outputs.size() == 1 ? " GIF saved" : " synchronized GIFs saved"), paths);
        }
    });
    return true;
}

bool GifExporter::startFromBake(std::shared_ptr<const BakeResult> bake, GifExportSettings settings,
                                int startFrame, int endFrame) {
    if (!bake || bake->frameCount <= 0) return false;
    settings.playbackFps = std::clamp(settings.playbackFps, 8, 60);
    settings.fieldMask &= (1u << 7u) - 1u;
    startFrame = std::clamp(startFrame, 0, bake->frameCount - 1);
    endFrame = std::clamp(endFrame, startFrame, bake->frameCount - 1);
    uint32_t availableMask = 0;
    for (const BakedField& field : bake->fields)
        availableMask |= 1u << static_cast<uint32_t>(field.view);
    settings.fieldMask &= availableMask;
    if (settings.fieldMask == 0 || running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    progress_.store(0.0f);
    setMessage("Exporting selected baked timeline...");

    worker_ = std::jthread([this, bake = std::move(bake), settings, startFrame, endFrame](std::stop_token stop) {
        const std::string directory = exportDirectory("baked");
        std::vector<const BakedField*> selected;
        std::vector<std::string> paths;
        for (const BakedField& field : bake->fields) {
            if ((settings.fieldMask & (1u << static_cast<uint32_t>(field.view))) == 0) continue;
            selected.push_back(&field);
            paths.push_back((std::filesystem::path(directory) /
                             (std::string(fieldSlug(field.view)) + ".gif")).string());
        }
        setMessage("Encoding baked frame range " + std::to_string(startFrame) + " - " +
                   std::to_string(endFrame) + "...", paths);

        const int outputWidth = 1536;
        const int outputHeight = std::max(64, static_cast<int>(std::round(outputWidth / bake->worldAspect)));
        const int selectedFrameCount = endFrame - startFrame + 1;
        auto encodeField = [&](size_t i) {
            std::ostringstream command;
            command << "ffmpeg -hide_banner -loglevel error -y -ss " << std::fixed << std::setprecision(6)
                    << static_cast<double>(startFrame) / 30.0 << " -t "
                    << static_cast<double>(selectedFrameCount) / 30.0 << " -i "
                    << shellQuote(selected[i]->videoPath)
                    << " -filter_complex \"setpts=N/(" << settings.playbackFps << "*TB),scale=" << outputWidth << ':'
                    << outputHeight << ":flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=256[p];"
                       "[s1][p]paletteuse=dither=sierra2_4a\" -r " << settings.playbackFps
                    << " -loop 0 " << shellQuote(paths[i]);
            return std::system(command.str().c_str()) != 0;
        };

        bool failed = false;
        size_t completed = 0;
        constexpr size_t parallelEncoders = 2;
        for (size_t batch = 0; batch < selected.size() && !stop.stop_requested(); batch += parallelEncoders) {
            std::vector<std::future<bool>> jobs;
            const size_t batchEnd = std::min(selected.size(), batch + parallelEncoders);
            for (size_t i = batch; i < batchEnd; ++i)
                jobs.push_back(std::async(std::launch::async, encodeField, i));
            for (std::future<bool>& job : jobs) {
                failed |= job.get();
                ++completed;
                progress_.store(static_cast<float>(completed) / selected.size());
            }
        }
        running_.store(false);
        if (stop.stop_requested()) setMessage("Baked GIF export cancelled", paths);
        else if (failed) setMessage("One or more baked GIF exports failed", paths);
        else {
            progress_.store(1.0f);
            setMessage(std::to_string(paths.size()) + (paths.size() == 1 ? " baked GIF saved" : " baked GIFs saved"), paths);
        }
    });
    return true;
}

} // namespace rocket
