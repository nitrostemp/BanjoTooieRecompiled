#pragma once

#include <cstdint>

namespace RT64 {
struct GameFrame;
struct WorkloadQueue;

// Diagnostic only: observes the CPU matrices selected for presentation.
// Inert unless TOOIE_MATRIX_TRACE=1 is set before process startup.
void tooieMatrixTraceFrame(const WorkloadQueue &queue, const GameFrame &curFrame,
    const GameFrame &prevFrame, float curFrameWeight, float prevFrameWeight,
    bool projectionsProcessed, bool transformsProcessed) noexcept;
}
