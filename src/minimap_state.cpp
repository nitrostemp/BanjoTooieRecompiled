#include "minimap_state.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr float pi = 3.14159265358979323846f;

float distance_xz(tooie::minimap::Position left,
    tooie::minimap::Position right) noexcept {
    return std::hypot(right.x - left.x, right.z - left.z);
}

bool finite(tooie::minimap::Position position, float yaw) noexcept {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) && std::isfinite(yaw);
}
}

namespace tooie::minimap {

void TrailState::reset_locked() noexcept {
    const auto next_generation = state_.generation + 1;
    state_ = {};
    state_.generation = next_generation;
}

void TrailState::reset() noexcept {
    std::lock_guard lock(mutex_);
    reset_locked();
}

void TrailState::observe(bool map_available, std::uint16_t map_id,
    bool activation_active, Position position, float yaw_degrees,
    float camera_yaw_degrees) noexcept {
    std::lock_guard lock(mutex_);
    if (!map_available || activation_active || !finite(position, yaw_degrees)) {
        if (state_.current || state_.count != 0) reset_locked();
        return;
    }

    const bool changed_map = state_.current && state_.map_id != map_id;
    const bool discontinuity = state_.current && !changed_map &&
        distance_xz(state_.position, position) > kDiscontinuityDistance;
    if (changed_map || discontinuity) reset_locked();

    state_.current = true;
    state_.map_id = map_id;
    state_.position = position;
    state_.yaw_degrees = yaw_degrees;
    state_.camera_heading_available = std::isfinite(camera_yaw_degrees);
    state_.camera_yaw_degrees = state_.camera_heading_available ? camera_yaw_degrees : yaw_degrees;

    const bool should_sample = state_.count == 0 ||
        distance_xz(state_.points[state_.count - 1], position) >= kSampleDistance;
    if (!should_sample) return;

    if (state_.count == kTrailCapacity) {
        std::move(state_.points.begin() + 1, state_.points.end(),
            state_.points.begin());
        --state_.count;
    }
    state_.points[state_.count++] = position;
    ++state_.generation;
}

TrailSnapshot TrailState::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
}

ProjectedPoint project_heading_up(Position origin, Position point,
    float yaw_degrees) noexcept {
    if (!finite(origin, yaw_degrees) || !finite(point, yaw_degrees)) return {};
    const float dx = point.x - origin.x;
    const float dz = point.z - origin.z;
    if (std::hypot(dx, dz) > kRadarRadius) return {};

    // Reduce every finite input before converting it to radians. This keeps
    // extreme but representable guest values from overflowing the multiply
    // and reaching the renderer as NaN coordinates.
    const float normalized_yaw = std::remainder(yaw_degrees, 360.0f);
    const float radians = normalized_yaw * pi / 180.0f;
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float right = dx * cosine - dz * sine;
    const float forward = dx * sine + dz * cosine;
    return {right / kRadarRadius, -forward / kRadarRadius, true};
}

} // namespace tooie::minimap
