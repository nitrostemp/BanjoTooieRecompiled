#pragma once

#include <cstdint>

namespace tooie::pacing {

// A narrow read-only sample of the guest's already-computed D_8012C760 game
// delta. It measures the time value actually handed to game systems; it does
// not establish the host clock or alter scheduler/VI behavior.
struct GameDeltaSnapshot {
    std::uint64_t samples = 0;
    float latest_seconds = 0.0f;
};

void reset_game_delta_observer() noexcept;
GameDeltaSnapshot game_delta_snapshot() noexcept;

} // namespace tooie::pacing

// `bits` is the guest float's raw IEEE-754 representation. Keeping the guest
// boundary integer-only avoids a host ABI float dependency in generated code.
extern "C" void tooie_game_delta_observed(std::uint32_t bits) noexcept;
