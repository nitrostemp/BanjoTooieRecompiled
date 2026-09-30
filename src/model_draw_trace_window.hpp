#pragma once

#include <cstdint>

namespace tooie::model_interpolation {

struct DrawTraceWindow {
    static constexpr std::uint32_t max_draws = 2000;
    static constexpr std::uint32_t max_tasks = 90;
    std::uint32_t seen = 0;
    std::uint32_t draws = 0;
    std::uint32_t tasks = 0;
    bool active = false;

    std::uint32_t request(std::uint32_t generation) noexcept {
        if (generation == 0 || generation <= seen) return 0;
        seen = generation;
        active = true;
        draws = 0;
        tasks = 0;
        return generation;
    }
    bool record_draw() noexcept {
        if (!active || draws == max_draws) return false;
        ++draws;
        return true;
    }
    bool record_task() noexcept {
        if (!active) return false;
        ++tasks;
        if (tasks < max_tasks && draws < max_draws) return false;
        active = false;
        return true;
    }
    void stop() noexcept { active = false; }
    void cancel(std::uint32_t generation) noexcept {
        stop();
        if (generation > seen) seen = generation;
        draws = 0;
        tasks = 0;
    }
};

} // namespace tooie::model_interpolation
