#include "pacing_guest_observer.hpp"

#include <atomic>

namespace {
std::atomic<std::uint64_t> updates{0};
std::atomic<std::uint64_t> wait_bypassed{0};
}

extern "C" void tooie_guest_update_observed(int bypassed) noexcept {
    updates.fetch_add(1, std::memory_order_relaxed);
    if (bypassed) wait_bypassed.fetch_add(1, std::memory_order_relaxed);
}

namespace tooie::pacing {
void reset_guest_update_observer() noexcept {
    updates.store(0, std::memory_order_release);
    wait_bypassed.store(0, std::memory_order_release);
}
GuestUpdateSnapshot guest_update_snapshot() noexcept {
    return {updates.load(std::memory_order_acquire), wait_bypassed.load(std::memory_order_acquire)};
}
}
