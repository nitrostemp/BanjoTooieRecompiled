#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <limits>

namespace tooie::minimap {

inline constexpr std::size_t kTrailCapacity = 64;
inline constexpr float kSampleDistance = 80.0f;
inline constexpr float kDiscontinuityDistance = 5000.0f;
inline constexpr float kRadarRadius = 2500.0f;

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct TrailSnapshot {
    std::array<Position, kTrailCapacity> points{};
    Position position{};
    std::size_t count = 0;
    std::uint16_t map_id = 0;
    float yaw_degrees = 0.0f;
    float camera_yaw_degrees = 0.0f;
    bool camera_heading_available = false;
    bool current = false;
    std::uint64_t generation = 0;
};

struct ProjectedPoint {
    // Normalized radar coordinates. (-1,-1) is top left and (1,1) is
    // bottom right. Points outside the local radius are marked invisible.
    float x = 0.0f;
    float y = 0.0f;
    bool visible = false;
};

class TrailState {
public:
    void observe(bool map_available, std::uint16_t map_id, bool activation_active,
        Position position, float yaw_degrees,
        float camera_yaw_degrees = std::numeric_limits<float>::quiet_NaN()) noexcept;
    TrailSnapshot snapshot() const noexcept;
    void reset() noexcept;

private:
    void reset_locked() noexcept;

    mutable std::mutex mutex_;
    TrailSnapshot state_{};
};

// Tooie advances a point at yaw Y by (+sin(Y), +cos(Y)) in X/Z. This helper
// uses that exact convention and rotates world offsets into a heading-up view.
ProjectedPoint project_heading_up(Position origin, Position point,
    float yaw_degrees) noexcept;

} // namespace tooie::minimap
