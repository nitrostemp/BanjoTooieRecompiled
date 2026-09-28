#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace tooie::pacing {

// These durations cover elapsed host time inside the two RT64 calls only.
// They can include waits; they do not measure CPU utilization, queued GPU
// execution, or successful presentation.
enum class RenderWorkPhase : std::uint8_t {
    ProcessDisplayLists = 0,
    UpdateScreen = 1,
};

struct RenderWorkPhaseSnapshot {
    double last_ms = 0.0;
    double max_ms = 0.0;
    // Bounded recent values cover the latest calls (up to 120). They are
    // intended to correlate an issue marker with the current view; max_ms and
    // samples_over_50ms remain lifetime hitch evidence.
    double recent_average_ms = 0.0;
    double recent_max_ms = 0.0;
    std::uint64_t recent_samples = 0;
    std::uint64_t samples = 0;
    std::uint64_t samples_over_50ms = 0;
};

struct RenderWorkSnapshot {
    RenderWorkPhaseSnapshot process_display_lists;
    RenderWorkPhaseSnapshot update_screen;
    double display_list_cpu_last_ms = 0.0;
    double display_list_cpu_average_ms = 0.0;
};

void reset_render_work() noexcept;
void record_render_work(RenderWorkPhase phase, std::chrono::nanoseconds elapsed) noexcept;
// RT64's own display-list CPU profiler ends and logs before its workload queue
// advance, so this excludes the queue barrier wait included in host wall time.
void record_display_list_cpu(double last_ms, double average_ms) noexcept;
RenderWorkSnapshot render_work_snapshot() noexcept;

} // namespace tooie::pacing
