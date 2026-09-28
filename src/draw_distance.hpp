#pragma once

#include <cstdint>

namespace tooie::draw_distance {

enum class ActorDistance : std::uint32_t {
    Original = 0,
    Extended2x = 1,
};

// Restart-latched with other game graphics features. This affects only the
// common actor model fade/cull distance selected by func_80101970.
void configure_actor_distance(ActorDistance value) noexcept;
void latch_for_game_start() noexcept;
ActorDistance configured_actor_distance() noexcept;
ActorDistance latched_actor_distance() noexcept;
float adjust_actor_distance(float original_distance) noexcept;

} // namespace tooie::draw_distance

extern "C" float tooie_actor_draw_distance_adjust(float original_distance);
