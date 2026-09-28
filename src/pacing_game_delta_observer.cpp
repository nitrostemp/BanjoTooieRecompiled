#include "pacing_game_delta_observer.hpp"

#include <atomic>
#include <bit>

namespace {
std::atomic<std::uint64_t> samples{0};
std::atomic<std::uint32_t> latest_bits{0};
}

extern "C" void tooie_game_delta_observed(std::uint32_t bits) noexcept {
    latest_bits.store(bits, std::memory_order_relaxed);
    samples.fetch_add(1, std::memory_order_relaxed);
}

namespace tooie::pacing {

void reset_game_delta_observer() noexcept {
    latest_bits.store(0, std::memory_order_release);
    samples.store(0, std::memory_order_release);
}

GameDeltaSnapshot game_delta_snapshot() noexcept {
    return {samples.load(std::memory_order_acquire),
        std::bit_cast<float>(latest_bits.load(std::memory_order_acquire))};
}

} // namespace tooie::pacing
