#pragma once

#include <cstdint>

namespace tooie::practice {

struct Position { float x = 0, y = 0, z = 0; };

// All values are copied on the guest thread at bainput_update entry. Input is
// the latest state processed by the previous update, not a live SDL event.
struct Sample {
    bool valid = false;
    std::uint16_t map_id = 0;
    std::uint32_t player_address = 0; // controlled PlayerState identity
    Position position{};
    float facing_degrees = 0;
    bool camera_valid = false;
    float camera_yaw_degrees = 0;
    float camera_pitch_degrees = 0;
    bool stick_valid = false;
    float stick_x = 0;
    float stick_y = 0;
    bool buttons_valid = false;
    std::uint16_t held_buttons = 0; // ButtonId bits, not libultra button bits.
    bool vertical_velocity_valid = false;
    float vertical_velocity = 0;
};

struct Snapshot {
    Sample current{};
    bool displacement_valid = false;
    Position displacement_per_update{};
    float horizontal_distance_per_update = 0;
    bool moving_angle_valid = false;
    float moving_angle_degrees = 0;
    bool target_valid = false;
    Sample target{};
    bool target_same_map = false;
    Position target_delta{}; // current minus target
    float target_facing_delta_degrees = 0;
    std::uint64_t observed_updates = 0;
};

// A single owner serializes practice actions that request a guest transition.
// Acquisition is atomic across the frontend and guest threads.
enum class TransitionOwner : std::uint8_t { None, Warp, Form };
bool try_reserve_transition(TransitionOwner owner) noexcept;
void release_transition(TransitionOwner owner) noexcept;
TransitionOwner transition_owner() noexcept;

void observe(const Sample& sample) noexcept;
Snapshot snapshot() noexcept;
bool mark_alignment() noexcept; // false when no current player sample exists
void clear_alignment() noexcept;
void reset() noexcept;

} // namespace tooie::practice
