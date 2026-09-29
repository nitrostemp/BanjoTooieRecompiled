#pragma once

#include <cstdint>

namespace RT64 {
struct DrawData;

// Return the existing transform-group index unless this guest model Mtx belongs
// to a task-scoped CPU-skinned draw. Such a draw receives a fresh original-pose
// group clone for its model or projection; the RSP's shared stacks stay intact.
uint32_t tooieOriginalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress);

// World and local CPU pose stay at the guest sample in either case. Ordinary
// gameplay draws may keep the scene's smooth camera; title/cutscene draws use
// the paired guest camera guard.
uint32_t tooieOriginalPoseProjectionGroup(DrawData &data,
    uint32_t originalGroup, uint32_t physicalAddress);

// Split only a projection whose effective policy differs from the active one;
// the next ordinary model restores the untouched shared camera stack.
bool tooieOriginalPoseProjectionChanged(const DrawData &data,
    uint32_t projectionIndex, uint32_t originalGroup, uint32_t physicalAddress);
}
