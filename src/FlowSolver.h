#pragma once

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

namespace rocket {

class VulkanFlowBackend;

enum class FieldView { Schlieren, Temperature, Mach, Pressure, Density, ExhaustFraction, Velocity };

struct Parameters {
    float chamberPressureMPa = 2.5f;
    float chamberTemperatureK = 3550.0f;
    float ambientPressureKPa = 101.325f;
    float ambientTemperatureK = 288.15f;
    float gamma = 1.22f;
    float molarMassGPerMol = 22.0f;
    float chamberRadiusM = 0.54f;
    float throatRadiusM = 0.14f;
    float exitRadiusM = 0.36f;
    float chamberLengthM = 0.62f;
    float convergingLengthM = 0.42f;
    float divergingLengthM = 0.90f;
    float cfl = 0.34f;
    float timeScale = 1.0f;
    int liveStepBudget = 3;
};

struct Diagnostics {
    float simulationTimeMs = 0.0f;
    float massFlowKgPerS = 0.0f;
    float thrustKN = 0.0f;
    float specificImpulseS = 0.0f;
    float exitMach = 0.0f;
    float exitPressureKPa = 0.0f;
    float maxMach = 0.0f;
    float minTemperatureK = 0.0f;
    float maxTemperatureK = 0.0f;
    float stepMilliseconds = 0.0f;
    int iteration = 0;
};

struct SolverSnapshot {
    int width = 0;
    int height = 0;
    Parameters parameters{};
    Diagnostics diagnostics{};
    std::vector<float> conserved;
    std::vector<uint8_t> fluidMask;
};

class FlowSolver {
public:
    FlowSolver(int width = 768, int height = 288);
    ~FlowSolver();
    FlowSolver(const FlowSolver&) = delete;
    FlowSolver& operator=(const FlowSolver&) = delete;

    void reset(const Parameters& parameters);
    void step(float realDeltaSeconds, const Parameters& parameters);
    void singleStep(const Parameters& parameters);
    void advanceSteps(int count, const Parameters& parameters);
    void enableGpu(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, uint32_t queueFamily);
    void disableGpu();
    [[nodiscard]] SolverSnapshot captureSnapshot() const;
    bool restoreSnapshot(const SolverSnapshot& snapshot);
    [[nodiscard]] bool gpuEnabled() const { return static_cast<bool>(gpu_); }

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] bool isFluid(int x, int y) const;
    [[nodiscard]] float nozzleRadius(float worldX, const Parameters& parameters) const;
    [[nodiscard]] float worldX(int x) const;
    [[nodiscard]] float worldY(int y) const;
    [[nodiscard]] float worldXMin() const { return xMin_; }
    [[nodiscard]] float worldXMax() const { return xMax_; }
    [[nodiscard]] float worldYMin() const { return yMin_; }
    [[nodiscard]] float worldYMax() const { return yMax_; }
    [[nodiscard]] std::array<float, 4> colorAt(int x, int y, FieldView view) const;
    [[nodiscard]] float exhaustFractionAt(int x, int y) const;
    [[nodiscard]] const Diagnostics& diagnostics() const { return diagnostics_; }
    [[nodiscard]] const std::string& status() const { return status_; }

    void setPaused(bool paused) { paused_ = paused; }
    [[nodiscard]] bool paused() const { return paused_; }

private:
    struct Cell {
        float rho = 1.225f;
        float mx = 0.0f;
        float my = 0.0f;
        float energy = 253000.0f;
        float exhaust = 0.0f;
    };

    struct Primitive {
        float rho, u, v, pressure, temperature, soundSpeed, exhaust;
    };

    int width_;
    int height_;
    float xMin_ = -0.82f;
    float xMax_ = 12.50f;
    float yMin_ = -1.70f;
    float yMax_ = 1.70f;
    float dx_;
    float dy_;
    Parameters activeParameters_{};
    std::vector<Cell> cells_;
    std::vector<Cell> next_;
    std::vector<Cell> stageBase_;
    std::vector<Primitive> primitiveCache_;
    std::vector<Cell> fluxX_;
    std::vector<Cell> fluxY_;
    std::vector<uint8_t> fluidMask_;
    Diagnostics diagnostics_{};
    std::string status_ = "Initializing";
    bool paused_ = false;
    float timeAccumulator_ = 0.0f;
    float parallelDt_ = 0.0f;
    int parallelPhase_ = 0;
    bool secondRungeKuttaStage_ = false;
    std::atomic<bool> stopWorkers_{false};
    std::vector<std::jthread> workers_;
    std::unique_ptr<std::barrier<>> workerStart_;
    std::unique_ptr<std::barrier<>> workerFinish_;
    std::unique_ptr<VulkanFlowBackend> gpu_;

    [[nodiscard]] int index(int x, int y) const { return y * width_ + x; }
    [[nodiscard]] Primitive primitive(const Cell& cell) const;
    [[nodiscard]] Cell stateFromPrimitive(float rho, float u, float v, float pressure, float exhaust) const;
    [[nodiscard]] Cell boundaryState(int x, int y, int nx, int ny, const Cell& center) const;
    [[nodiscard]] Cell reconstructedState(int x, int y, int axis, float direction) const;
    [[nodiscard]] bool physicallyValid(const Cell& cell) const;
    [[nodiscard]] Cell flux(const Cell& state, const Primitive& primitive, int axis) const;
    [[nodiscard]] Cell hllc(const Cell& left, const Cell& right, int axis) const;
    void rebuildPrimitiveCache();
    void rebuildGeometry(const Parameters& parameters);
    void computeFluxRows(int yBegin, int yEnd);
    void updateRows(float dt, int yBegin, int yEnd);
    void startWorkers();
    void advance(float dt);
    void updateDiagnostics();
    void uploadGpuState();
    void advanceGpuSteps(int count, const Parameters& parameters);
    [[nodiscard]] float stableTimeStep() const;
};

} // namespace rocket
