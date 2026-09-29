#include "model_interpolation_rt64.hpp"

#include "model_interpolation.hpp"
#include "hle/rt64_workload.h"

namespace RT64 {

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
    return tooieOriginalPoseGroup(data, originalGroup, physicalAddress);
}

uint32_t tooieOriginalPoseGroup(DrawData &data, uint32_t originalGroup,
    uint32_t physicalAddress) {
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
    const uint32_t clonedIndex = uint32_t(data.transformGroups.size());
    data.transformGroups.emplace_back(originalPose);
    return clonedIndex;
}

} // namespace RT64
