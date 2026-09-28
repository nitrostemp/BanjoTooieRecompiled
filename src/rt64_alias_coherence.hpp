#pragma once

#include "hle/rt64_workload_queue.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace RT64 {
namespace {

bool tooieSameMatrix(const hlslpp::float4x4 &a, const hlslpp::float4x4 &b) noexcept {
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            if (a[row][column] != b[row][column]) return false;
    return true;
}

bool tooieSameMatrixPolicy(const TransformGroup &a, const TransformGroup &b) noexcept {
    return a.matrixId == b.matrixId && a.decompose == b.decompose &&
        a.positionInterpolation == b.positionInterpolation &&
        a.rotationInterpolation == b.rotationInterpolation &&
        a.scaleInterpolation == b.scaleInterpolation &&
        a.skewInterpolation == b.skewInterpolation &&
        a.perspectiveInterpolation == b.perspectiveInterpolation &&
        a.vertexInterpolation == b.vertexInterpolation;
}

bool tooieSameRigidBodyFlags(const RigidBody &a, const RigidBody &b) noexcept {
    return a.lerpTranslation == b.lerpTranslation &&
        a.lerpRotation == b.lerpRotation && a.lerpScale == b.lerpScale &&
        a.lerpSkew == b.lerpSkew && a.lerpPerspective == b.lerpPerspective &&
        a.lerpDecompose == b.lerpDecompose;
}

} // namespace

// A single source Mtx may be submitted in multiple calls with different local
// vertex sequences. After ordinary matching is finished, keep its duplicate
// world entries on the same interpolation timeline only when the RT64 values
// prove the alias unambiguously. This does not infer an object from position or
// relax the AUTO geometry-identity guard.
inline void tooieRepairSourceMatrixAliases(WorkloadQueue &queue, GameFrame &frame) {
    for (uint32_t workloadIndex : frame.workloads) {
        if (workloadIndex >= queue.workloads.size() ||
            workloadIndex >= frame.frameMap.workloads.size()) continue;
        auto &workloadMap = frame.frameMap.workloads[workloadIndex];
        if (!workloadMap.mapped || workloadMap.prevWorkloadIndex >= queue.workloads.size()) continue;
        const DrawData &curData = queue.workloads[workloadIndex].drawData;
        const DrawData &prevData = queue.workloads[workloadMap.prevWorkloadIndex].drawData;
        const size_t count = curData.worldTransforms.size();
        if (workloadMap.transforms.size() < count ||
            curData.worldTransformPhysicalAddresses.size() < count ||
            curData.worldTransformGroups.size() < count) continue;

        std::vector<uint8_t> originallyMapped(count);
        std::vector<std::pair<uint32_t, size_t>> donorsByAddress;
        donorsByAddress.reserve(count);
        for (size_t i = 0; i < count; ++i)
            if ((originallyMapped[i] = workloadMap.transforms[i].mapped) &&
                curData.worldTransformPhysicalAddresses[i] != 0)
                donorsByAddress.emplace_back(curData.worldTransformPhysicalAddresses[i], i);
        std::sort(donorsByAddress.begin(), donorsByAddress.end());

        for (size_t target = 0; target < count; ++target) {
            if (originallyMapped[target] ||
                curData.worldTransformPhysicalAddresses[target] == 0 ||
                target >= curData.worldTransformVertexIndices.size() ||
                curData.worldTransformVertexCount(uint32_t(target)) == 0) continue;
            const uint32_t targetGroupIndex = curData.worldTransformGroups[target];
            if (targetGroupIndex >= curData.transformGroups.size()) continue;
            const TransformGroup &targetPolicy = curData.transformGroups[targetGroupIndex];
            // Preserve explicit native directives, especially the CPU-skinned
            // original-pose fallback's IGNORE/all-SKIP clone.
            if (targetPolicy.matrixId != G_EX_ID_AUTO ||
                targetPolicy.vertexInterpolation != G_EX_COMPONENT_SKIP ||
                targetPolicy.positionInterpolation == G_EX_COMPONENT_SKIP ||
                targetPolicy.rotationInterpolation == G_EX_COMPONENT_SKIP ||
                targetPolicy.scaleInterpolation == G_EX_COMPONENT_SKIP ||
                targetPolicy.skewInterpolation == G_EX_COMPONENT_SKIP ||
                targetPolicy.perspectiveInterpolation == G_EX_COMPONENT_SKIP) continue;

            size_t chosen = count;
            bool ambiguous = false;
            const uint32_t physicalAddress = curData.worldTransformPhysicalAddresses[target];
            const auto firstDonor = std::lower_bound(donorsByAddress.begin(), donorsByAddress.end(),
                std::pair<uint32_t, size_t>{physicalAddress, 0});
            for (auto it = firstDonor; it != donorsByAddress.end() && it->first == physicalAddress; ++it) {
                const size_t donor = it->second;
                if (!tooieSameMatrix(curData.worldTransforms[donor], curData.worldTransforms[target]))
                    continue;
                const uint32_t donorGroupIndex = curData.worldTransformGroups[donor];
                const auto &donorMap = workloadMap.transforms[donor];
                if (donorGroupIndex >= curData.transformGroups.size() ||
                    !tooieSameMatrixPolicy(targetPolicy, curData.transformGroups[donorGroupIndex]) ||
                    donorMap.prevTransformIndex >= prevData.worldTransforms.size()) {
                    ambiguous = true;
                    break;
                }
                if (chosen == count) {
                    chosen = donor;
                    continue;
                }
                const auto &chosenMap = workloadMap.transforms[chosen];
                const auto &previous = prevData.worldTransforms[donorMap.prevTransformIndex];
                const auto &chosenPrevious = prevData.worldTransforms[chosenMap.prevTransformIndex];
                if (!tooieSameMatrix(previous, chosenPrevious) ||
                    !tooieSameRigidBodyFlags(donorMap.rigidBody, chosenMap.rigidBody) ||
                    !tooieSameMatrix(
                        donorMap.rigidBody.lerp(0.5f, previous, curData.worldTransforms[donor], true),
                        chosenMap.rigidBody.lerp(0.5f, chosenPrevious, curData.worldTransforms[chosen], true))) {
                    ambiguous = true;
                    break;
                }
            }
            if (!ambiguous && chosen != count)
                workloadMap.transforms[target] = workloadMap.transforms[chosen];
            // prevTransformsMapped is intentionally unchanged: the donor's
            // accepted prior matrix already owns that guest-source timeline.
        }
    }
}

} // namespace RT64
