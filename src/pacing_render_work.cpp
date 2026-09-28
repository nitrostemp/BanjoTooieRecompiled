#include "pacing_render_work.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace {
constexpr std::size_t phase_count = 2;
constexpr std::size_t recent_capacity = 120;
constexpr std::uint64_t hitch_threshold_ns = 50'000'000;

struct PhaseCounters {
    std::atomic_uint64_t last_ns{0};
    std::atomic_uint64_t max_ns{0};
    std::atomic_uint64_t samples{0};
    std::atomic_uint64_t samples_over_50ms{0};
    std::array<std::atomic_uint64_t, recent_capacity> recent_ns{};
    std::atomic_uint64_t recent_sequence{0};
};

std::array<PhaseCounters, phase_count> counters{};
std::atomic_uint64_t display_list_cpu_last_ns{0};
std::atomic_uint64_t display_list_cpu_average_ns{0};

std::size_t index(tooie::pacing::RenderWorkPhase phase) noexcept {
    return static_cast<std::size_t>(phase);
}

void update_max(std::atomic_uint64_t& maximum, std::uint64_t value) noexcept {
    auto observed = maximum.load(std::memory_order_relaxed);
    while (observed < value && !maximum.compare_exchange_weak(observed, value,
        std::memory_order_release, std::memory_order_relaxed)) {}
}

tooie::pacing::RenderWorkPhaseSnapshot snapshot(const PhaseCounters& source) noexcept {
    const auto sequence = source.recent_sequence.load(std::memory_order_acquire);
    const auto recent_samples = std::min<std::uint64_t>(sequence, recent_capacity);
    std::uint64_t recent_total_ns = 0;
    std::uint64_t recent_max_ns = 0;
    for (std::uint64_t sample = 0; sample < recent_samples; ++sample) {
        const auto value = source.recent_ns[sample].load(std::memory_order_relaxed);
        recent_total_ns += value;
        recent_max_ns = std::max(recent_max_ns, value);
    }
    return {
        .last_ms = static_cast<double>(source.last_ns.load(std::memory_order_acquire)) / 1'000'000.0,
        .max_ms = static_cast<double>(source.max_ns.load(std::memory_order_acquire)) / 1'000'000.0,
        .recent_average_ms = recent_samples == 0 ? 0.0 :
            static_cast<double>(recent_total_ns) / static_cast<double>(recent_samples) / 1'000'000.0,
        .recent_max_ms = static_cast<double>(recent_max_ns) / 1'000'000.0,
        .recent_samples = recent_samples,
        .samples = source.samples.load(std::memory_order_acquire),
        .samples_over_50ms = source.samples_over_50ms.load(std::memory_order_acquire),
    };
}
} // namespace

namespace tooie::pacing {

void reset_render_work() noexcept {
    for (auto& phase : counters) {
        phase.last_ns.store(0, std::memory_order_release);
        phase.max_ns.store(0, std::memory_order_release);
        phase.samples.store(0, std::memory_order_release);
        phase.samples_over_50ms.store(0, std::memory_order_release);
        for (auto& sample : phase.recent_ns) sample.store(0, std::memory_order_relaxed);
        phase.recent_sequence.store(0, std::memory_order_release);
    }
    display_list_cpu_last_ns.store(0, std::memory_order_release);
    display_list_cpu_average_ns.store(0, std::memory_order_release);
}

void record_render_work(RenderWorkPhase phase, std::chrono::nanoseconds elapsed) noexcept {
    const auto slot = index(phase);
    if (slot >= counters.size() || elapsed <= std::chrono::nanoseconds::zero()) return;
    const auto value = static_cast<std::uint64_t>(elapsed.count());
    auto& target = counters[slot];
    target.last_ns.store(value, std::memory_order_release);
    update_max(target.max_ns, value);
    target.samples.fetch_add(1, std::memory_order_relaxed);
    if (value >= hitch_threshold_ns) target.samples_over_50ms.fetch_add(1, std::memory_order_relaxed);
    // Each phase has one producer on the RT64 host thread. Publish the slot
    // after its value so diagnostic readers never count an unwritten sample.
    const auto sequence = target.recent_sequence.load(std::memory_order_relaxed);
    target.recent_ns[sequence % recent_capacity].store(value, std::memory_order_relaxed);
    target.recent_sequence.store(sequence + 1, std::memory_order_release);
}

void record_display_list_cpu(double last_ms, double average_ms) noexcept {
    if (!std::isfinite(last_ms) || !std::isfinite(average_ms) || last_ms < 0.0 || average_ms < 0.0) return;
    display_list_cpu_last_ns.store(static_cast<std::uint64_t>(std::llround(last_ms * 1'000'000.0)),
        std::memory_order_release);
    display_list_cpu_average_ns.store(static_cast<std::uint64_t>(std::llround(average_ms * 1'000'000.0)),
        std::memory_order_release);
}

RenderWorkSnapshot render_work_snapshot() noexcept {
    return {snapshot(counters[index(RenderWorkPhase::ProcessDisplayLists)]),
        snapshot(counters[index(RenderWorkPhase::UpdateScreen)]),
        static_cast<double>(display_list_cpu_last_ns.load(std::memory_order_acquire)) / 1'000'000.0,
        static_cast<double>(display_list_cpu_average_ns.load(std::memory_order_acquire)) / 1'000'000.0};
}

} // namespace tooie::pacing
