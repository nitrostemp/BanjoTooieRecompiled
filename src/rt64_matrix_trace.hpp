#pragma once

#include <cstdint>
#include <filesystem>

namespace RT64 {
struct GameFrame;
struct WorkloadQueue;

// Opt-in diagnostic: the next issue marker arms a bounded screen-X trace.
void tooieScreenXTraceSetLogDirectory(const std::filesystem::path &directory) noexcept;
void tooieScreenXTraceArm() noexcept;

// Diagnostic only: observes the CPU matrices selected for presentation.
// Inert unless TOOIE_MATRIX_TRACE=1 is set before process startup.
void tooieMatrixTraceFrame(const WorkloadQueue &queue, const GameFrame &curFrame,
    const GameFrame &prevFrame, float curFrameWeight, float prevFrameWeight,
    bool projectionsProcessed, bool transformsProcessed) noexcept;
}
