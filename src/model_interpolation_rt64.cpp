#include "model_interpolation_rt64.hpp"

#include "model_interpolation.hpp"
#include "hle/rt64_workload.h"

namespace RT64 {
namespace {
uint32_t originalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress, bool world);
}

bool tooieOriginalPoseProjectionChanged(const DrawData &data,
    uint32_t projectionIndex, uint32_t originalGroup, uint32_t physicalAddress) {
    if (projectionIndex >= data.viewProjTransformGroups.size() ||
        originalGroup >= data.transformGroups.size()) return false;
    const uint32_t activeGroup = data.viewProjTransformGroups[projectionIndex];
    if (activeGroup >= data.transformGroups.size()) return false;
    const bool pairedCamera = physicalAddress != 0 &&
        tooie::model_interpolation::original_pose_for_matrix(physicalAddress) &&
        !tooie::model_interpolation::original_pose_camera_interpolation_for_matrix(
            physicalAddress);
    // The paired CPU clone has IGNORE/all-SKIP. Gameplay CPU draws and the
    // following ordinary draw reselect the exact shared camera group.
    return pairedCamera ? data.transformGroups[activeGroup].matrixId != G_EX_ID_IGNORE
                        : activeGroup != originalGroup;
}

uint32_t tooieOriginalPoseProjectionGroup(DrawData &data,
    uint32_t originalGroup, uint32_t physicalAddress) {
    if (physicalAddress != 0 &&
        tooie::model_interpolation::original_pose_camera_interpolation_for_matrix(
            physicalAddress)) return originalGroup;
    return originalPoseGroup(data, originalGroup, physicalAddress, false);
}

uint32_t tooieOriginalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress) {
    return originalPoseGroup(data, originalGroup, physicalAddress, true);
}

namespace {
uint32_t originalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress, bool world) {
    if (physicalAddress == 0 ||
        !tooie::model_interpolation::original_pose_for_matrix(physicalAddress) ||
        originalGroup >= data.transformGroups.size()) return originalGroup;

    TransformGroup originalPose = data.transformGroups[originalGroup];
    originalPose.matrixId = G_EX_ID_IGNORE;
    originalPose.positionInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.rotationInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.scaleInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.skewInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.perspectiveInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.vertexInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.texcoordInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.tileInterpolation = G_EX_COMPONENT_SKIP;
    originalPose.lookAtInterpolation = G_EX_COMPONENT_SKIP;
    const auto rootId = world ?
        tooie::model_interpolation::original_pose_root_id_for_matrix(physicalAddress) : 0;
    if (rootId != 0) {
        originalPose.matrixId = rootId;
        originalPose.positionInterpolation = G_EX_COMPONENT_INTERPOLATE;
        // One affine root transforms the entire CPU-deformed mesh coherently.
        // With decomposition off, this blends its complete 3x3 block.
        originalPose.rotationInterpolation = G_EX_COMPONENT_INTERPOLATE;
        // RT64's per-pair CPU policy validates geometry/UV/triangle ownership
        // before generating local pose velocities for these explicit IDs.
        originalPose.vertexInterpolation = G_EX_COMPONENT_INTERPOLATE;
        originalPose.ordering = G_EX_ORDER_LINEAR;
        originalPose.decompose = false;
    }
    const uint32_t clonedIndex = uint32_t(data.transformGroups.size());
    data.transformGroups.emplace_back(originalPose);
    return clonedIndex;
}
}

} // namespace RT64
