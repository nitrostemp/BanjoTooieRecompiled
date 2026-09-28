#include "replay_timing.hpp"

#include <cstdio>
#include <stdexcept>

using namespace tooie::replay_timing;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        // A normal frame has no replay sample and returns its own map-selected
        // scheduler divisor unchanged.
        record_divisor(0, false);
        require(frame_refresh_rate() == 0, "normal frame published replay rate");
        require(scheduler_divisor(2, true) == 2, "normal fallback changed");
        require(scheduler_divisor(3, false) == 3, "normal loop fallback changed");
        require(static_cast<int64_t>(static_cast<int32_t>(scheduler_divisor(0xffffffffu, true))) == -1,
            "normal signed guest return changed");

        record_divisor(1, true);
        require(frame_refresh_rate() == 60, "minimum replay rate lost");
        require(scheduler_divisor(2, true) == 1, "minimum replay divisor lost");
        require(scheduler_divisor(3, false) == 1, "minimum replay loop diverged");
        require(scheduler_divisor(3, true) == 3, "next ordinary frame retained replay divisor");

        record_divisor(15, true);
        require(frame_refresh_rate() == 4, "maximum replay rate lost");
        require(scheduler_divisor(2, true) == 15, "maximum replay divisor lost");
        require(scheduler_divisor(2, false) == 15, "maximum replay loop diverged");

        record_divisor(0, true);
        require(scheduler_divisor(3, true) == 3, "zero record divisor accepted");
        record_divisor(16, true);
        require(scheduler_divisor(3, true) == 3, "out-of-range record divisor accepted");
        record_divisor(4, false);
        require(scheduler_divisor(3, true) == 3, "EOF retained a replay divisor");
        require(frame_refresh_rate() == 0, "EOF retained a replay rate");

        record_divisor(3, true);
        require(frame_refresh_rate() == 20, "recorded divisor did not publish rate");
        begin_record();
        require(frame_refresh_rate() == 0, "next record entry retained stale replay rate");

        std::puts("PASS replay scheduler divisor: bounds, loop consistency, EOF, normal fallback");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
