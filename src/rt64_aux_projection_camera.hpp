#pragma once

#include "rt64_aux_projection_policy.hpp"
#include "hle/rt64_workload_queue.h"
#include <limits>
#include <vector>

namespace RT64 {

// Mirror ProjectionProcessor's effective aspect decision for each source
// projection. AUTO depends on this projection's viewport and framebuffer pair.
inline bool tooieAuxiliaryAspectAdjusted(const DrawData &data,
    const Projection &projection, const FramebufferPair &pair, bool &adjusted) {
    const uint32_t index = projection.transformsIndex;
    if (index >= data.viewProjTransformGroups.size() ||
        data.viewProjTransformGroups[index] >= data.transformGroups.size() ||
        index >= data.viewportOrigins.size()) return false;
    const auto mode = data.transformGroups[data.viewProjTransformGroups[index]].aspectMode;
    adjusted = mode == G_EX_ASPECT_ADJUST;
    if (mode != G_EX_ASPECT_AUTO) return true;
    FixedRect intersection = projection.scissorRect;
    if (projection.usesViewport()) {
        if (index >= data.rspViewports.size() ||
            index >= data.viewportClipRatios.size() / 4) return false;
        intersection = intersection.intersection(data.rspViewports[index].rect(
            &data.viewportClipRatios[size_t(index) * 4]));
    }
    if (!intersection.isEmpty()) {
        const bool coversWidth = intersection.ulx <= pair.scissorRect.ulx &&
            intersection.lrx >= pair.scissorRect.lrx;
        const bool horizontal = intersection.width(true, true) >
            intersection.height(true, true);
        adjusted = data.viewportOrigins[index] == G_EX_ORIGIN_NONE &&
            coversWidth && horizontal;
    }
    return true;
}

inline bool tooieAuxiliaryProjectionWithCameraDelta(
    const interop::float4x4 &auxiliaryCurrent,
    const interop::float4x4 &mainCurrent,
    const interop::float4x4 &mainInterpolated,
    interop::float4x4 &result) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const double value = double(auxiliaryCurrent[row][col]) +
                double(mainInterpolated[row][col]) - double(mainCurrent[row][col]);
            if (!std::isfinite(value) ||
                std::abs(value) > double(std::numeric_limits<float>::max())) return false;
            result[row][col] = float(value);
        }
    }
    return true;
}

// A rejected auxiliary history may belong to another object. Keep the current
// object's translation and borrow only the main camera's presentation delta.
// Run after all scenes so draw order cannot affect the selected camera.
inline void tooieApplyAuxiliaryCameraFallback(Workload &workload,
    const GameFrameMap::WorkloadMap &map, float aspectRatioScale) {
    if (!map.mapped || workload.debuggerCamera.enabled ||
        workload.fbPairCount > workload.fbPairs.size() ||
        !std::isfinite(aspectRatioScale) || aspectRatioScale <= 0.0f) return;
    auto &data = workload.drawData;
    const auto valid = [&](uint32_t index) {
        return index > 0 && index < data.viewTransforms.size() &&
            index < data.projTransforms.size() && index < data.viewProjTransforms.size() &&
            index < data.modViewTransforms.size() && index < data.modProjTransforms.size() &&
            index < data.modViewProjTransforms.size() && index < data.prevViewTransforms.size() &&
            index < data.prevViewProjTransforms.size() && index < data.prevProjTransforms.size() &&
            index < data.viewportOrigins.size() &&
            index < data.viewProjTransformGroups.size() && index < map.viewProjections.size() &&
            data.viewProjTransformGroups[index] < data.transformGroups.size();
    };
    const Projection *main = nullptr;
    const FramebufferPair *mainPair = nullptr;
    for (uint32_t pairIndex = 0; pairIndex < workload.fbPairCount; ++pairIndex) {
        const auto &pair = workload.fbPairs[pairIndex];
        if (pair.projectionCount > pair.projections.size()) continue;
        for (uint32_t p = 0; p < pair.projectionCount; ++p) {
            const auto &projection = pair.projections[p];
            if (projection.type != Projection::Type::Perspective ||
                projection.scissorRect.isNull() || !valid(projection.transformsIndex)) continue;
            if (!main || projection.gameCallCount > main->gameCallCount) {
                main = &projection;
                mainPair = &pair;
            }
        }
    }
    if (!main) return;
    const uint32_t camera = main->transformsIndex;
    const auto &mainMap = map.viewProjections[camera];
    if (!mainMap.mapped ||
        data.transformGroups[data.viewProjTransformGroups[camera]].matrixId == G_EX_ID_IGNORE ||
        (!mainMap.rigidBody.lerpTranslation && !mainMap.rigidBody.lerpRotation)) return;
    bool mainAspectAdjusted = false;
    if (!tooieAuxiliaryAspectAdjusted(data, *main, *mainPair, mainAspectAdjusted)) return;
    std::vector<bool> processed(data.viewTransforms.size(), false);
    for (uint32_t pairIndex = 0; pairIndex < workload.fbPairCount; ++pairIndex) {
        const auto &pair = workload.fbPairs[pairIndex];
        if (pair.projectionCount > pair.projections.size()) continue;
        for (uint32_t p = 0; p < pair.projectionCount; ++p) {
            const auto &projection = pair.projections[p];
            const uint32_t index = projection.transformsIndex;
            if (index == camera || projection.type != Projection::Type::Perspective ||
                projection.scissorRect.isNull() || !valid(index) || processed[index] || map.viewProjections[index].mapped ||
                data.transformGroups[data.viewProjTransformGroups[index]].matrixId != G_EX_ID_AUTO ||
                data.viewportOrigins[index] != data.viewportOrigins[camera]) continue;
            tooie::rt64_match::ViewTranslation offset{};
            bool auxiliaryAspectAdjusted = false;
            if (!tooie::rt64_match::relative_auxiliary_translation(
                    data.viewTransforms[index], data.viewTransforms[camera], offset) ||
                !tooie::rt64_match::same_projection_lens(data.projTransforms[index], data.projTransforms[camera]) ||
                !tooieAuxiliaryAspectAdjusted(data, projection, pair, auxiliaryAspectAdjusted) ||
                auxiliaryAspectAdjusted != mainAspectAdjusted) continue;

            // The normal unmapped path has already applied aspect adjustment
            // while retaining the original fixed-point combined matrix.
            const interop::float4x4 auxiliaryCurrent = data.modViewProjTransforms[index];
            interop::float4x4 mainCurrent = data.viewProjTransforms[camera];
            const float aspect = auxiliaryAspectAdjusted ? 1.0f / aspectRatioScale : 1.0f;
            if (!std::isfinite(aspect)) continue;
            for (int row = 0; row < 4; ++row) mainCurrent[row][0] *= aspect;
            interop::float4x4 mainCurrentProjection = data.projTransforms[camera];
            for (int row = 0; row < 4; ++row) mainCurrentProjection[row][0] *= aspect;
            interop::float4x4 combined, previousCombined, view, previousView;
            interop::float4x4 interpolatedProjection, previousProjection;
            if (!tooie::rt64_match::compose_relative_camera_delta(auxiliaryCurrent, mainCurrent,
                    data.modViewProjTransforms[camera], offset, combined) ||
                !tooie::rt64_match::compose_relative_camera_delta(auxiliaryCurrent, mainCurrent,
                    data.prevViewProjTransforms[camera], offset, previousCombined) ||
                !tooie::rt64_match::compose_relative_auxiliary_view(data.modViewTransforms[camera], offset, view) ||
                !tooie::rt64_match::compose_relative_auxiliary_view(data.prevViewTransforms[camera], offset, previousView) ||
                !tooieAuxiliaryProjectionWithCameraDelta(data.modProjTransforms[index], mainCurrentProjection,
                    data.modProjTransforms[camera], interpolatedProjection) ||
                !tooieAuxiliaryProjectionWithCameraDelta(data.modProjTransforms[index], mainCurrentProjection,
                    data.prevProjTransforms[camera], previousProjection)) continue;
            data.modViewProjTransforms[index] = combined;
            data.prevViewProjTransforms[index] = previousCombined;
            data.modViewTransforms[index] = view;
            data.prevViewTransforms[index] = previousView;
            data.modProjTransforms[index] = interpolatedProjection;
            data.prevProjTransforms[index] = previousProjection;
            processed[index] = true;
        }
    }
}

} // namespace RT64
