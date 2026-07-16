#include "GifExporter.h"
#include "HeadlessVulkanContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

std::string timestampedStem() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream name;
    name << "rocketsim_" << std::put_time(&local, "%Y%m%d_%H%M%S");
    const std::filesystem::path directory = std::filesystem::current_path() / "exports";
    std::filesystem::create_directories(directory);
    return (directory / name.str()).string();
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

bool GifExporter::start(const Parameters& parameters, GifExportSettings settings) {
    constexpr uint32_t allFieldsMask = (1u << 7u) - 1u;
    settings.fieldMask &= allFieldsMask;
    if (settings.fieldMask == 0) return false;
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();

    settings.playbackFps = std::clamp(settings.playbackFps, 8, 60);
    settings.durationSeconds = std::clamp(settings.durationSeconds, 1, 12);
    settings.solverStepsPerFrame = std::clamp(settings.solverStepsPerFrame, 1, 80);
    settings.warmupSteps = std::clamp(settings.warmupSteps, 0, 30000);
    progress_.store(0.0f);
    setMessage("GPU-building a developed plume before capture...");

    worker_ = std::jthread([this, parameters, settings](std::stop_token stop) {
        HeadlessVulkanContext gpu;
        FlowSolver exportSolver;
        exportSolver.enableGpu(gpu.physicalDevice(), gpu.device(), gpu.queue(), gpu.queueFamily());
        constexpr float warmupShare = 0.35f;
        int warmed = 0;
        while (warmed < settings.warmupSteps && !stop.stop_requested()) {
            const int count = std::min(512, settings.warmupSteps - warmed);
            exportSolver.advanceSteps(count, parameters);
            warmed += count;
            progress_.store(warmupShare * static_cast<float>(warmed) / std::max(settings.warmupSteps, 1));
        }
        if (stop.stop_requested()) {
            running_.store(false);
            setMessage("GIF export cancelled");
            return;
        }

        const std::string stem = timestampedStem();
        const int width = exportSolver.width();
        const int height = exportSolver.height();
        const float worldAspect = (exportSolver.worldXMax() - exportSolver.worldXMin()) /
                                  (exportSolver.worldYMax() - exportSolver.worldYMin());
        constexpr int outputWidth = 1536;
        const int outputHeight = std::max(64, static_cast<int>(std::round(outputWidth / worldAspect)));
        const int frameCount = settings.playbackFps * settings.durationSeconds;

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
            const std::string path = stem + "_" + fieldSlug(selected) + ".gif";
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
            exportSolver.advanceSteps(settings.solverStepsPerFrame, parameters);
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
            progress_.store(warmupShare + (1.0f - warmupShare) * static_cast<float>(frame + 1) / frameCount);
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
        const std::string stem = timestampedStem() + "_baked";
        std::vector<const BakedField*> selected;
        std::vector<std::string> paths;
        for (const BakedField& field : bake->fields) {
            if ((settings.fieldMask & (1u << static_cast<uint32_t>(field.view))) == 0) continue;
            selected.push_back(&field);
            paths.push_back(stem + "_" + fieldSlug(field.view) + ".gif");
        }
        setMessage("Encoding baked frame range " + std::to_string(startFrame) + " - " +
                   std::to_string(endFrame) + "...", paths);

        bool failed = false;
        const int outputWidth = 1536;
        const int outputHeight = std::max(64, static_cast<int>(std::round(outputWidth / bake->worldAspect)));
        for (size_t i = 0; i < selected.size() && !stop.stop_requested(); ++i) {
            std::ostringstream command;
            command << "ffmpeg -hide_banner -loglevel error -y -i " << shellQuote(selected[i]->videoPath)
                    << " -filter_complex \"select='between(n\\," << startFrame << "\\," << endFrame
                    << ")',setpts=N/(" << settings.playbackFps << "*TB),scale=" << outputWidth << ':'
                    << outputHeight << ":flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=256[p];"
                       "[s1][p]paletteuse=dither=sierra2_4a\" -r " << settings.playbackFps
                    << " -loop 0 " << shellQuote(paths[i]);
            failed |= std::system(command.str().c_str()) != 0;
            progress_.store(static_cast<float>(i + 1) / selected.size());
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
