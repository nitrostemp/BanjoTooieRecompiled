#include "common/rt64_math.h"
#include "hle/rt64_workload_queue.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

// Fixed 16.16 view-projection values observed at two camera angles. The first
// one has visible sky vertices only about two clip units inside the far plane.
// These numbers are geometry-free regression fixtures, not game assets.
const hlslpp::float4x4 bad_view_projection(
    -0.7722930908203125f, -0.0269927978515625f, 0.0133819580078125f, 0.01336669921875f,
    0.0f, 1.107269287109375f, 0.29608154296875f, 0.295928955078125f,
    0.0256500244140625f, -0.8126068115234375f, 0.4029998779296875f, 0.4027862548828125f,
    -6147.169921875f, 479.8936767578125f, 450.934967041015625f, 455.698486328125f);

const hlslpp::float4x4 good_view_projection(
    -0.7722930908203125f, -0.0047607421875f, 0.016510009765625f, 0.0164947509765625f,
    0.0f, 1.3662109375f, 0.0522308349609375f, 0.052215576171875f,
    0.0256500244140625f, -0.1433868408203125f, 0.4972381591796875f, 0.496978759765625f,
    -6147.169921875f, 1045.9385986328125f, 300.516204833984375f, 305.358154296875f);

float clip_z_minus_w(const hlslpp::float4x4 &vp, const std::array<float, 3> &local) {
    // The captured world matrix is an identity basis with this translation.
    const float world[4] = { local[0] - 7968.31005859375f, local[1] - 821.0f,
        local[2] - 263.4635009765625f, 1.0f };
    float z = 0.0f, w = 0.0f;
    for (int row = 0; row < 4; ++row) {
        z += world[row] * vp[row][2];
        w += world[row] * vp[row][3];
    }
    return z - w;
}

void check_case(const hlslpp::float4x4 &source, uint8_t aspect_mode,
    float aspect_ratio_scale, bool check_far_plane) {
    RT64::WorkloadQueue queue;
    RT64::Workload &workload = queue.workloads[0];
    workload.debuggerCamera.enabled = false;
    workload.fbPairs.resize(1);
    workload.fbPairs[0].projections.resize(1);
    RT64::Projection &projection = workload.fbPairs[0].projections[0];
    projection.type = RT64::Projection::Type::Perspective;
    projection.transformsIndex = 1;
    projection.scissorRect = RT64::FixedRect(0, 0, 1280, 960);

    RT64::DrawData &draw = workload.drawData;
    draw.viewTransforms.resize(2);
    draw.projTransforms.resize(2);
    draw.viewProjTransforms.resize(2);
    draw.viewProjTransforms[1] = source;
    hlslpp::float4x4 view, projection_matrix;
    RT64::matrixDecomposeViewProj(source, view, projection_matrix);
    draw.viewTransforms[1] = view;
    draw.projTransforms[1] = projection_matrix;
    draw.viewportOrigins.resize(2);
    draw.viewProjTransformGroups.resize(2);
    draw.transformGroups.resize(1);
    draw.viewProjTransformGroups[1] = 0;
    draw.transformGroups[0].aspectMode = aspect_mode;

    RT64::GameFrame frame;
    frame.workloads.push_back(0);
    frame.frameMap.workloads.resize(1);
    frame.perspectiveScenes.resize(1);
    frame.perspectiveScenes[0].projections.push_back({ 0, 0, 0 });

    RT64::ProjectionProcessor processor;
    RT64::ProjectionProcessor::ProcessParams params;
    params.workloadQueue = &queue;
    params.curFrame = &frame;
    params.aspectRatioScale = aspect_ratio_scale;
    processor.process(params);

    const auto &modified = draw.modViewProjTransforms[1];
    const auto &previous = draw.prevViewProjTransforms[1];
    if (check_far_plane) {
        const std::array<float, 3> sky_vertex = { 5297.0f, 4844.0f, 8374.0f };
        // Original captured clip z-W is about -2.46. Recomposition of the
        // decomposed projection incorrectly makes it positive (+0.28).
        std::fprintf(stderr, "clip z-W: original %.9g, rendered %.9g\n",
            clip_z_minus_w(source, sky_vertex), clip_z_minus_w(modified, sky_vertex));
        if (clip_z_minus_w(source, sky_vertex) >= -2.0f ||
            clip_z_minus_w(modified, sky_vertex) >= -2.0f) {
            throw std::runtime_error("visible sky vertex crossed the far clip plane");
        }
    }
    const float horizontal_scale = (aspect_mode == G_EX_ASPECT_ADJUST)
        ? 1.0f / aspect_ratio_scale : 1.0f;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float expected = source[row][col] * ((col == 0) ? horizontal_scale : 1.0f);
            if (modified[row][col] != expected || previous[row][col] != expected) {
                std::fprintf(stderr, "projection changed at [%d][%d]: source %.9g, modified %.9g, previous %.9g\n",
                    row, col, expected, modified[row][col], previous[row][col]);
                throw std::runtime_error("source combined projection changed");
            }
        }
    }


}

void check_existing_modified_path(bool debugger_camera) {
    RT64::WorkloadQueue queue;
    RT64::Workload &workload = queue.workloads[0];
    workload.debuggerCamera.enabled = debugger_camera;
    workload.fbPairs.resize(1);
    workload.debuggerCamera.sceneIndex = 0;
    workload.debuggerCamera.viewMatrix = hlslpp::float4x4::identity();
    workload.debuggerCamera.projMatrix = hlslpp::float4x4::identity();
    workload.fbPairs[0].projections.resize(1);
    RT64::Projection &projection = workload.fbPairs[0].projections[0];
    projection.type = RT64::Projection::Type::Perspective;
    projection.transformsIndex = 1;
    projection.scissorRect = RT64::FixedRect(0, 0, 1280, 960);

    hlslpp::float4x4 view, projection_matrix;
    RT64::matrixDecomposeViewProj(bad_view_projection, view, projection_matrix);
    RT64::DrawData &draw = workload.drawData;
    draw.viewTransforms.resize(2);
    draw.projTransforms.resize(2);
    draw.viewProjTransforms.resize(2);
    draw.viewTransforms[1] = view;
    draw.projTransforms[1] = projection_matrix;
    draw.viewProjTransforms[1] = bad_view_projection;
    draw.viewportOrigins.resize(2);
    draw.viewProjTransformGroups.resize(2);
    draw.transformGroups.resize(1);
    draw.transformGroups[0].aspectMode = G_EX_ASPECT_STRETCH;

    RT64::GameFrame frame;
    frame.workloads.push_back(0);
    frame.perspectiveScenes.resize(1);
    frame.perspectiveScenes[0].projections.push_back({ 0, 0, 0 });
    frame.frameMap.workloads.resize(1);
    RT64::GameFrame previous_frame;
    if (!debugger_camera) {
        // A mapped projection must retain interpolation and the source
        // combined-matrix residual at its endpoints.
        RT64::DrawData &previous_draw = queue.workloads[1].drawData;
        previous_draw.viewTransforms.resize(2);
        previous_draw.projTransforms.resize(2);
        previous_draw.viewProjTransforms.resize(2);
        previous_draw.viewTransforms[1] = view;
        previous_draw.projTransforms[1] = projection_matrix;
        previous_draw.viewProjTransforms[1] = bad_view_projection;
        auto &mapping = frame.frameMap.workloads[0];
        mapping.mapped = true;
        mapping.prevWorkloadIndex = 1;
        mapping.viewProjections.resize(2);
        mapping.viewProjections[1].mapped = true;
        mapping.viewProjections[1].prevTransformIndex = 1;
        mapping.viewProjections[1].rigidBody.lerpDecompose = false;
    }

    RT64::ProjectionProcessor processor;
    RT64::ProjectionProcessor::ProcessParams params;
    params.workloadQueue = &queue;
    params.curFrame = &frame;
    params.prevFrame = debugger_camera ? nullptr : &previous_frame;
    processor.process(params);
    const hlslpp::float4x4 expected = debugger_camera
        ? hlslpp::float4x4::identity() : bad_view_projection;
    const auto &modified = draw.modViewProjTransforms[1];
    const auto &previous = draw.prevViewProjTransforms[1];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (std::abs(modified[row][col] - expected[row][col]) > 1.0e-5f ||
                std::abs(previous[row][col] - expected[row][col]) > 1.0e-5f) {
                throw std::runtime_error("debugger or matched projection endpoint changed");
            }
        }
    }
}

void check_auxiliary_camera_fallback(float weight, bool ignore, bool mapped,
    bool different_lens, bool original_main, bool changing_lens = false,
    bool different_aspect = false, bool auto_narrow_auxiliary = false) {
    RT64::WorkloadQueue queue;
    auto &workload = queue.workloads[0];
    workload.debuggerCamera.enabled = false;
    workload.fbPairCount = 2;
    workload.fbPairs.resize(2);
    auto &pair = workload.fbPairs[0];
    pair.projectionCount = 2;
    pair.projections.resize(2);
    pair.scissorRect = RT64::FixedRect(0, 0, 1280, 960);
    auto &draw = workload.drawData;
    auto &old = queue.workloads[1].drawData;
    for (auto *data : {&draw, &old}) {
        data->viewTransforms.resize(3);
        data->projTransforms.resize(3);
        data->viewProjTransforms.resize(3);
        data->viewportOrigins.resize(3);
        data->rspViewports.resize(3);
        data->viewportClipRatios.resize(12);
        data->viewProjTransformGroups.resize(3);
        data->transformGroups.resize(2);
        data->viewportOrigins[1] = data->viewportOrigins[2] = G_EX_ORIGIN_NONE;
        for (auto &group : data->transformGroups) {
            group.matrixId = G_EX_ID_AUTO;
            group.aspectMode = G_EX_ASPECT_ADJUST;
        }
        if (different_aspect) data->transformGroups[1].aspectMode = G_EX_ASPECT_STRETCH;
        if (auto_narrow_auxiliary) {
            for (auto &group : data->transformGroups) group.aspectMode = G_EX_ASPECT_AUTO;
            for (uint32_t index = 1; index <= 2; ++index) {
                auto &viewport = data->rspViewports[index];
                viewport.scale = interop::float3(index == 2 ? 80.0f : 160.0f, 120.0f, 1.0f);
                viewport.translate = interop::float3(160.0f, 120.0f, 0.0f);
                for (int coordinate = 0; coordinate < 4; ++coordinate)
                    data->viewportClipRatios[index * 4 + coordinate] = coordinate < 2 ? 1 : -1;
            }
        }
    }
    hlslpp::float4x4 current = hlslpp::float4x4::identity();
    hlslpp::float4x4 previous = current;
    current[3][0] = 20.0f;
    previous[3][0] = -20.0f;
    current[0][0] = current[2][2] = std::cos(0.14f);
    current[0][2] = std::sin(0.14f);
    current[2][0] = -std::sin(0.14f);
    hlslpp::float4x4 relative = hlslpp::float4x4::identity();
    relative[3][0] = -6275.0f;
    relative[3][1] = -924.0f;
    relative[3][2] = 2963.0f;
    hlslpp::float4x4 lens = hlslpp::float4x4::identity();
    lens[0][0] = 1.25f;
    lens[1][1] = 1.5f;
    for (uint32_t index = 1; index <= 2; ++index) {
        auto &projection = pair.projections[index - 1];
        projection.type = RT64::Projection::Type::Perspective;
        projection.transformsIndex = index;
        projection.gameCallCount = index == 1 ? 100 : 1;
        projection.scissorRect = pair.scissorRect;
        draw.viewTransforms[index] = index == 1 ? current : hlslpp::mul(relative, current);
        old.viewTransforms[index] = index == 1 ? previous : hlslpp::mul(relative, previous);
        draw.projTransforms[index] = old.projTransforms[index] = lens;
        if (changing_lens) old.projTransforms[index][1][1] = 1.25f;
        if (index == 2 && different_lens) draw.projTransforms[index][1][1] = 3.0f;
        draw.viewProjTransforms[index] = hlslpp::mul(draw.viewTransforms[index], draw.projTransforms[index]);
        old.viewProjTransforms[index] = hlslpp::mul(old.viewTransforms[index], old.projTransforms[index]);
        if (index == 2) {
            draw.viewProjTransforms[index][2][2] += 0.00002f;
            old.viewProjTransforms[index][2][2] += 0.00002f;
        }
        draw.viewProjTransformGroups[index] = index - 1;
    }
    draw.transformGroups[1].matrixId = ignore ? G_EX_ID_IGNORE : G_EX_ID_AUTO;
    draw.transformGroups[0].matrixId = original_main ? G_EX_ID_IGNORE : G_EX_ID_AUTO;
    RT64::GameFrame frame, previous_frame;
    frame.workloads.push_back(0);
    frame.frameMap.workloads.resize(1);
    auto &map = frame.frameMap.workloads[0];
    map.mapped = true;
    map.prevWorkloadIndex = 1;
    map.viewProjections.resize(3);
    for (uint32_t index = 1; index <= 2; ++index) {
        auto &entry = map.viewProjections[index];
        entry.mapped = index == 1 || mapped;
        entry.prevTransformIndex = index;
        entry.rigidBody.lerpDecompose = false;
        entry.rigidBody.lerpTranslation = true;
        entry.rigidBody.lerpRotation = true;
        entry.rigidBody.updateLinear(old.viewTransforms[index], draw.viewTransforms[index], G_EX_COMPONENT_INTERPOLATE);
    }
    frame.perspectiveScenes.resize(1);
    // Visit the auxiliary first: fallback must not depend on scene order.
    // Captured feathers use a later framebuffer pair than the main scene.
    workload.fbPairs[1].projectionCount = 2;
    workload.fbPairs[1].projections.push_back(pair.projections[1]);
    workload.fbPairs[1].projections.push_back(pair.projections[1]);
    workload.fbPairs[1].scissorRect = pair.scissorRect;
    pair.projectionCount = 1;
    frame.perspectiveScenes[0].projections.push_back({0, 1, 0});
    frame.perspectiveScenes[0].projections.push_back({0, 1, 1});
    frame.perspectiveScenes[0].projections.push_back({0, 0, 0});
    RT64::ProjectionProcessor processor;
    RT64::ProjectionProcessor::ProcessParams params;
    params.workloadQueue = &queue;
    params.curFrame = &frame;
    params.prevFrame = &previous_frame;
    params.curFrameWeight = weight;
    params.prevFrameWeight = 0.25f;
    params.aspectRatioScale = 2.0f;
    processor.process(params);
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const auto &actual = endpoint ? draw.prevViewProjTransforms[2] : draw.modViewProjTransforms[2];
        const bool expected_smooth = !ignore && !different_lens && !original_main &&
            !different_aspect && !auto_narrow_auxiliary;
        hlslpp::float4x4 expected = expected_smooth
            ? hlslpp::mul(relative, endpoint ? draw.prevViewProjTransforms[1] : draw.modViewProjTransforms[1])
            : hlslpp::float4x4(draw.viewProjTransforms[2]);
        if (!expected_smooth && !different_aspect && !auto_narrow_auxiliary)
            for (int row = 0; row < 4; ++row) expected[row][0] *= 0.5f;
        if (expected_smooth) expected[2][2] += 0.00002f;
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
            if (std::abs(actual[row][col] - expected[row][col]) > 0.001f)
                throw std::runtime_error("auxiliary lost coherent camera interpolation or bypassed opt-out");
        }
        if (expected_smooth) {
            const auto &actualProjection = endpoint ? draw.prevProjTransforms[2] : draw.modProjTransforms[2];
            const auto &mainProjection = endpoint ? draw.prevProjTransforms[1] : draw.modProjTransforms[1];
            for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
                if (std::abs(actualProjection[row][col] - mainProjection[row][col]) > 0.001f)
                    throw std::runtime_error("auxiliary lens did not follow the mapped camera lens");
            }
        }
    }
}

} // namespace

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    try {
        check_case(bad_view_projection, G_EX_ASPECT_STRETCH, 1.0f, true);
        check_case(bad_view_projection, G_EX_ASPECT_STRETCH, 4.0f / 3.0f, true);
        check_case(bad_view_projection, G_EX_ASPECT_ADJUST, 4.0f / 3.0f, true);
        check_case(good_view_projection, G_EX_ASPECT_STRETCH, 1.0f, false);
        check_case(good_view_projection, G_EX_ASPECT_STRETCH, 4.0f / 3.0f, false);
        check_case(good_view_projection, G_EX_ASPECT_ADJUST, 4.0f / 3.0f, false);
        check_existing_modified_path(true);
        check_existing_modified_path(false);
        for (float weight : {0.0f, 0.5f, 1.0f})
            check_auxiliary_camera_fallback(weight, false, false, false, false);
        // A current auxiliary lens matches the current main lens even while
        // the mapped main projection interpolates from the previous lens.
        check_auxiliary_camera_fallback(0.25f, false, false, false, false, true);
        check_auxiliary_camera_fallback(0.5f, true, false, false, false);
        check_auxiliary_camera_fallback(0.5f, false, false, true, false);
        check_auxiliary_camera_fallback(0.5f, false, false, false, true);
        check_auxiliary_camera_fallback(0.5f, false, true, false, false);
        check_auxiliary_camera_fallback(0.5f, false, false, false, false, false, true);
        check_auxiliary_camera_fallback(0.5f, false, false, false, false, false, false, true);
        return 0;
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "RT64 projection preservation: %s\n", error.what());
        return 1;
    }
}
