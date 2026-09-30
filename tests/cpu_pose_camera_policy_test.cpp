#include "model_interpolation.hpp"
#include "model_interpolation_rt64.hpp"
#include "hle/rt64_game_frame.h"
#include "hle/rt64_workload.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

int main() {
    using tooie::model_interpolation::MatrixRange;
    using tooie::model_interpolation::TaskScope;
    constexpr std::uint32_t cpuMatrix = 0x00100000;
    constexpr std::uint32_t ordinaryMatrix = 0x00100040;
    constexpr std::uint32_t invalidMatrix = 0x00800000;

    // The captured Wooded Hollow draw is a CPU-skinned world pose with a
    // stationary world root. Its camera must stay in the scene's smooth group.
    std::vector<MatrixRange> gameplay{{cpuMatrix, ordinaryMatrix, true}};
    {
        TaskScope task(gameplay);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        data.viewProjTransformGroups.push_back(0);
        const auto world = RT64::tooieOriginalPoseGroup(data, 0, cpuMatrix);
        const auto camera = RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix);
        assert(world != 0);
        assert(data.transformGroups[world].matrixId == G_EX_ID_IGNORE);
        assert(camera == 0);
        assert(!RT64::tooieOriginalPoseProjectionChanged(data, 0, 0, cpuMatrix));
        // A following ordinary draw must reuse the unmodified shared camera.
        assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, ordinaryMatrix) == 0);
    }

    // Intro/cutscene CPU poses retain their previously accepted paired guard.
    std::vector<MatrixRange> intro{{cpuMatrix, ordinaryMatrix, false}};
    {
        TaskScope task(intro);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        data.viewProjTransformGroups.push_back(0);
        const auto camera = RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix);
        assert(camera != 0);
        assert(data.transformGroups[camera].matrixId == G_EX_ID_IGNORE);
        data.viewProjTransformGroups.push_back(camera);
        assert(!RT64::tooieOriginalPoseProjectionChanged(data, 1, 0, cpuMatrix));
        assert(RT64::tooieOriginalPoseProjectionChanged(data, 1, 0, ordinaryMatrix));
        assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, ordinaryMatrix) == 0);
    }

    // A verified single actor root has a stable explicit identity. Its pose
    // vertices can interpolate only when the paired CPU geometry proves that
    // their topology, UV ownership, and local displacement are compatible.
    std::vector<MatrixRange> flying{{cpuMatrix, ordinaryMatrix, true, 0x60000001U}};
    RT64::TransformGroup rootGroup;
    {
        TaskScope task(flying);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        data.viewProjTransformGroups.push_back(0);
        const auto world = RT64::tooieOriginalPoseGroup(data, 0, cpuMatrix);
        const auto& group = data.transformGroups[world];
        assert(group.matrixId == 0x60000001U);
        assert(group.positionInterpolation == G_EX_COMPONENT_INTERPOLATE);
        assert(group.rotationInterpolation == G_EX_COMPONENT_INTERPOLATE);
        assert(group.scaleInterpolation == G_EX_COMPONENT_SKIP);
        assert(group.vertexInterpolation == G_EX_COMPONENT_INTERPOLATE);
        assert(group.ordering == G_EX_ORDER_LINEAR);
        assert(!group.decompose);
        assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix) == 0);
        rootGroup = group;

        // The historical one-vertex/no-face fixture deliberately has no
        // geometry proof. It must retain original vertices while its root
        // translation continues to smooth.
        RT64::Workload previous;
        RT64::Workload current;
        previous.drawData.transformGroups.push_back(group);
        current.drawData.transformGroups.push_back(group);
        previous.drawData.worldTransformGroups.push_back(0);
        current.drawData.worldTransformGroups.push_back(0);
        previous.drawData.worldTransformVertexIndices.push_back(0);
        current.drawData.worldTransformVertexIndices.push_back(0);
        previous.drawData.worldIndices.push_back(0);
        current.drawData.worldIndices.push_back(0);
        previous.drawData.posFloats = {1.0f, 2.0f, 3.0f};
        current.drawData.posFloats = {9.0f, 8.0f, 7.0f};
        current.drawData.velFloats = {31.0f, 32.0f, 33.0f};
        auto priorRoot = hlslpp::float4x4::identity();
        auto currentRoot = hlslpp::float4x4::identity();
        currentRoot[0][0] = 0.0f;
        currentRoot[0][1] = 1.0f;
        currentRoot[1][0] = -1.0f;
        currentRoot[1][1] = 0.0f;
        currentRoot[3][0] = 10.0f;
        previous.drawData.worldTransforms.push_back(priorRoot);
        current.drawData.worldTransforms.push_back(currentRoot);

        RT64::GameFrame frame;
        std::multimap<std::uint32_t, std::uint32_t> priorIds, currentIds;
        std::vector<std::uint32_t> ignored;
        frame.buildTransformIdMap(previous, priorIds, ignored);
        frame.buildTransformIdMap(current, currentIds, ignored);
        assert(priorIds.count(group.matrixId) == 1);
        assert(currentIds.count(group.matrixId) == 1);
        assert(priorIds.find(group.matrixId)->second == 0);
        assert(currentIds.find(group.matrixId)->second == 0);

        RT64::GameFrameMap::WorkloadMap map;
        map.transforms.resize(1);
        map.prevTransformsMapped.resize(1);
        RT64::ModifiedBuffers modified;
        frame.matchTransform(current, previous, map, nullptr, 0, 0, modified);
        assert(map.transforms[0].mapped && map.transforms[0].prevTransformIndex == 0);
        assert(map.prevTransformsMapped[0]);
        assert(map.transforms[0].rigidBody.lerpTranslation);
        assert(map.transforms[0].rigidBody.lerpRotation);
        const auto halfway = map.transforms[0].rigidBody.lerp(0.5f,
            priorRoot, currentRoot, true);
        assert(halfway[3][0] == 5.0f);
        // Decomposition is deliberately disabled for this CPU root. RT64's
        // angular fallback linearly blends its full affine 3x3, so this
        // quarter turn has the expected 0.5 midpoint components.
        assert(halfway[0][0] == 0.5f);
        assert(halfway[0][1] == 0.5f);
        assert(halfway[1][0] == -0.5f);
        assert(halfway[1][1] == 0.5f);
        // CPU-root velocity is cleared before correspondence is assessed, so
        // a no-face pose cannot retain a stale shader backstep. The explicit
        // root translation is still matched above.
        assert(modified.positionVelocity && !modified.texcoordVelocity);
        assert((current.drawData.velFloats == std::vector<float>{0.0f, 0.0f, 0.0f}));

        current.drawData.transformGroups[0].matrixId = group.matrixId + 1;
        frame.buildTransformIdMap(current, currentIds, ignored);
        assert(currentIds.count(group.matrixId) == 0);
        assert(currentIds.count(group.matrixId + 1) == 1);
        assert(priorIds.count(group.matrixId + 1) == 0);
    }

    // Exercise RT64's actual explicit LINEAR path with a complete CPU pose
    // triangle. The root moves from 0 to 10 while every local vertex moves
    // one unit; halfway reconstruction must therefore be the local midpoint.
    auto makePoseDrawData = [&](const std::vector<float>& positions,
                                const std::vector<float>& texcoords,
                                const std::vector<std::uint32_t>& faces,
                                const std::vector<std::uint16_t>& worlds =
                                    std::vector<std::uint16_t>{0, 0, 0}) {
        RT64::DrawData data;
        data.transformGroups.push_back(rootGroup);
        data.worldTransformGroups.push_back(0);
        data.worldTransformVertexIndices.push_back(0);
        data.worldIndices = worlds;
        data.posFloats = positions;
        data.velFloats.assign(positions.size(), 91.0f);
        data.tcFloats = texcoords;
        data.tcVelFloats.assign(texcoords.size(), 73.0f);
        data.faceIndices = faces;
        data.worldTransforms.push_back(hlslpp::float4x4::identity());
        return data;
    };
    const std::vector<float> priorPositions{
        0.0f, 0.0f, 0.0f,
        4.0f, 0.0f, 0.0f,
        0.0f, 4.0f, 0.0f};
    const std::vector<float> currentPositions{
        1.0f, 0.0f, 0.0f,
        5.0f, 0.0f, 0.0f,
        1.0f, 4.0f, 0.0f};
    const std::vector<float> stableTexcoords{
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f};
    const std::vector<std::uint32_t> stableFaces{0, 1, 2};

    auto matchPose = [](RT64::Workload& current, const RT64::Workload& previous,
                        RT64::GameFrameMap::WorkloadMap& map,
                        RT64::ModifiedBuffers& modified) {
        map.transforms.resize(current.drawData.worldTransforms.size());
        map.prevTransformsMapped.resize(previous.drawData.worldTransforms.size());
        RT64::GameFrame{}.matchTransform(current, previous, map, nullptr, 0, 0, modified);
    };
    {
        RT64::Workload previous;
        RT64::Workload current;
        previous.drawData = makePoseDrawData(priorPositions, stableTexcoords, stableFaces);
        current.drawData = makePoseDrawData(currentPositions, stableTexcoords, stableFaces);
        current.drawData.worldTransforms[0][3][0] = 10.0f;
        RT64::GameFrameMap::WorkloadMap map;
        RT64::ModifiedBuffers modified;
        matchPose(current, previous, map, modified);
        assert(map.transforms[0].mapped);
        assert(map.transforms[0].rigidBody.lerpTranslation);
        assert(map.transforms[0].rigidBody.lerpRotation);
        assert(modified.positionVelocity && !modified.texcoordVelocity);
        assert((current.drawData.velFloats == std::vector<float>{
            1.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f}));
        // The velocity is current-prior, so the shader's half-tick backstep
        // reconstructs the exact local midpoint.
        assert(current.drawData.posFloats[0] - current.drawData.velFloats[0] * 0.5f == 0.5f);
        const auto rootHalf = map.transforms[0].rigidBody.lerp(0.5f,
            previous.drawData.worldTransforms[0], current.drawData.worldTransforms[0], true);
        assert(rootHalf[3][0] == 5.0f);
    }

    auto assertPoseFallback = [&](const std::vector<float>& positions,
                                  const std::vector<float>& texcoords,
                                  const std::vector<std::uint32_t>& faces,
                                  const std::vector<std::uint16_t>& worlds =
                                      std::vector<std::uint16_t>{0, 0, 0}) {
        RT64::Workload previous;
        RT64::Workload current;
        previous.drawData = makePoseDrawData(priorPositions, stableTexcoords, stableFaces, worlds);
        current.drawData = makePoseDrawData(positions, texcoords, faces, worlds);
        current.drawData.worldTransforms[0][3][0] = 10.0f;
        RT64::GameFrameMap::WorkloadMap map;
        RT64::ModifiedBuffers modified;
        matchPose(current, previous, map, modified);
        // Rejecting the local-pose pair cannot regress the independently
        // verified explicit root translation.
        assert(map.transforms[0].mapped && map.transforms[0].rigidBody.lerpTranslation);
        // A rejected pair has its current root-owned span uploaded as zeros:
        // the RT64 vertex path otherwise applies any old velocity even when
        // correspondence declined to generate a new one.
        assert(modified.positionVelocity);
        for (std::size_t vertex = 0; vertex < worlds.size(); ++vertex) {
            const float expected = worlds[vertex] == 0 ? 0.0f : 91.0f;
            for (std::size_t component = 0; component < 3; ++component)
                assert(current.drawData.velFloats[vertex * 3 + component] == expected);
        }
    };
    // A changed index sequence, UV correspondence, mixed matrix ownership,
    // non-finite source, or a discontinuous local jump must not emit velocity.
    assertPoseFallback(currentPositions, stableTexcoords, {0, 2, 1});
    auto changedTexcoords = stableTexcoords;
    changedTexcoords[2] = 0.75f;
    assertPoseFallback(currentPositions, changedTexcoords, stableFaces);
    assertPoseFallback(currentPositions, stableTexcoords, stableFaces, {0, 0, 1});
    auto nanPositions = currentPositions;
    nanPositions[3] = std::numeric_limits<float>::quiet_NaN();
    assertPoseFallback(nanPositions, stableTexcoords, stableFaces);
    auto jumpedPositions = currentPositions;
    jumpedPositions[0] = 257.0f;
    assertPoseFallback(jumpedPositions, stableTexcoords, stableFaces);
    flying[0].interpolate_camera = false;
    {
        TaskScope task(flying);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        const auto camera = RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix);
        assert(data.transformGroups[camera].matrixId == G_EX_ID_IGNORE);
        assert(data.transformGroups[camera].positionInterpolation == G_EX_COMPONENT_SKIP);
    }

    // Missing/invalid source matrices never opt into a different policy.
    RT64::DrawData data;
    data.transformGroups.emplace_back();
    assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, invalidMatrix) == 0);
}
