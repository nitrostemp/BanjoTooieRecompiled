#pragma once

#include <cmath>
#include <cstdint>

namespace tooie::rt64_match {

inline bool guard_auto_skip_quad(bool auto_id, bool vertex_skip,
    uint32_t current_vertices, uint32_t previous_vertices) noexcept {
    return auto_id && vertex_skip && current_vertices == 4 && previous_vertices == 4;
}

inline bool allow_world_pair(bool auto_skip_quad,
    float current_x, float current_y, float current_z,
    float previous_x, float previous_y, float previous_z) noexcept {
    if (!auto_skip_quad) return true;
    if (!std::isfinite(current_x) || !std::isfinite(current_y) ||
        !std::isfinite(current_z) || !std::isfinite(previous_x) ||
        !std::isfinite(previous_y) || !std::isfinite(previous_z)) return false;
    const double dx = double(current_x) - double(previous_x);
    const double dy = double(current_y) - double(previous_y);
    const double dz = double(current_z) - double(previous_z);
    // A four-vertex AUTO/SKIP draw has no reliable instance identity. Reject
    // an extreme displacement as a heuristic discontinuity safeguard.
    constexpr double max_guest_frame_travel = 256.0;
    return dx * dx + dy * dy + dz * dz <=
        max_guest_frame_travel * max_guest_frame_travel;
}

} // namespace tooie::rt64_match
