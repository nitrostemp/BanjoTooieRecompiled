#include "practice_state.hpp"
#include "scene_observer.hpp"

#include <chrono>
#include <cmath>
#include <atomic>
#include <mutex>

namespace {
std::mutex state_mutex;
std::atomic<tooie::practice::TransitionOwner> transition_reservation{
    tooie::practice::TransitionOwner::None};
tooie::practice::Snapshot state;
std::chrono::steady_clock::time_point last_observed{};
constexpr float discontinuity_distance = 5000.0f;
constexpr auto stale_after = std::chrono::seconds(2);

bool finite(const tooie::practice::Sample& sample) noexcept {
    return std::isfinite(sample.position.x) && std::isfinite(sample.position.y) &&
        std::isfinite(sample.position.z) && std::isfinite(sample.facing_degrees);
}
}

namespace tooie::practice {

bool try_reserve_transition(TransitionOwner owner) noexcept {
    if (owner == TransitionOwner::None) return false;
    auto expected = TransitionOwner::None;
    return transition_reservation.compare_exchange_strong(expected, owner,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

void release_transition(TransitionOwner owner) noexcept {
    if (owner == TransitionOwner::None) return;
    auto expected = owner;
    transition_reservation.compare_exchange_strong(expected, TransitionOwner::None,
        std::memory_order_acq_rel, std::memory_order_acquire);
}

TransitionOwner transition_owner() noexcept {
    return transition_reservation.load(std::memory_order_acquire);
}

void observe(const Sample& incoming) noexcept {
    std::lock_guard lock(state_mutex);
    last_observed = std::chrono::steady_clock::now();
    ++state.observed_updates;
    const auto previous = state.current;
    state.displacement_valid = false;
    state.moving_angle_valid = false;
    state.displacement_per_update = {};
    state.horizontal_distance_per_update = 0;
    state.current = incoming;
    state.current.valid = incoming.valid && finite(incoming);
    if (!state.current.valid || !previous.valid || previous.map_id != incoming.map_id ||
        previous.player_address != incoming.player_address)
        return;
    const Position delta{incoming.position.x - previous.position.x,
        incoming.position.y - previous.position.y,
        incoming.position.z - previous.position.z};
    const float horizontal = std::hypot(delta.x, delta.z);
    if (!std::isfinite(horizontal) || !std::isfinite(delta.y) ||
        std::hypot(horizontal, delta.y) > discontinuity_distance) return;
    state.displacement_valid = true;
    state.displacement_per_update = delta;
    state.horizontal_distance_per_update = horizontal;
    if (horizontal > 0.0001f) {
        state.moving_angle_valid = true;
        state.moving_angle_degrees = std::atan2(delta.x, delta.z) * (180.0f / 3.14159265358979323846f);
    }
}

Snapshot snapshot() noexcept {
    Snapshot copy;
    std::chrono::steady_clock::time_point sampled_at;
    {
        std::lock_guard lock(state_mutex);
        copy = state;
        sampled_at = last_observed;
    }
    const auto scene = scene::snapshot();
    if (sampled_at == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() - sampled_at > stale_after ||
        !scene.map_available || scene.activation_active ||
        copy.current.map_id != scene.map_id) {
        copy.current.valid = false;
        copy.displacement_valid = false;
        copy.moving_angle_valid = false;
    }
    copy.target_same_map = copy.current.valid && copy.target_valid &&
        copy.current.map_id == copy.target.map_id;
    if (copy.target_same_map) {
        copy.target_delta = {copy.current.position.x - copy.target.position.x,
            copy.current.position.y - copy.target.position.y,
            copy.current.position.z - copy.target.position.z};
        copy.target_facing_delta_degrees = std::remainder(
            copy.current.facing_degrees - copy.target.facing_degrees, 360.0f);
    }
    return copy;
}

bool mark_alignment() noexcept {
    const auto scene = scene::snapshot();
    std::lock_guard lock(state_mutex);
    if (!state.current.valid || !scene.map_available || scene.activation_active ||
        scene.map_id != state.current.map_id ||
        last_observed == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() - last_observed > stale_after) return false;
    state.target = state.current;
    state.target_valid = true;
    return true;
}

void clear_alignment() noexcept {
    std::lock_guard lock(state_mutex);
    state.target_valid = false;
    state.target = {};
}

void reset() noexcept {
    std::lock_guard lock(state_mutex);
    state = {};
    last_observed = {};
    transition_reservation.store(TransitionOwner::None, std::memory_order_release);
}

} // namespace tooie::practice
