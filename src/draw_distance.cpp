#include "draw_distance.hpp"

#include <atomic>
#include <cmath>

namespace {
std::atomic<tooie::draw_distance::ActorDistance> configured{
    tooie::draw_distance::ActorDistance::Original};
std::atomic<tooie::draw_distance::ActorDistance> latched{
    tooie::draw_distance::ActorDistance::Original};

constexpr tooie::draw_distance::ActorDistance normalize(
    tooie::draw_distance::ActorDistance value) noexcept {
    return value == tooie::draw_distance::ActorDistance::Extended2x
        ? value : tooie::draw_distance::ActorDistance::Original;
}
}

namespace tooie::draw_distance {

void configure_actor_distance(ActorDistance value) noexcept {
    configured.store(normalize(value), std::memory_order_release);
}

void latch_for_game_start() noexcept {
    latched.store(configured.load(std::memory_order_acquire), std::memory_order_release);
}

ActorDistance configured_actor_distance() noexcept {
    return configured.load(std::memory_order_acquire);
}

ActorDistance latched_actor_distance() noexcept {
    return latched.load(std::memory_order_acquire);
}

float adjust_actor_distance(float original_distance) noexcept {
    if (latched_actor_distance() != ActorDistance::Extended2x ||
        !std::isfinite(original_distance) || original_distance <= 0.0f)
        return original_distance;
    return original_distance * 2.0f;
}

} // namespace tooie::draw_distance

extern "C" float tooie_actor_draw_distance_adjust(float original_distance) {
    return tooie::draw_distance::adjust_actor_distance(original_distance);
}
