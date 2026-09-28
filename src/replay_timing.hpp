#pragma once

#include <atomic>
#include <cstdint>

// Replay records carry the number of NTSC retraces that elapsed before their
// input sample.  Consume that divisor only in core1's two wait-loop queries.
// The first query consumes the pending sample, so an ordinary or EOF frame
// immediately returns to the guest's original scheduler divisor.
namespace tooie::replay_timing {

inline std::atomic_uint32_t pending_divisor{0};
// Published at the replay-record boundary, before that frame can submit its
// graphics OSTask. It is cleared at every entry so a non-replay branch cannot
// label a later workload with an old replay rate.
inline std::atomic_uint32_t published_refresh_rate{0};
inline thread_local uint32_t active_divisor = 0;

constexpr bool valid_divisor(uint32_t divisor) noexcept {
    return divisor >= 1 && divisor <= 15;
}

inline void begin_record() noexcept {
    pending_divisor.store(0, std::memory_order_release);
    published_refresh_rate.store(0, std::memory_order_release);
}

inline void record_divisor(uint32_t divisor, bool record_available) noexcept {
    const uint32_t accepted = record_available && valid_divisor(divisor) ? divisor : 0;
    pending_divisor.store(accepted, std::memory_order_release);
    // This deliberately matches Banjo-Kazooie's gEXSetRefreshRate(60 / divisor)
    // integer division. Divisors that do not divide 60 are truncated by the
    // original-compatible integer expression.
    published_refresh_rate.store(accepted == 0 ? 0 : 60U / accepted,
        std::memory_order_release);
}

inline uint16_t frame_refresh_rate() noexcept {
    return static_cast<uint16_t>(published_refresh_rate.load(std::memory_order_acquire));
}

inline uint32_t scheduler_divisor(uint32_t original, bool first_query) noexcept {
    if (first_query) {
        active_divisor = pending_divisor.exchange(0, std::memory_order_acq_rel);
    }
    return active_divisor != 0 ? active_divisor : original;
}

} // namespace tooie::replay_timing
