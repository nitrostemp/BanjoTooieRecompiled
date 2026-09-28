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
        return 0;
    }
    catch (const std::exception &error) {
        std::fprintf(stderr, "RT64 projection preservation: %s\n", error.what());
        return 1;
    }
}
