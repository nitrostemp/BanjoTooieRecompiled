#pragma once

#include <cmath>
#include <cstdint>

namespace tooie::rt64_match {

inline bool guard_auto_skip_mesh(bool auto_id, bool vertex_skip,
    uint32_t current_vertices, uint32_t previous_vertices) noexcept {
    return auto_id && vertex_skip && current_vertices != 0 &&
        current_vertices == previous_vertices;
}

inline bool allow_world_pair(bool auto_skip_mesh,
    float current_x, float current_y, float current_z,
    float previous_x, float previous_y, float previous_z) noexcept {
    if (!auto_skip_mesh) return true;
    if (!std::isfinite(current_x) || !std::isfinite(current_y) ||
        !std::isfinite(current_z) || !std::isfinite(previous_x) ||
        !std::isfinite(previous_y) || !std::isfinite(previous_z)) return false;
    const double dx = double(current_x) - double(previous_x);
    const double dy = double(current_y) - double(previous_y);
    const double dz = double(current_z) - double(previous_z);
    // Geometry identifies a mesh, not its instance. Reject extreme travel
    // for AUTO/SKIP pairs, including particle quads and repeated actor meshes.
    constexpr double max_guest_frame_travel = 256.0;
    return dx * dx + dy * dy + dz * dz <=
        max_guest_frame_travel * max_guest_frame_travel;
}

} // namespace tooie::rt64_match
