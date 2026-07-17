#include "FlowSolver.h"
#include "VulkanFlowBackend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace rocket {
namespace {
constexpr float kRu = 8.314462618f;
constexpr float kGravity = 9.80665f;

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

std::array<float, 3> turbo(float t) {
    t = clamp01(t);
    const float r = 0.1357f + t * (4.6154f + t * (-42.6603f + t * (132.1311f + t * (-152.9424f + 59.2864f * t))));
    const float g = 0.0914f + t * (2.1942f + t * (4.8429f + t * (-14.1850f + t * (4.2773f + 2.8296f * t))));
    const float b = 0.1067f + t * (12.6419f + t * (-60.5820f + t * (110.3627f + t * (-89.9031f + 27.3482f * t))));
    return {clamp01(r), clamp01(g), clamp01(b)};
}

std::array<float, 3> blueRed(float t) {
    t = clamp01(t);
    return {clamp01(1.5f * t), 0.22f + 0.48f * (1.0f - std::abs(2.0f * t - 1.0f)), clamp01(1.45f * (1.0f - t))};
}
} // namespace

Parameters transientReservoirParameters(const Parameters& requested, float simulationTimeMs) {
    if (!requested.hardStartEnabled) return requested;
    Parameters effective = requested;
    const float preIgnition = std::clamp(requested.preIgnitionPressureFraction, 0.01f, 0.60f);
    const float elapsed = simulationTimeMs - std::max(requested.ignitionDelayMs, 0.0f);
    if (elapsed <= 0.0f) {
        effective.chamberPressureMPa = requested.chamberPressureMPa * preIgnition;
        effective.chamberTemperatureK = requested.ambientTemperatureK;
        return effective;
    }
    const float riseTime = std::max(requested.ignitionRiseMs, 0.02f);
    const float normalized = std::clamp(elapsed / riseTime, 0.0f, 1.0f);
    const float ramp = normalized * normalized * (3.0f - 2.0f * normalized);
    const float pulse = elapsed <= riseTime ? ramp :
        std::exp(-(elapsed - riseTime) / std::max(requested.hardStartDecayMs, 0.05f));
    const float pressureRatio = std::max(requested.hardStartPressureRatio, 1.0f);
    effective.chamberPressureMPa = requested.chamberPressureMPa *
        (preIgnition + (1.0f - preIgnition) * ramp + (pressureRatio - 1.0f) * pulse);
    effective.chamberTemperatureK = requested.ambientTemperatureK +
        (requested.chamberTemperatureK - requested.ambientTemperatureK) * ramp;
    return effective;
}

FlowSolver::FlowSolver(int width, int height)
    : width_(width), height_(height), dx_((xMax_ - xMin_) / width), dy_((yMax_ - yMin_) / height),
      cells_(static_cast<size_t>(width * height)), next_(cells_.size()),
      stageBase_(cells_.size()), primitiveCache_(cells_.size()),
      fluxX_(static_cast<size_t>((width + 1) * height)),
      fluxY_(static_cast<size_t>(width * (height + 1))), fluidMask_(cells_.size()) {
    reset(activeParameters_);
    startWorkers();
}

FlowSolver::~FlowSolver() {
    if (workers_.empty()) return;
    stopWorkers_.store(true, std::memory_order_release);
    workerStart_->arrive_and_wait();
    workers_.clear();
}

void FlowSolver::startWorkers() {
    if (width_ < 400) return;
    const int workerCount = std::min<int>(8, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
    workerStart_ = std::make_unique<std::barrier<>>(workerCount + 1);
    workerFinish_ = std::make_unique<std::barrier<>>(workerCount + 1);
    workers_.reserve(static_cast<size_t>(workerCount));
    for (int worker = 0; worker < workerCount; ++worker) {
        const int begin = height_ * worker / workerCount;
        const int end = height_ * (worker + 1) / workerCount;
        workers_.emplace_back([this, begin, end] {
            while (true) {
                workerStart_->arrive_and_wait();
                if (stopWorkers_.load(std::memory_order_acquire)) break;
                if (parallelPhase_ == 0) computeFluxRows(begin, end);
                else updateRows(parallelDt_, begin, end);
                workerFinish_->arrive_and_wait();
            }
        });
    }
}

float FlowSolver::worldX(int x) const { return xMin_ + (x + 0.5f) * dx_; }
float FlowSolver::worldY(int y) const { return yMin_ + (y + 0.5f) * dy_; }

float FlowSolver::nozzleRadius(float x, const Parameters& p) const {
    const float chamberStart = -0.10f - p.chamberLengthM;
    // chamberLengthM is the physical head-to-converging-section length.  The
    // old +length expression accidentally made the chamber twice as long.
    constexpr float chamberEnd = -0.10f;
    const float throatX = chamberEnd + p.convergingLengthM;
    const float exitX = throatX + p.divergingLengthM;
    if (x < chamberStart) return -1.0f;
    if (x <= chamberEnd) return p.chamberRadiusM;
    if (x < throatX) {
        const float t = (x - chamberEnd) / std::max(p.convergingLengthM, 0.01f);
        const float s = t * t * (3.0f - 2.0f * t);
        return std::lerp(p.chamberRadiusM, p.throatRadiusM, s);
    }
    if (x < exitX) {
        const float t = (x - throatX) / std::max(p.divergingLengthM, 0.01f);
        const float s = t * (1.6f - 0.6f * t);
        return std::lerp(p.throatRadiusM, p.exitRadiusM, s);
    }
    return -1.0f;
}

bool FlowSolver::isFluid(int x, int y) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;
    return fluidMask_[index(x, y)] != 0;
}

float FlowSolver::exhaustFractionAt(int x, int y) const {
    if (!isFluid(x, y)) return 0.0f;
    return primitive(cells_[index(x, y)]).exhaust;
}

FlowSolver::Cell FlowSolver::stateFromPrimitive(float rho, float u, float v, float pressure, float exhaust) const {
    Cell c;
    c.rho = std::max(rho, 1.0e-5f);
    c.mx = c.rho * u;
    c.my = c.rho * v;
    c.energy = pressure / (activeParameters_.gamma - 1.0f) + 0.5f * c.rho * (u * u + v * v);
    c.exhaust = c.rho * clamp01(exhaust);
    return c;
}

FlowSolver::Primitive FlowSolver::primitive(const Cell& c) const {
    const float rho = std::max(c.rho, 1.0e-5f);
    const float u = c.mx / rho;
    const float v = c.my / rho;
    const float kinetic = 0.5f * rho * (u * u + v * v);
    const float p = std::max((activeParameters_.gamma - 1.0f) * (c.energy - kinetic), 30.0f);
    const float gasR = kRu / (activeParameters_.molarMassGPerMol * 0.001f);
    return {rho, u, v, p, p / (rho * gasR), std::sqrt(activeParameters_.gamma * p / rho), clamp01(c.exhaust / rho)};
}

void FlowSolver::rebuildGeometry(const Parameters& p) {
    const float chamberStart = -0.10f - p.chamberLengthM;
    constexpr float chamberEnd = -0.10f;
    const float exitX = chamberEnd + p.convergingLengthM + p.divergingLengthM;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const float wx = worldX(x);
            const float wy = std::abs(worldY(y));
            const float radius = nozzleRadius(wx, p);
            const float wallThickness = std::min(1.55f * std::max(dx_, dy_), 0.22f * p.throatRadiusM);
            const bool sideWall = wx >= chamberStart - wallThickness && wx < exitX && radius > 0.0f &&
                                  std::abs(wy - radius) <= wallThickness;
            const bool backWall = std::abs(wx - chamberStart) <= 1.65f * wallThickness &&
                                  wy <= p.chamberRadiusM + 2.0f * wallThickness;
            // The exterior is ambient fluid. Only the thin metal contour is
            // solid, so plume/atmosphere interaction is visible around the engine.
            fluidMask_[index(x, y)] = static_cast<uint8_t>(!(sideWall || backWall));
        }
    }
}

void FlowSolver::reset(const Parameters& p) {
    activeParameters_ = p;
    rebuildGeometry(p);
    const Parameters reservoir = transientReservoirParameters(p, 0.0f);
    const float gasR = kRu / (p.molarMassGPerMol * 0.001f);
    const float rhoAmbient = p.ambientPressureKPa * 1000.0f / (gasR * p.ambientTemperatureK);
    const float rhoChamber = reservoir.chamberPressureMPa * 1.0e6f /
                             (gasR * reservoir.chamberTemperatureK);
    const float chamberStart = -0.10f - p.chamberLengthM;
    const float wallThickness = std::min(1.55f * std::max(dx_, dy_), 0.22f * p.throatRadiusM);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const float wx = worldX(x);
            const bool injectorPlenum = wx > chamberStart + 2.0f * wallThickness &&
                                        wx < chamberStart + 2.0f * wallThickness + 4.0f * dx_ &&
                                        std::abs(worldY(y)) < p.chamberRadiusM - 2.0f * wallThickness;
            if (!isFluid(x, y)) {
                cells_[index(x, y)] = {};
            } else if (injectorPlenum) {
                cells_[index(x, y)] = stateFromPrimitive(rhoChamber, 4.0f, 0.0f,
                                                         reservoir.chamberPressureMPa * 1.0e6f, 1.0f);
            } else {
                cells_[index(x, y)] = stateFromPrimitive(rhoAmbient, 0.0f, 0.0f, p.ambientPressureKPa * 1000.0f, 0.0f);
            }
        }
    }
    next_ = cells_;
    diagnostics_ = {};
    timeAccumulator_ = 0.0f;
    status_ = "Injector plenum charged; chamber and nozzle start at ambient conditions";
    updateDiagnostics();
    if (gpu_) uploadGpuState();
}

void FlowSolver::enableGpu(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, uint32_t queueFamily) {
    gpu_ = std::make_unique<VulkanFlowBackend>(physicalDevice, device, queue, queueFamily,
                                               width_, height_, dx_, dy_, xMin_, yMin_);
    uploadGpuState();
    status_ = "Axisymmetric Vulkan compute solver active";
}

void FlowSolver::uploadGpuState() {
    std::vector<float> packed(cells_.size() * 5);
    std::vector<uint32_t> mask(fluidMask_.size());
    for (size_t i = 0; i < cells_.size(); ++i) {
        packed[i*5]=cells_[i].rho; packed[i*5+1]=cells_[i].mx; packed[i*5+2]=cells_[i].my;
        packed[i*5+3]=cells_[i].energy; packed[i*5+4]=cells_[i].exhaust; mask[i]=fluidMask_[i];
    }
    gpu_->upload(packed, mask);
}

void FlowSolver::disableGpu() { gpu_.reset(); }

SolverSnapshot FlowSolver::captureSnapshot() const {
    SolverSnapshot snapshot;
    snapshot.width = width_;
    snapshot.height = height_;
    snapshot.parameters = activeParameters_;
    snapshot.diagnostics = diagnostics_;
    snapshot.fluidMask = fluidMask_;
    snapshot.conserved.resize(cells_.size() * 5);
    for (size_t i = 0; i < cells_.size(); ++i) {
        snapshot.conserved[i*5] = cells_[i].rho;
        snapshot.conserved[i*5+1] = cells_[i].mx;
        snapshot.conserved[i*5+2] = cells_[i].my;
        snapshot.conserved[i*5+3] = cells_[i].energy;
        snapshot.conserved[i*5+4] = cells_[i].exhaust;
    }
    return snapshot;
}

bool FlowSolver::restoreSnapshot(const SolverSnapshot& snapshot) {
    if (snapshot.width != width_ || snapshot.height != height_ ||
        snapshot.conserved.size() != cells_.size() * 5 ||
        snapshot.fluidMask.size() != cells_.size()) return false;
    activeParameters_ = snapshot.parameters;
    diagnostics_ = snapshot.diagnostics;
    fluidMask_ = snapshot.fluidMask;
    for (size_t i = 0; i < cells_.size(); ++i) {
        cells_[i] = {snapshot.conserved[i*5], snapshot.conserved[i*5+1],
                     snapshot.conserved[i*5+2], snapshot.conserved[i*5+3],
                     snapshot.conserved[i*5+4]};
    }
    next_ = cells_;
    stageBase_ = cells_;
    timeAccumulator_ = 0.0f;
    rebuildPrimitiveCache();
    if (gpu_) uploadGpuState();
    status_ = "Restored current CFD state for export";
    return true;
}

void FlowSolver::advanceGpuSteps(int count, const Parameters& parameters) {
    activeParameters_ = parameters;
    int remaining = std::max(count, 0);
    while (remaining > 0) {
        const float resolvedTransientEnd = parameters.ignitionDelayMs + parameters.ignitionRiseMs +
                                           6.0f * parameters.hardStartDecayMs;
        const bool resolvingIgnition = parameters.hardStartEnabled &&
                                        diagnostics_.simulationTimeMs < resolvedTransientEnd;
        const int chunk = std::min(remaining, resolvingIgnition ? 64 : 256);
        const Parameters reservoir = transientReservoirParameters(parameters, diagnostics_.simulationTimeMs);
        const float dt = gpu_->advance(chunk, reservoir);
        diagnostics_.simulationTimeMs += dt * chunk * 1000.0f;
        diagnostics_.iteration += chunk;
        remaining -= chunk;
    }
    std::vector<float> packed;
    gpu_->download(packed);
    for (size_t i = 0; i < cells_.size(); ++i) {
        cells_[i] = {packed[i*5],packed[i*5+1],packed[i*5+2],packed[i*5+3],packed[i*5+4]};
    }
    next_ = cells_;
    activeParameters_ = parameters;
}

FlowSolver::Cell FlowSolver::flux(const Cell& c, const Primitive& q, int axis) const {
    Cell f;
    if (axis == 0) {
        f.rho = c.mx;
        f.mx = c.mx * q.u + q.pressure;
        f.my = c.my * q.u;
        f.energy = (c.energy + q.pressure) * q.u;
        f.exhaust = c.exhaust * q.u;
    } else {
        f.rho = c.my;
        f.mx = c.mx * q.v;
        f.my = c.my * q.v + q.pressure;
        f.energy = (c.energy + q.pressure) * q.v;
        f.exhaust = c.exhaust * q.v;
    }
    return f;
}

FlowSolver::Cell FlowSolver::hllc(const Cell& left, const Cell& right, int axis) const {
    const Primitive l = primitive(left);
    const Primitive r = primitive(right);
    const Cell fl = flux(left, l, axis);
    const Cell fr = flux(right, r, axis);
    const float unL = axis == 0 ? l.u : l.v;
    const float unR = axis == 0 ? r.u : r.v;
    const float sL = std::min(unL - l.soundSpeed, unR - r.soundSpeed);
    const float sR = std::max(unL + l.soundSpeed, unR + r.soundSpeed);
    if (sL >= 0.0f) return fl;
    if (sR <= 0.0f) return fr;

    const float denominator = l.rho * (sL - unL) - r.rho * (sR - unR);
    if (std::abs(denominator) < 1.0e-7f) {
        const float speed = std::max(std::abs(unL) + l.soundSpeed, std::abs(unR) + r.soundSpeed);
        return {
            0.5f * (fl.rho + fr.rho - speed * (right.rho - left.rho)),
            0.5f * (fl.mx + fr.mx - speed * (right.mx - left.mx)),
            0.5f * (fl.my + fr.my - speed * (right.my - left.my)),
            0.5f * (fl.energy + fr.energy - speed * (right.energy - left.energy)),
            0.5f * (fl.exhaust + fr.exhaust - speed * (right.exhaust - left.exhaust)),
        };
    }
    const float sM = (r.pressure - l.pressure + l.rho * unL * (sL - unL) -
                      r.rho * unR * (sR - unR)) / denominator;

    const auto starFlux = [&](const Cell& state, const Primitive& q, const Cell& baseFlux, float sK, float un) {
        const float rhoStar = q.rho * (sK - un) / (sK - sM);
        const float pStar = q.pressure + q.rho * (sK - un) * (sM - un);
        Cell star;
        star.rho = rhoStar;
        if (axis == 0) {
            star.mx = rhoStar * sM;
            star.my = rhoStar * q.v;
        } else {
            star.mx = rhoStar * q.u;
            star.my = rhoStar * sM;
        }
        star.energy = ((sK - un) * state.energy - q.pressure * un + pStar * sM) / (sK - sM);
        star.exhaust = rhoStar * q.exhaust;
        return Cell{
            baseFlux.rho + sK * (star.rho - state.rho),
            baseFlux.mx + sK * (star.mx - state.mx),
            baseFlux.my + sK * (star.my - state.my),
            baseFlux.energy + sK * (star.energy - state.energy),
            baseFlux.exhaust + sK * (star.exhaust - state.exhaust),
        };
    };
    return sM >= 0.0f ? starFlux(left, l, fl, sL, unL) : starFlux(right, r, fr, sR, unR);
}

FlowSolver::Cell FlowSolver::boundaryState(int x, int y, int nx, int ny, const Cell& center) const {
    if (nx < 0 || nx >= width_ || ny < 0 || ny >= height_) {
        const Primitive q = primitive(center);
        // Convective far field: extrapolate outgoing flow and prescribe
        // ambient only for incoming characteristics. This stops the outlet
        // from sending a clearing wave back through an established plume.
        const bool outgoing = (nx >= width_ && q.u >= 0.0f) || (nx < 0 && q.u <= 0.0f) ||
                              (ny >= height_ && q.v >= 0.0f) || (ny < 0 && q.v <= 0.0f);
        if (outgoing) return center;
        const float gasR = kRu / (activeParameters_.molarMassGPerMol * 0.001f);
        const float rho = activeParameters_.ambientPressureKPa * 1000.0f / (gasR * activeParameters_.ambientTemperatureK);
        return stateFromPrimitive(rho, 0.0f, 0.0f, activeParameters_.ambientPressureKPa * 1000.0f, 0.0f);
    }
    if (!isFluid(nx, ny)) {
        Primitive q = primitive(center);
        const float solidX = worldX(std::clamp(nx, 0, width_ - 1));
        const float solidY = worldY(std::clamp(ny, 0, height_ - 1));
        const float radius = nozzleRadius(solidX, activeParameters_);
        const float chamberStart = -0.10f - activeParameters_.chamberLengthM;
        const bool backWall = std::abs(solidX - chamberStart) < 3.0f * dx_ &&
                              std::abs(solidY) < activeParameters_.chamberRadiusM + 3.0f * dy_;
        if (backWall || radius <= 0.0f) {
            if (nx != x) q.u = -q.u;
            if (ny != y) q.v = -q.v;
        } else {
            // Reflect against the analytic curved wall instead of the grid's
            // staircase face. This removes false oblique shocks at the nozzle.
            const float r0 = nozzleRadius(solidX - dx_, activeParameters_);
            const float r1 = nozzleRadius(solidX + dx_, activeParameters_);
            const float drdx = (r0 > 0.0f && r1 > 0.0f) ? (r1 - r0) / (2.0f * dx_) : 0.0f;
            float normalX = -drdx;
            float normalY = std::copysign(1.0f, solidY);
            const float inverseLength = 1.0f / std::max(std::hypot(normalX, normalY), 1.0e-6f);
            normalX *= inverseLength;
            normalY *= inverseLength;
            const float normalVelocity = q.u * normalX + q.v * normalY;
            q.u -= 2.0f * normalVelocity * normalX;
            q.v -= 2.0f * normalVelocity * normalY;
        }
        return stateFromPrimitive(q.rho, q.u, q.v, q.pressure, q.exhaust);
    }
    return cells_[index(nx, ny)];
}

bool FlowSolver::physicallyValid(const Cell& c) const {
    if (!std::isfinite(c.rho) || !std::isfinite(c.mx) || !std::isfinite(c.my) ||
        !std::isfinite(c.energy) || c.rho <= 1.0e-5f) return false;
    const float kinetic = 0.5f * (c.mx * c.mx + c.my * c.my) / c.rho;
    return c.energy > kinetic + 30.0f / (activeParameters_.gamma - 1.0f) &&
           c.exhaust >= 0.0f && c.exhaust <= c.rho;
}

FlowSolver::Cell FlowSolver::reconstructedState(int x, int y, int axis, float direction) const {
    const Cell& center = cells_[index(x, y)];
    const int ax = axis == 0 ? 1 : 0;
    const int ay = axis == 1 ? 1 : 0;
    if (!isFluid(x - ax, y - ay) || !isFluid(x + ax, y + ay)) return center;
    const Primitive& a = primitiveCache_[index(x - ax, y - ay)];
    const Primitive& b = primitiveCache_[index(x, y)];
    const Primitive& c = primitiveCache_[index(x + ax, y + ay)];
    const auto minmod = [](float backward, float forward) {
        if (backward * forward <= 0.0f) return 0.0f;
        return std::copysign(std::min(std::abs(backward), std::abs(forward)), backward);
    };
    const auto face = [&](float av, float bv, float cv) {
        return bv + 0.5f * direction * minmod(bv - av, cv - bv);
    };
    Cell result = stateFromPrimitive(face(a.rho, b.rho, c.rho),
                                     face(a.u, b.u, c.u),
                                     face(a.v, b.v, c.v),
                                     face(a.pressure, b.pressure, c.pressure),
                                     face(a.exhaust, b.exhaust, c.exhaust));
    return physicallyValid(result) ? result : center;
}

void FlowSolver::rebuildPrimitiveCache() {
    for (size_t i = 0; i < cells_.size(); ++i)
        if (fluidMask_[i]) primitiveCache_[i] = primitive(cells_[i]);
}

float FlowSolver::stableTimeStep() const {
    float maxWave = 1.0f;
    for (size_t i = 0; i < cells_.size(); ++i) {
        if (!fluidMask_[i]) continue;
        const Primitive q = primitive(cells_[i]);
        maxWave = std::max(maxWave, std::max(std::abs(q.u), std::abs(q.v)) + q.soundSpeed);
    }
    return activeParameters_.cfl * std::min(dx_, dy_) / maxWave;
}

void FlowSolver::computeFluxRows(int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
        for (int face = 0; face <= width_; ++face) {
            const int lx = face - 1;
            const int rx = face;
            const bool hasLeft = isFluid(lx, y);
            const bool hasRight = isFluid(rx, y);
            Cell result{};
            if (hasLeft && hasRight) {
                result = hllc(reconstructedState(lx, y, 0, 1.0f),
                              reconstructedState(rx, y, 0, -1.0f), 0);
            } else if (hasLeft) {
                const Cell& left = cells_[index(lx, y)];
                result = hllc(left, boundaryState(lx, y, rx, y, left), 0);
            } else if (hasRight) {
                const Cell& right = cells_[index(rx, y)];
                result = hllc(boundaryState(rx, y, lx, y, right), right, 0);
            }
            fluxX_[static_cast<size_t>(y * (width_ + 1) + face)] = result;
        }
    }

    const int faceEnd = yEnd == height_ ? yEnd + 1 : yEnd;
    for (int face = yBegin; face < faceEnd; ++face) {
        const int by = face - 1;
        const int ty = face;
        for (int x = 0; x < width_; ++x) {
            const bool hasBottom = isFluid(x, by);
            const bool hasTop = isFluid(x, ty);
            Cell result{};
            if (hasBottom && hasTop) {
                result = hllc(reconstructedState(x, by, 1, 1.0f),
                              reconstructedState(x, ty, 1, -1.0f), 1);
            } else if (hasBottom) {
                const Cell& bottom = cells_[index(x, by)];
                result = hllc(bottom, boundaryState(x, by, x, ty, bottom), 1);
            } else if (hasTop) {
                const Cell& top = cells_[index(x, ty)];
                result = hllc(boundaryState(x, ty, x, by, top), top, 1);
            }
            fluxY_[static_cast<size_t>(face * width_ + x)] = result;
        }
    }
}

void FlowSolver::updateRows(float dt, int yBegin, int yEnd) {
    const Parameters reservoirParameters = transientReservoirParameters(activeParameters_,
                                                                         diagnostics_.simulationTimeMs);
    for (int y = yBegin; y < yEnd; ++y) {
        for (int x = 0; x < width_; ++x) {
            const int i = index(x, y);
            if (!fluidMask_[i]) continue;
            const Cell& c = cells_[i];
            const Cell& fxL = fluxX_[static_cast<size_t>(y * (width_ + 1) + x)];
            const Cell& fxR = fluxX_[static_cast<size_t>(y * (width_ + 1) + x + 1)];
            const Cell& fyB = fluxY_[static_cast<size_t>(y * width_ + x)];
            const Cell& fyT = fluxY_[static_cast<size_t>((y + 1) * width_ + x)];
            Cell n;
            n.rho = c.rho - dt * ((fxR.rho - fxL.rho) / dx_ + (fyT.rho - fyB.rho) / dy_);
            n.mx = c.mx - dt * ((fxR.mx - fxL.mx) / dx_ + (fyT.mx - fyB.mx) / dy_);
            n.my = c.my - dt * ((fxR.my - fxL.my) / dx_ + (fyT.my - fyB.my) / dy_);
            n.energy = c.energy - dt * ((fxR.energy - fxL.energy) / dx_ + (fyT.energy - fyB.energy) / dy_);
            n.exhaust = c.exhaust - dt * ((fxR.exhaust - fxL.exhaust) / dx_ + (fyT.exhaust - fyB.exhaust) / dy_);

            if (!std::isfinite(n.rho) || !std::isfinite(n.energy) || n.rho < 1.0e-5f) n = c;
            const float kinetic = 0.5f * (n.mx * n.mx + n.my * n.my) / std::max(n.rho, 1.0e-5f);
            n.energy = std::max(n.energy, kinetic + 30.0f / (activeParameters_.gamma - 1.0f));
            n.exhaust = std::clamp(n.exhaust, 0.0f, n.rho);

            // Maintain a small reservoir safely inside the sealed chamber.
            // Unlike a ghost-face inlet, this cannot inject through an exterior
            // stair-step cell at the back-wall/nozzle-wall junction.
            const float chamberStart = -0.10f - activeParameters_.chamberLengthM;
            const float wallThickness = std::min(1.55f * std::max(dx_, dy_),
                                                 0.22f * activeParameters_.throatRadiusM);
            const float wx = worldX(x);
            const bool reservoir = wx > chamberStart + 2.0f * wallThickness &&
                                   wx < chamberStart + 2.0f * wallThickness + 4.0f * dx_ &&
                                   std::abs(worldY(y)) < activeParameters_.chamberRadiusM - 2.0f * wallThickness;
            if (reservoir) {
                const float gasR = kRu / (reservoirParameters.molarMassGPerMol * 0.001f);
                const float rho = reservoirParameters.chamberPressureMPa * 1.0e6f /
                                  (gasR * reservoirParameters.chamberTemperatureK);
                n = stateFromPrimitive(rho, 4.0f, 0.0f,
                                       reservoirParameters.chamberPressureMPa * 1.0e6f, 1.0f);
            }
            if (secondRungeKuttaStage_) {
                const Cell& original = stageBase_[i];
                n.rho = 0.5f * (original.rho + n.rho);
                n.mx = 0.5f * (original.mx + n.mx);
                n.my = 0.5f * (original.my + n.my);
                n.energy = 0.5f * (original.energy + n.energy);
                n.exhaust = 0.5f * (original.exhaust + n.exhaust);
            }
            next_[i] = n;
        }
    }
}

void FlowSolver::advance(float dt) {
    stageBase_ = cells_;
    const auto runStage = [&] {
        rebuildPrimitiveCache();
        if (workers_.empty()) {
            computeFluxRows(0, height_);
            updateRows(dt, 0, height_);
        } else {
            parallelDt_ = dt;
            parallelPhase_ = 0;
            workerStart_->arrive_and_wait();
            workerFinish_->arrive_and_wait();
            parallelPhase_ = 1;
            workerStart_->arrive_and_wait();
            workerFinish_->arrive_and_wait();
        }
        cells_.swap(next_);
    };
    secondRungeKuttaStage_ = false;
    runStage();
    secondRungeKuttaStage_ = true;
    runStage();
    secondRungeKuttaStage_ = false;
    diagnostics_.simulationTimeMs += dt * 1000.0f;
    ++diagnostics_.iteration;
}

void FlowSolver::singleStep(const Parameters& parameters) {
    activeParameters_ = parameters;
    const auto start = std::chrono::steady_clock::now();
    if (gpu_) advanceGpuSteps(1, parameters);
    else advance(stableTimeStep());
    updateDiagnostics();
    diagnostics_.stepMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void FlowSolver::advanceSteps(int count, const Parameters& parameters) {
    activeParameters_ = parameters;
    const auto start = std::chrono::steady_clock::now();
    if (gpu_) advanceGpuSteps(count, parameters);
    else {
        int remaining = std::max(count, 0);
        while (remaining > 0) {
            const int chunk = std::min(remaining, 32);
            const float dt = stableTimeStep();
            for (int i = 0; i < chunk; ++i) advance(dt);
            remaining -= chunk;
        }
    }
    updateDiagnostics();
    diagnostics_.stepMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void FlowSolver::step(float realDeltaSeconds, const Parameters& parameters) {
    activeParameters_ = parameters;
    if (paused_) return;
    const auto start = std::chrono::steady_clock::now();
    if (gpu_) {
        const int steps = std::clamp(static_cast<int>(std::round(parameters.liveStepBudget * parameters.timeScale)), 1, 32);
        advanceGpuSteps(steps, parameters);
        updateDiagnostics();
        diagnostics_.stepMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        status_ = "Axisymmetric second-order HLLC running on Vulkan compute";
        return;
    }
    timeAccumulator_ += std::min(realDeltaSeconds, 0.05f) * std::clamp(parameters.timeScale, 0.05f, 5.0f);
    int substeps = 0;
    const float dt = stableTimeStep();
    const int maxSubsteps = std::clamp(parameters.liveStepBudget, 1, 20);
    while (timeAccumulator_ > 0.0f && substeps < maxSubsteps) {
        advance(dt);
        timeAccumulator_ -= dt;
        ++substeps;
    }
    if (substeps == maxSubsteps) timeAccumulator_ = 0.0f;
    updateDiagnostics();
    diagnostics_.stepMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    status_ = substeps == maxSubsteps ? "Solver running at the live screen-update budget" : "Conservative HLLC update stable";
}

void FlowSolver::updateDiagnostics() {
    diagnostics_.maxMach = 0.0f;
    diagnostics_.minTemperatureK = std::numeric_limits<float>::max();
    diagnostics_.maxTemperatureK = 0.0f;
    constexpr float chamberEnd = -0.10f;
    const float exitX = chamberEnd + activeParameters_.convergingLengthM + activeParameters_.divergingLengthM;
    int exitColumn = std::clamp(static_cast<int>((exitX - xMin_) / dx_), 1, width_ - 2);
    float mdotPlanar = 0.0f, thrustPlanar = 0.0f, machSum = 0.0f, pressureSum = 0.0f;
    int exitSamples = 0;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            if (!isFluid(x, y)) continue;
            const Primitive q = primitive(cells_[index(x, y)]);
            diagnostics_.maxMach = std::max(diagnostics_.maxMach, std::hypot(q.u, q.v) / q.soundSpeed);
            diagnostics_.minTemperatureK = std::min(diagnostics_.minTemperatureK, q.temperature);
            diagnostics_.maxTemperatureK = std::max(diagnostics_.maxTemperatureK, q.temperature);
        }
        if (std::abs(worldY(y)) <= activeParameters_.exitRadiusM && isFluid(exitColumn, y)) {
            const Primitive q = primitive(cells_[index(exitColumn, y)]);
            // Axisymmetric annular integration: 2*pi*r*dr for the mirrored cross-section.
            const float ringArea = 2.0f * 3.14159265f * std::abs(worldY(y)) * dy_;
            mdotPlanar += std::max(q.rho * q.u, 0.0f) * ringArea;
            thrustPlanar += (std::max(q.rho * q.u, 0.0f) * q.u + q.pressure - activeParameters_.ambientPressureKPa * 1000.0f) * ringArea;
            machSum += std::hypot(q.u, q.v) / q.soundSpeed;
            pressureSum += q.pressure;
            ++exitSamples;
        }
    }
    diagnostics_.massFlowKgPerS = mdotPlanar;
    diagnostics_.thrustKN = thrustPlanar * 0.001f;
    diagnostics_.specificImpulseS = mdotPlanar > 0.01f ? thrustPlanar / (mdotPlanar * kGravity) : 0.0f;
    diagnostics_.exitMach = exitSamples ? machSum / exitSamples : 0.0f;
    diagnostics_.exitPressureKPa = exitSamples ? pressureSum / exitSamples * 0.001f : 0.0f;
}

std::array<float, 4> FlowSolver::colorAt(int x, int y, FieldView view) const {
    if (!isFluid(x, y)) return {0.48f, 0.56f, 0.67f, 1.0f};
    const Primitive q = primitive(cells_[index(x, y)]);
    const float mach = std::hypot(q.u, q.v) / q.soundSpeed;
    if (view == FieldView::Schlieren) {
        const auto logDensity = [&](int sx, int sy) {
            if (!isFluid(sx, sy)) return std::log(std::max(q.rho, 1.0e-5f));
            return std::log(std::max(primitive(cells_[index(sx, sy)]).rho, 1.0e-5f));
        };
        const float center = logDensity(x, y);
        const float left = logDensity(x - 1, y);
        const float right = logDensity(x + 1, y);
        const float bottom = logDensity(x, y - 1);
        const float top = logDensity(x, y + 1);
        const float gx = 0.5f * (right - left);
        const float gy = 0.5f * (top - bottom);
        const float laplacian = left + right + bottom + top - 4.0f * center;
        const float shock = std::pow(1.0f - std::exp(-10.0f * std::hypot(gx, gy)), 0.72f);
        const float plume = std::pow(q.exhaust, 0.35f);
        // Compression edges are warm and expansion edges are ice blue. This
        // two-sided palette makes each shock cell readable without false neon.
        const float compression = 0.5f + 0.5f * std::tanh(-12.0f * laplacian);
        const std::array<float, 3> expansionColor{0.18f, 0.66f, 1.00f};
        const std::array<float, 3> compressionColor{1.00f, 0.48f, 0.16f};
        const float edgeR = std::lerp(expansionColor[0], compressionColor[0], compression);
        const float edgeG = std::lerp(expansionColor[1], compressionColor[1], compression);
        const float edgeB = std::lerp(expansionColor[2], compressionColor[2], compression);
        return {
            0.016f + 0.095f * plume + shock * edgeR,
            0.028f + 0.145f * plume + shock * edgeG,
            0.055f + 0.255f * plume + shock * edgeB,
            1.0f,
        };
    }
    if (view == FieldView::Pressure) {
        const float ambient = activeParameters_.ambientPressureKPa * 1000.0f;
        constexpr float chamberEnd = -0.10f;
        const float exitX = chamberEnd + activeParameters_.convergingLengthM + activeParameters_.divergingLengthM;
        const float radius = nozzleRadius(worldX(x), activeParameters_);
        const bool insideEngine = worldX(x) < exitX && radius > 0.0f && std::abs(worldY(y)) < radius;
        if (insideEngine) {
            const float span = std::max(std::log(activeParameters_.chamberPressureMPa * 1.0e6f / ambient), 0.1f);
            const float normalized = clamp01(std::log(std::max(q.pressure / ambient, 1.0f)) / span);
            const auto hot = turbo(0.28f + 0.72f * normalized);
            return {hot[0], hot[1], hot[2], 1.0f};
        }
        const float signedRatio = std::clamp(std::log2(std::max(q.pressure / ambient, 0.0625f)), -2.0f, 2.0f);
        const float amount = std::pow(std::abs(signedRatio) * 0.5f, 0.72f);
        const std::array<float, 3> neutral{0.025f, 0.045f, 0.085f};
        const std::array<float, 3> expansion{0.06f, 0.42f, 1.00f};
        const std::array<float, 3> compression{1.00f, 0.25f, 0.055f};
        const auto& target = signedRatio >= 0.0f ? compression : expansion;
        return {std::lerp(neutral[0], target[0], amount),
                std::lerp(neutral[1], target[1], amount),
                std::lerp(neutral[2], target[2], amount), 1.0f};
    }
    if (view == FieldView::Mach) {
        const float t = clamp01(mach / 4.0f);
        const auto pressureRelated = turbo(std::pow(t, 0.82f));
        const std::array<float, 3> cool{0.025f + 0.18f * t, 0.08f + 0.48f * t, 0.22f + 0.62f * t};
        const float blend = 0.22f;
        std::array<float, 3> color{
            std::lerp(pressureRelated[0], cool[0], blend),
            std::lerp(pressureRelated[1], cool[1], blend),
            std::lerp(pressureRelated[2], cool[2], blend),
        };
        if (q.exhaust < 0.01f && mach < 0.04f) color = {0.025f, 0.045f, 0.085f};
        return {color[0], color[1], color[2], 1.0f};
    }
    float t = 0.0f;
    switch (view) {
        case FieldView::Schlieren: break;
        case FieldView::Temperature: t = (q.temperature - 250.0f) / 3400.0f; break;
        case FieldView::Mach: break;
        case FieldView::Pressure: break;
        case FieldView::Density: t = q.rho / 8.0f; break;
        case FieldView::ExhaustFraction: t = q.exhaust; break;
        case FieldView::Velocity: t = std::hypot(q.u, q.v) / 3200.0f; break;
    }
    auto rgb = view == FieldView::ExhaustFraction ? blueRed(t) : turbo(t);
    if (q.exhaust < 0.01f && view != FieldView::Pressure && view != FieldView::Density)
        rgb = {0.035f + 0.06f * t, 0.055f + 0.08f * t, 0.09f + 0.12f * t};
    return {rgb[0], rgb[1], rgb[2], 1.0f};
}

} // namespace rocket
