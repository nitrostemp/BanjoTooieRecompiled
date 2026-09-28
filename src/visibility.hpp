#pragma once

#include <cstdint>

namespace tooie::visibility {

struct PerspectiveAdjustmentReadout {
    // Reset on each real Start Game latch. Counts renderer tasks where RT64
    // preserved Tooie's native projection instead of widening it again.
    std::uint64_t applied_tasks = 0;
    bool native_projection_latched = false;
    // Compatibility alias for the frontend while the retired diagnostic UI is
    // removed. It has the same value as native_projection_latched.
    bool diagnostic_latched = false;
};

// Retained as a compatibility sink for existing profiles. Tooie's game
// widescreen mode is now the sole source of projection ownership.
void configure_profile(bool rt64_perspective_adjustment) noexcept;
void latch_for_game_start() noexcept;

// Tooie widens both the perspective projection and its CPU frustum. When game
// widescreen is latched, the renderer must preserve that projection rather
// than applying a second perspective expansion.
bool task_projection_override_active() noexcept;
void record_task_applied() noexcept;
PerspectiveAdjustmentReadout perspective_adjustment_readout() noexcept;

} // namespace tooie::visibility
