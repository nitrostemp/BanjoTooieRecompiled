#include "visibility.hpp"

#include "widescreen.hpp"

#include <atomic>

namespace {
std::atomic_uint64_t applied_tasks{0};

} // namespace

namespace tooie::visibility {

void configure_profile(bool) noexcept {
    // Compatibility sink for profiles created while perspective ownership was
    // exposed as a diagnostic toggle. Native widescreen now determines it.
}

void latch_for_game_start() noexcept {
    applied_tasks.store(0, std::memory_order_release);
}

bool task_projection_override_active() noexcept {
    return widescreen::latched_enabled();
}

void record_task_applied() noexcept {
    applied_tasks.fetch_add(1, std::memory_order_relaxed);
}

PerspectiveAdjustmentReadout perspective_adjustment_readout() noexcept {
    const bool native_projection = task_projection_override_active();
    return {
        applied_tasks.load(std::memory_order_acquire),
        native_projection,
        native_projection,
    };
}

} // namespace tooie::visibility
