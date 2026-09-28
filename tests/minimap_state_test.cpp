#include "minimap_state.hpp"

#include <cassert>
#include <cmath>
#include <limits>

namespace {
constexpr float epsilon = 0.0001f;

bool near(float left, float right) {
    return std::fabs(left - right) < epsilon;
}
}

int main() {
    using namespace tooie::minimap;

    TrailState trail;
    trail.observe(true, 0x14AU, false, {100.0f, 20.0f, 200.0f}, 0.0f);
    auto state = trail.snapshot();
    assert(state.current);
    assert(state.map_id == 0x14AU);
    assert(state.count == 1);

    // Current position and heading update every observation, while breadcrumb
    // storage is distance bounded.
    trail.observe(true, 0x14AU, false, {120.0f, 900.0f, 210.0f}, 15.0f);
    state = trail.snapshot();
    assert(state.count == 1);
    assert(near(state.position.x, 120.0f));
    assert(near(state.position.z, 210.0f));
    assert(near(state.yaw_degrees, 15.0f));

    trail.observe(true, 0x14AU, false, {180.0f, 900.0f, 200.0f}, 15.0f);
    state = trail.snapshot();
    assert(state.count == 2);

    // A map transition starts a new local trail.
    trail.observe(true, 0x14BU, false, {180.0f, 900.0f, 200.0f}, 30.0f);
    state = trail.snapshot();
    assert(state.map_id == 0x14BU);
    assert(state.count == 1);

    // Loading and teleport-sized discontinuities never draw a false line.
    trail.observe(true, 0x14BU, true, {200.0f, 900.0f, 200.0f}, 30.0f);
    assert(!trail.snapshot().current);
    trail.observe(true, 0x14BU, false, {0.0f, 0.0f, 0.0f}, 0.0f);
    trail.observe(true, 0x14BU, false, {6000.0f, 0.0f, 0.0f}, 0.0f);
    state = trail.snapshot();
    assert(state.current);
    assert(state.count == 1);
    assert(near(state.points[0].x, 6000.0f));

    trail.observe(true, 0x14BU, false,
        {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 0.0f);
    assert(!trail.snapshot().current);

    // Storage remains fixed at 64 samples and retains the newest path.
    trail.reset();
    for (std::size_t index = 0; index < kTrailCapacity + 10; ++index) {
        trail.observe(true, 1U, false,
            {static_cast<float>(index) * kSampleDistance, 0.0f, 0.0f}, 0.0f);
    }
    state = trail.snapshot();
    assert(state.count == kTrailCapacity);
    assert(near(state.points[0].x, 10.0f * kSampleDistance));
    assert(near(state.points[kTrailCapacity - 1].x,
        static_cast<float>(kTrailCapacity + 9) * kSampleDistance));

    // Tooie's forward vector is (+sin(yaw), +cos(yaw)) in X/Z. Heading-up
    // projection therefore maps +Z ahead at 0 degrees and +X ahead at 90.
    auto projected = project_heading_up(
        {100.0f, 0.0f, 200.0f}, {100.0f, 0.0f, 1200.0f}, 0.0f);
    assert(projected.visible);
    assert(near(projected.x, 0.0f));
    assert(near(projected.y, -0.4f));

    projected = project_heading_up(
        {100.0f, 0.0f, 200.0f}, {1100.0f, 0.0f, 200.0f}, 90.0f);
    assert(projected.visible);
    assert(near(projected.x, 0.0f));
    assert(near(projected.y, -0.4f));

    projected = project_heading_up(
        {100.0f, 0.0f, 200.0f}, {1100.0f, 0.0f, 200.0f}, 0.0f);
    assert(projected.visible);
    assert(near(projected.x, 0.4f));
    assert(near(projected.y, 0.0f));

    projected = project_heading_up(
        {0.0f, 0.0f, 0.0f}, {kRadarRadius + 1.0f, 0.0f, 0.0f}, 0.0f);
    assert(!projected.visible);

    projected = project_heading_up({0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f}, std::numeric_limits<float>::max());
    assert(projected.visible);
    assert(std::isfinite(projected.x));
    assert(std::isfinite(projected.y));
}
