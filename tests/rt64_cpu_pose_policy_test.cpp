#include "rt64_cpu_pose_policy.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

int main() {
    constexpr std::uint32_t root_id = 0x60000010U;
    auto make = [](std::uint32_t id) {
        RT64::DrawData data;
        RT64::TransformGroup group;
        group.matrixId = id;
        group.ordering = G_EX_ORDER_LINEAR;
        data.transformGroups.push_back(group);
        data.worldTransformGroups.push_back(0);
        data.worldTransformVertexIndices.push_back(0);
        data.worldIndices = {0, 0, 0};
        data.posFloats = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        data.velFloats.resize(data.posFloats.size());
        data.tcFloats = {0, 0, 1, 0, 0, 1};
        data.faceIndices = {0, 1, 2};
        return data;
    };
    auto prior = make(root_id);
    auto current = make(root_id);
    current.posFloats[0] = 10;
    assert(RT64::tooieCpuPoseCorrespondence(current, prior, 0, 0));

    // Root indices can shift globally while local topology stays identical.
    auto shifted = make(root_id);
    shifted.transformGroups.insert(shifted.transformGroups.begin(), RT64::TransformGroup{});
    shifted.worldTransformGroups = {0, 1};
    shifted.worldTransformVertexIndices = {0, 3};
    shifted.worldIndices = {0, 0, 0, 1, 1, 1};
    shifted.posFloats = {8, 8, 8, 9, 9, 9, 7, 7, 7,
        0, 0, 0, 1, 0, 0, 0, 1, 0};
    shifted.tcFloats = {2, 2, 3, 3, 4, 4, 0, 0, 1, 0, 0, 1};
    shifted.velFloats.resize(shifted.posFloats.size());
    shifted.faceIndices = {0, 1, 2, 3, 4, 5};
    assert(RT64::tooieCpuPoseCorrespondence(shifted, prior, 1, 0));
    auto short_velocity = current;
    short_velocity.velFloats.resize(8);
    assert(!RT64::tooieCpuPoseCorrespondence(short_velocity, prior, 0, 0));

    // A stable ID must identify one world transform in each workload, even
    // when an extra transform has no vertices of its own.
    auto duplicate_current = current;
    duplicate_current.worldTransformGroups.push_back(0);
    duplicate_current.worldTransformVertexIndices.push_back(3);
    assert(!RT64::tooieCpuPoseCorrespondence(duplicate_current, prior, 0, 0));
    auto duplicate_previous = prior;
    duplicate_previous.worldTransformGroups.push_back(0);
    duplicate_previous.worldTransformVertexIndices.push_back(3);
    assert(!RT64::tooieCpuPoseCorrespondence(current, duplicate_previous, 0, 0));
    auto duplicate_group = current;
    duplicate_group.transformGroups.push_back(duplicate_group.transformGroups[0]);
    duplicate_group.worldTransformGroups.push_back(1);
    duplicate_group.worldTransformVertexIndices.push_back(3);
    assert(!RT64::tooieCpuPoseCorrespondence(duplicate_group, prior, 0, 0));

    auto different_actor = prior;
    different_actor.transformGroups[0].matrixId = root_id + 1;
    assert(!RT64::tooieCpuPoseCorrespondence(current, different_actor, 0, 0));
    auto changed_order = prior;
    changed_order.faceIndices = {0, 2, 1};
    assert(!RT64::tooieCpuPoseCorrespondence(current, changed_order, 0, 0));
    auto changed_uv = prior;
    changed_uv.tcFloats[0] = 0.5f;
    assert(!RT64::tooieCpuPoseCorrespondence(current, changed_uv, 0, 0));
    auto reordered_vertices = prior;
    std::swap(reordered_vertices.tcFloats[0], reordered_vertices.tcFloats[2]);
    assert(!RT64::tooieCpuPoseCorrespondence(current, reordered_vertices, 0, 0));
    auto mixed_root = prior;
    mixed_root.worldTransformGroups.push_back(0);
    mixed_root.worldTransformVertexIndices.push_back(3);
    mixed_root.worldIndices[2] = 1;
    assert(!RT64::tooieCpuPoseCorrespondence(current, mixed_root, 0, 0));
    auto bad_face = prior;
    bad_face.faceIndices = {0, 1, 3};
    assert(!RT64::tooieCpuPoseCorrespondence(current, bad_face, 0, 0));
    auto missing_face = prior;
    missing_face.faceIndices.clear();
    assert(!RT64::tooieCpuPoseCorrespondence(current, missing_face, 0, 0));

    auto nonfinite = prior;
    nonfinite.posFloats[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!RT64::tooieCpuPoseCorrespondence(current, nonfinite, 0, 0));
    nonfinite = prior;
    nonfinite.tcFloats[0] = std::numeric_limits<float>::infinity();
    assert(!RT64::tooieCpuPoseCorrespondence(current, nonfinite, 0, 0));
    auto jumped = prior;
    jumped.posFloats[0] = 267; // Current x=10; 257 local units away.
    assert(!RT64::tooieCpuPoseCorrespondence(current, jumped, 0, 0));
    jumped.posFloats[0] = 266; // The inclusive 256-unit boundary.
    assert(RT64::tooieCpuPoseCorrespondence(current, jumped, 0, 0));

    auto no_vertices = prior;
    no_vertices.worldIndices.clear();
    no_vertices.posFloats.clear();
    no_vertices.tcFloats.clear();
    no_vertices.faceIndices.clear();
    assert(!RT64::tooieCpuPoseCorrespondence(current, no_vertices, 0, 0));
    auto too_many = prior;
    too_many.worldIndices.resize(4097, 0);
    too_many.posFloats.resize(4097 * 3, 0);
    too_many.tcFloats.resize(4097 * 2, 0);
    assert(!RT64::tooieCpuPoseCorrespondence(current, too_many, 0, 0));
    auto too_many_faces = prior;
    too_many_faces.faceIndices.resize(262147, 0);
    assert(!RT64::tooieCpuPoseCorrespondence(current, too_many_faces, 0, 0));
    assert(!RT64::tooieCpuPoseCorrespondence(current, prior, 10, 0));

    // Clearing stale CPU pose velocity is independent of pair eligibility.
    auto velocity = prior;
    velocity.velFloats.assign(9, 7.0f);
    assert(RT64::tooieClearCpuPoseVelocity(velocity, 0));
    for (float component : velocity.velFloats) assert(component == 0.0f);
    assert(!RT64::tooieClearCpuPoseVelocity(velocity, 0));

    // Only current-root-owned vertices are touched, even in a mixed span.
    auto mixed_velocity = prior;
    mixed_velocity.worldTransformGroups.push_back(0);
    mixed_velocity.worldTransformVertexIndices.push_back(3);
    mixed_velocity.worldIndices = {0, 1, 0};
    mixed_velocity.velFloats.assign(9, 5.0f);
    assert(RT64::tooieClearCpuPoseVelocity(mixed_velocity, 0));
    for (unsigned component = 0; component < 3; ++component) {
        assert(mixed_velocity.velFloats[component] == 0.0f);
        assert(mixed_velocity.velFloats[3 + component] == 5.0f);
        assert(mixed_velocity.velFloats[6 + component] == 0.0f);
    }
    assert(!RT64::tooieClearCpuPoseVelocity(mixed_velocity, 0));
    assert(!RT64::tooieClearCpuPoseVelocity(mixed_velocity, 10));

    // A truncated velocity buffer only exposes complete triples; no OOB write.
    auto partial_velocity = prior;
    partial_velocity.velFloats.assign(7, 3.0f);
    assert(RT64::tooieClearCpuPoseVelocity(partial_velocity, 0));
    for (unsigned i = 0; i < 6; ++i) assert(partial_velocity.velFloats[i] == 0.0f);
    assert(partial_velocity.velFloats[6] == 3.0f);
}
