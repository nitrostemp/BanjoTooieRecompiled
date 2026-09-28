#include "virtual_clock.hpp"

#include <atomic>
#include <mutex>
#include <limits>

namespace {
using steady_clock = std::chrono::steady_clock;

struct ClockState {
    std::mutex mutex;
    steady_clock::time_point host_anchor{steady_clock::now()};
    std::chrono::nanoseconds virtual_anchor{};
    uint32_t rate{1};
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    uint64_t persistent_epoch=0;
#endif
};

ClockState clock_state;
std::atomic_uint configured_fast_forward_rate{2};
std::atomic_bool fast_forward_is_held{false};
using RateChangeNotifier = void (*)() noexcept;
std::atomic<RateChangeNotifier> rate_change_notifier{nullptr};

uint32_t clamp_rate(uint32_t value) noexcept {
    switch (value) {
    case 1:
    case 2:
    case 4:
    case 6:
    case 8:
        return value;
    default:
        return 1;
    }
}

void accrue_locked(steady_clock::time_point now) noexcept {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if(clock_state.persistent_epoch) {clock_state.host_anchor=now;return;}
#endif
    const auto host_elapsed = now - clock_state.host_anchor;
    if (host_elapsed > steady_clock::duration::zero()) {
        const auto host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(host_elapsed).count();
        clock_state.virtual_anchor += std::chrono::nanoseconds{host_ns * static_cast<int64_t>(clock_state.rate)};
    }
    clock_state.host_anchor = now;
}
} // namespace

namespace tooie::timing {

void reset_virtual_clock() noexcept {
    std::lock_guard lock(clock_state.mutex);
    clock_state.host_anchor = steady_clock::now();
    clock_state.virtual_anchor = {};
    clock_state.rate = 1;
    fast_forward_is_held.store(false, std::memory_order_release);
}

std::chrono::nanoseconds virtual_elapsed() noexcept {
    std::lock_guard lock(clock_state.mutex);
    accrue_locked(steady_clock::now());
    return clock_state.virtual_anchor;
}

std::chrono::steady_clock::time_point host_deadline_for_virtual(std::chrono::nanoseconds target) noexcept {
    std::lock_guard lock(clock_state.mutex);
    const auto now = steady_clock::now();
    accrue_locked(now);
    if (target <= clock_state.virtual_anchor) return now;
    const auto remaining = target - clock_state.virtual_anchor;
    return now + remaining / static_cast<int64_t>(clock_state.rate);
}

uint32_t rate() noexcept {
    std::lock_guard lock(clock_state.mutex);
    return clock_state.rate;
}

bool set_rate(uint32_t value) noexcept {
    value = clamp_rate(value);
    std::lock_guard lock(clock_state.mutex);
    if (clock_state.rate == value) return false;
    accrue_locked(steady_clock::now());
    clock_state.rate = value;
    return true;
}

void set_rate_change_notifier(RateChangeNotifier notifier) noexcept {
    rate_change_notifier.store(notifier, std::memory_order_release);
}

void set_fast_forward_rate(uint32_t value) noexcept {
    configured_fast_forward_rate.store(clamp_rate(value), std::memory_order_release);
    if (fast_forward_is_held.load(std::memory_order_acquire) && set_rate(clamp_rate(value))) {
        if (const auto notifier = rate_change_notifier.load(std::memory_order_acquire)) notifier();
    }
}

uint32_t fast_forward_rate() noexcept {
    return configured_fast_forward_rate.load(std::memory_order_acquire);
}

void set_fast_forward_held(bool held) noexcept {
    const bool changed = fast_forward_is_held.exchange(held, std::memory_order_acq_rel) != held;
    if (changed && set_rate(held ? fast_forward_rate() : 1U)) {
        if (const auto notifier = rate_change_notifier.load(std::memory_order_acquire)) notifier();
    }
}

bool fast_forward_held() noexcept {
    return fast_forward_is_held.load(std::memory_order_acquire);
}

bool fast_forward_active() noexcept {
    return rate() > 1U;
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_quiesce(uint64_t epoch) noexcept {
    std::lock_guard lock(clock_state.mutex);
    if(!epoch||clock_state.persistent_epoch||clock_state.rate!=1||fast_forward_is_held.load())return false;
    accrue_locked(steady_clock::now());clock_state.persistent_epoch=epoch;return true;
}
void persistent_resume(uint64_t epoch) noexcept {
    std::lock_guard lock(clock_state.mutex);
    if(clock_state.persistent_epoch!=epoch)return;
    clock_state.host_anchor=steady_clock::now();clock_state.persistent_epoch=0;
}
bool persistent_restore(uint64_t ticks,uint64_t epoch) noexcept {
    // The pinned runtime quantizes to microseconds then floor(us*46875/1000).
    // Invert only that exact representable lattice; avoid clock/timer drift.
    const auto seconds=ticks/46875000;
    const auto remainder=ticks%46875000;
    if(seconds>static_cast<uint64_t>(std::numeric_limits<int64_t>::max()/1000000000))return false;
    const auto micros=seconds*1000000+(remainder*1000+46874)/46875;
    if((micros/1000)*46875+(micros%1000)*46875/1000!=ticks||micros>static_cast<uint64_t>(std::numeric_limits<int64_t>::max()/1000))return false;
    std::lock_guard lock(clock_state.mutex);
    if(!epoch||clock_state.persistent_epoch!=epoch)return false;
    clock_state.virtual_anchor=std::chrono::nanoseconds{static_cast<int64_t>(micros*1000)};
    clock_state.host_anchor=steady_clock::now();clock_state.rate=1;return true;
}
#endif

} // namespace tooie::timing
