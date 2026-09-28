#pragma once

#include <cstdint>

namespace RT64 {
struct DrawData;

// Return the existing transform-group index unless this guest model Mtx belongs
// to a task-scoped CPU-skinned draw. Such a draw receives a fresh original-pose
// group clone for its model or projection; the RSP's shared stacks stay intact.
uint32_t tooieOriginalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress);

// CPU-skinned vertices kept at the current guest pose must use that guest
// camera too. Split only their projection; leave the shared camera stack alone.
bool tooieOriginalPoseProjectionChanged(const DrawData &data,
    uint32_t projectionIndex, uint32_t originalGroup, uint32_t physicalAddress);
}
