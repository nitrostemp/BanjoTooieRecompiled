#include "cutscene_tools.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace {
std::atomic<std::int64_t> skip_deadline_ns{0};
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

namespace tooie::cutscene_tools {
void request_skip() noexcept {
    skip_deadline_ns.store(now_ns() + 2'000'000'000LL, std::memory_order_release);
}
void reset() noexcept { skip_deadline_ns.store(0, std::memory_order_release); }
}

extern "C" int tooie_cutscene_skip_input(int original_pressed) noexcept {
    const auto deadline = skip_deadline_ns.exchange(0, std::memory_order_acq_rel);
    // The original caller still selects a supported map from its own table,
    // checks transition state, and runs its scene-specific exit function.
    // This does not bypass script cleanup or force a general map transition.
    return deadline != 0 && now_ns() <= deadline ? 1 : original_pressed;
}
