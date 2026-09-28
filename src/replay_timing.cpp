#include "replay_timing.hpp"

extern "C" void tooie_replay_timing_begin_record() noexcept {
    tooie::replay_timing::begin_record();
}

extern "C" void tooie_replay_timing_record_divisor(uint32_t divisor, int record_available) noexcept {
    tooie::replay_timing::record_divisor(divisor, record_available != 0);
}

extern "C" uint32_t tooie_replay_timing_scheduler_divisor(uint32_t original, int first_query) noexcept {
    return tooie::replay_timing::scheduler_divisor(original, first_query != 0);
}
