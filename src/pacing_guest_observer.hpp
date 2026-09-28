#pragma once

#include <cstdint>

namespace tooie::pacing {
struct GuestUpdateSnapshot {
    std::uint64_t updates = 0;
    std::uint64_t wait_bypassed = 0;
};
void reset_guest_update_observer() noexcept;
GuestUpdateSnapshot guest_update_snapshot() noexcept;
}

extern "C" void tooie_guest_update_observed(int wait_bypassed) noexcept;
