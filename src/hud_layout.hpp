#pragma once

#include <cstdint>

namespace tooie::hud_layout {

enum class CounterLayout : std::uint32_t {
    Centered = 0,
    Expanded = 1,
};

enum class Proportions : std::uint32_t {
    Original = 0,
    Stretch = 1,
};

void configure_counter_layout(CounterLayout layout) noexcept;
void configure_proportions(Proportions proportions) noexcept;
void latch_for_game_start() noexcept;
CounterLayout configured_counter_layout() noexcept;
CounterLayout latched_counter_layout() noexcept;
Proportions configured_proportions() noexcept;
Proportions latched_proportions() noexcept;

// Scales only the horizontal origin of source-identified ordinary scinfobar
// counters: health, Notes, eggs, Jiggies, feathers, Empty Honeycombs, and the
// nine Jinjo families. Custom/minigame counter types are unchanged.
int adjust_counter_x(std::uint16_t counter_id, int original_x) noexcept;

} // namespace tooie::hud_layout
