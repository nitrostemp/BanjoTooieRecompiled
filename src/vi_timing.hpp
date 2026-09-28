#pragma once

#include <chrono>
#include <cstdint>
#include "ultramodern/ultramodern.hpp"
#include "virtual_clock.hpp"

namespace tooie::vi {
constexpr uint64_t lines_per_ntsc_frame=525;

// The boundary helper is pure so callers and tests share the exact wrapping
// and range contract. The current ROM is NTSC and the pinned VI loop uses
// 60 * get_speed_multiplier() virtual retraces per second.
constexpr uint32_t line_for_elapsed(uint64_t elapsed_ns,uint32_t speed_multiplier) noexcept {
    if(!speed_multiplier)return 0;
    constexpr uint64_t ns_per_second=1'000'000'000ull;
    const uint64_t frame_ns=ns_per_second/(60ull*speed_multiplier);
    if(!frame_ns)return 0;
    return static_cast<uint32_t>(((elapsed_ns%frame_ns)*lines_per_ntsc_frame)/frame_ns);
}

inline void reset_epoch() noexcept {
    timing::reset_virtual_clock();
}

// This is not hardware-register accuracy, but retains range, wrap and rate
// semantics without forging a fixed "ready" response. Virtual elapsed time
// shares VI/timer phase and remains continuous through a rate change.
inline uint32_t current_line() noexcept {
    using namespace std::chrono;
    const auto elapsed=timing::virtual_elapsed().count();
    return line_for_elapsed(elapsed>0?static_cast<uint64_t>(elapsed):0,1);
}

}
