#include "FlowSolver.h"

#include <cmath>
#include <iostream>

int main() {
    rocket::Parameters parameters;
    rocket::Parameters hardStart = parameters;
    hardStart.hardStartEnabled = true;
    const rocket::Parameters beforeIgnition = rocket::transientReservoirParameters(hardStart, 0.0f);
    const rocket::Parameters atPressurePeak = rocket::transientReservoirParameters(
        hardStart, hardStart.ignitionDelayMs + hardStart.ignitionRiseMs);
    const rocket::Parameters afterDecay = rocket::transientReservoirParameters(
        hardStart, hardStart.ignitionDelayMs + hardStart.ignitionRiseMs + 20.0f * hardStart.hardStartDecayMs);
    const bool hardStartProfile = beforeIgnition.chamberPressureMPa < parameters.chamberPressureMPa &&
        std::abs(atPressurePeak.chamberPressureMPa - parameters.chamberPressureMPa *
                 hardStart.hardStartPressureRatio) < 1.0e-4f &&
        std::abs(afterDecay.chamberPressureMPa - parameters.chamberPressureMPa) < 1.0e-3f;
    rocket::FlowSolver solver(180, 80);
    float initialNozzleExhaust = 0.0f;
    for (int y = 0; y < solver.height(); ++y) {
        for (int x = 0; x < solver.width(); ++x) {
            if (solver.worldX(x) > 0.55f)
                initialNozzleExhaust = std::max(initialNozzleExhaust, solver.exhaustFractionAt(x, y));
        }
    }
    for (int i = 0; i < 500; ++i) solver.singleStep(parameters);
    const float establishedMassFlow = solver.diagnostics().massFlowKgPerS;
    float upstreamExteriorLeak = 0.0f;
    float leakX = 0.0f, leakY = 0.0f;
    const float chamberStart = -0.10f - parameters.chamberLengthM;
    for (int y = 0; y < solver.height(); ++y) {
        for (int x = 0; x < solver.width(); ++x) {
            if (solver.worldX(x) < chamberStart - 0.02f &&
                std::abs(solver.worldY(y)) < parameters.chamberRadiusM + 0.05f &&
                solver.exhaustFractionAt(x, y) > upstreamExteriorLeak) {
                upstreamExteriorLeak = solver.exhaustFractionAt(x, y);
                leakX = solver.worldX(x);
                leakY = solver.worldY(y);
            }
        }
    }
    // Keep a compact CPU fallback check; long-duration coverage runs on the
    // production Vulkan backend in gpu_solver_smoke.
    for (int i = 0; i < 500; ++i) solver.singleStep(parameters);

    const rocket::Diagnostics& d = solver.diagnostics();
    const bool finite = std::isfinite(d.maxMach) && std::isfinite(d.massFlowKgPerS) &&
                        std::isfinite(d.thrustKN) && std::isfinite(d.minTemperatureK) &&
                        std::isfinite(d.maxTemperatureK);
    const bool physicalBounds = d.minTemperatureK > 0.0f && d.maxTemperatureK < 20000.0f &&
                                d.maxMach >= 0.0f && d.maxMach < 20.0f && d.massFlowKgPerS >= 0.0f;
    const bool sustainedFeed = establishedMassFlow > 1.0f && d.massFlowKgPerS > establishedMassFlow * 0.40f;
    const bool sealedChamber = upstreamExteriorLeak < 1.0e-3f;
    const bool nozzleStartsEmpty = initialNozzleExhaust < 1.0e-6f;
    std::cout << "iterations=" << d.iteration << " maxMach=" << d.maxMach
              << " massFlow=" << d.massFlowKgPerS << " thrustKN=" << d.thrustKN
              << " exitMach=" << d.exitMach << " exitPressureKPa=" << d.exitPressureKPa
              << " temperatureK=[" << d.minTemperatureK << ", " << d.maxTemperatureK << "]\n";
    std::cout << " establishedMassFlow=" << establishedMassFlow << " sustained=" << sustainedFeed
              << " upstreamLeak=" << upstreamExteriorLeak << " sealed=" << sealedChamber << '\n';
    std::cout << "initialNozzleExhaust=" << initialNozzleExhaust
              << " nozzleStartsEmpty=" << nozzleStartsEmpty << '\n';
    std::cout << "leakLocation=(" << leakX << ", " << leakY << ")\n";
    std::cout << "hardStartPressureMPa=[" << beforeIgnition.chamberPressureMPa << ", "
              << atPressurePeak.chamberPressureMPa << ", " << afterDecay.chamberPressureMPa
              << "] profile=" << hardStartProfile << '\n';
    rocket::FlowSolver detailedSolver;
    detailedSolver.step(1.0f / 60.0f, parameters);
    std::cout << "detailedGrid=" << detailedSolver.width() << 'x' << detailedSolver.height()
              << " frameBudgetMs=" << detailedSolver.diagnostics().stepMilliseconds << '\n';
    return finite && physicalBounds && sustainedFeed && sealedChamber && nozzleStartsEmpty &&
           hardStartProfile ? 0 : 1;
}
