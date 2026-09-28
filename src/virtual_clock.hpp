#pragma once

#include <chrono>
#include <cstdint>

namespace tooie::timing {

// Guest time advances at the selected rate while host deadlines remain on a
// monotonic clock. Rate changes first accrue the old-rate interval, so guest
// counters and VI sequencing never jump.
void reset_virtual_clock() noexcept;
std::chrono::nanoseconds virtual_elapsed() noexcept;
std::chrono::steady_clock::time_point host_deadline_for_virtual(std::chrono::nanoseconds target) noexcept;
uint32_t rate() noexcept;
bool set_rate(uint32_t value) noexcept;
void set_rate_change_notifier(void (*notifier)() noexcept) noexcept;

// UI/input-facing controls. The selected rate is used only while the hold is
// active; releasing it always returns to normal guest time.
void set_fast_forward_rate(uint32_t value) noexcept;
uint32_t fast_forward_rate() noexcept;
void set_fast_forward_held(bool held) noexcept;
bool fast_forward_held() noexcept;
bool fast_forward_active() noexcept;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_quiesce(uint64_t epoch) noexcept;
void persistent_resume(uint64_t epoch) noexcept;
bool persistent_restore(uint64_t virtual_ticks,uint64_t epoch) noexcept;
#endif

} // namespace tooie::timing
