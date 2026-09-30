#pragma once

#include <cstdint>

namespace tooie::rt64_trace {

// Each nonzero, newer request gets its own bounded capture file. A new press
// ends the preceding window without reusing or truncating its file name.
struct Window {
    static constexpr uint32_t duration = 420;
    uint32_t seen = 0;
    uint32_t calls = duration;
    uint32_t generation = 0;

    uint32_t request(uint32_t generation) noexcept {
        if (generation == 0 || generation <= seen) return 0;
        seen = generation;
        this->generation = generation;
        calls = 0;
        return generation;
    }
};

} // namespace tooie::rt64_trace
