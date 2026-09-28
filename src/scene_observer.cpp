#include "scene_observer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <limits>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto phase_count = static_cast<std::size_t>(tooie::scene::ActivationPhase::Count);
constexpr std::size_t nested_parent_count = 2;
constexpr std::uint32_t nested_parent_a5c28 = 0x800A5C28U;
constexpr std::uint32_t nested_parent_f73c4 = 0x800F73C4U;

std::atomic_uint map_id{0};
std::atomic_bool map_observed{false};
std::atomic_uint64_t activation_started_ns{0};
std::atomic_uint64_t activations_completed{0};
std::atomic_uint64_t last_activation_ns{0};
std::atomic_uint64_t max_activation_ns{0};
std::array<std::atomic_uint64_t, phase_count> phase_started_ns{};
std::array<std::atomic_uint64_t, phase_count> phase_last_ns{};
std::atomic_uint64_t world_call_started_ns{0};
std::atomic_uint world_call_started_pc{0};
// High 32 bits: elapsed microseconds. Low 32 bits: original jal PC.
std::atomic_uint64_t world_slowest_call{};
std::array<std::atomic_uint64_t, nested_parent_count> nested_call_started_ns{};
std::array<std::atomic_uint, nested_parent_count> nested_call_started_pc{};
std::array<std::atomic_uint, nested_parent_count> nested_observed_calls{};
// High 32 bits: elapsed microseconds. Low 32 bits: original child jal PC.
std::array<std::atomic_uint64_t, nested_parent_count> nested_slowest_call{};

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}

bool valid_phase(tooie::scene::ActivationPhase phase) noexcept {
    return static_cast<std::size_t>(phase) < phase_count;
}

int nested_parent_index(std::uint32_t parent_pc) noexcept {
    if (parent_pc == nested_parent_a5c28) return 0;
    if (parent_pc == nested_parent_f73c4) return 1;
    return -1;
}

void update_max(std::uint64_t value) noexcept {
    auto observed = max_activation_ns.load(std::memory_order_relaxed);
    while (observed < value && !max_activation_ns.compare_exchange_weak(observed, value,
        std::memory_order_release, std::memory_order_relaxed)) {}
}

double milliseconds(std::uint64_t nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}
} // namespace

namespace tooie::scene {

void observe_map(std::uint16_t value) noexcept {
    map_id.store(value, std::memory_order_release);
    map_observed.store(true, std::memory_order_release);
}

void activation_started() noexcept {
    std::uint64_t expected = 0;
    if (!activation_started_ns.compare_exchange_strong(expected, now_ns(), std::memory_order_release,
            std::memory_order_relaxed)) {
        return;
    }
    for (std::size_t index = 0; index < phase_count; ++index) {
        phase_started_ns[index].store(0, std::memory_order_release);
        phase_last_ns[index].store(0, std::memory_order_release);
    }
    world_call_started_ns.store(0, std::memory_order_release);
    world_call_started_pc.store(0, std::memory_order_release);
    world_slowest_call.store(0, std::memory_order_release);
    for (std::size_t index = 0; index < nested_parent_count; ++index) {
        nested_call_started_ns[index].store(0, std::memory_order_release);
        nested_call_started_pc[index].store(0, std::memory_order_release);
        nested_observed_calls[index].store(0, std::memory_order_release);
        nested_slowest_call[index].store(0, std::memory_order_release);
    }
}

void activation_finished() noexcept {
    const auto start = activation_started_ns.exchange(0, std::memory_order_acq_rel);
    const auto now = now_ns();
    if (start == 0 || now < start) return;
    const auto elapsed = now - start;
    last_activation_ns.store(elapsed, std::memory_order_release);
    update_max(elapsed);
    activations_completed.fetch_add(1, std::memory_order_relaxed);
}

void activation_phase_started(ActivationPhase phase) noexcept {
    if (!valid_phase(phase) || activation_started_ns.load(std::memory_order_acquire) == 0) return;
    phase_started_ns[static_cast<std::size_t>(phase)].store(now_ns(), std::memory_order_release);
}

void activation_phase_finished(ActivationPhase phase) noexcept {
    if (!valid_phase(phase)) return;
    const auto start = phase_started_ns[static_cast<std::size_t>(phase)].exchange(0, std::memory_order_acq_rel);
    const auto now = now_ns();
    if (start == 0 || now < start) return;
    phase_last_ns[static_cast<std::size_t>(phase)].store(now - start, std::memory_order_release);
}

void world_call_started(std::uint32_t call_pc) noexcept {
    if (activation_started_ns.load(std::memory_order_acquire) == 0) return;
    world_call_started_pc.store(call_pc, std::memory_order_release);
    world_call_started_ns.store(now_ns(), std::memory_order_release);
}

void world_call_finished(std::uint32_t call_pc) noexcept {
    const auto start = world_call_started_ns.exchange(0, std::memory_order_acq_rel);
    if (start == 0 || world_call_started_pc.load(std::memory_order_acquire) != call_pc) return;
    const auto now = now_ns();
    if (now < start) return;
    const auto microseconds = std::min<std::uint64_t>((now - start) / 1'000U, std::numeric_limits<std::uint32_t>::max());
    const auto candidate = (microseconds << 32U) | call_pc;
    auto observed = world_slowest_call.load(std::memory_order_relaxed);
    while ((observed >> 32U) < microseconds &&
           !world_slowest_call.compare_exchange_weak(observed, candidate, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

void nested_call_started(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept {
    if (activation_started_ns.load(std::memory_order_acquire) == 0) return;
    const auto index = nested_parent_index(parent_pc);
    if (index < 0) return;
    const auto slot = static_cast<std::size_t>(index);
    nested_observed_calls[slot].fetch_add(1, std::memory_order_relaxed);
    nested_call_started_pc[slot].store(child_pc, std::memory_order_release);
    nested_call_started_ns[slot].store(now_ns(), std::memory_order_release);
}

void nested_call_finished(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept {
    const auto index = nested_parent_index(parent_pc);
    if (index < 0) return;
    const auto slot = static_cast<std::size_t>(index);
    const auto start = nested_call_started_ns[slot].exchange(0, std::memory_order_acq_rel);
    if (start == 0 || nested_call_started_pc[slot].load(std::memory_order_acquire) != child_pc) return;
    const auto now = now_ns();
    if (now < start) return;
    const auto microseconds = std::min<std::uint64_t>((now - start) / 1'000U, std::numeric_limits<std::uint32_t>::max());
    const auto candidate = (microseconds << 32U) | child_pc;
    auto observed = nested_slowest_call[slot].load(std::memory_order_relaxed);
    while ((observed >> 32U) < microseconds &&
           !nested_slowest_call[slot].compare_exchange_weak(observed, candidate, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

Snapshot snapshot() noexcept {
    Snapshot result{};
    result.map_available = map_observed.load(std::memory_order_acquire);
    result.map_id = static_cast<std::uint16_t>(map_id.load(std::memory_order_acquire));
    result.activation_active = activation_started_ns.load(std::memory_order_acquire) != 0;
    result.activations_completed = activations_completed.load(std::memory_order_acquire);
    result.last_activation_ms = milliseconds(last_activation_ns.load(std::memory_order_acquire));
    result.max_activation_ms = milliseconds(max_activation_ns.load(std::memory_order_acquire));
    result.section_shutdown_ms = milliseconds(phase_last_ns[static_cast<std::size_t>(ActivationPhase::SectionShutdown)].load(std::memory_order_acquire));
    result.level_shutdown_ms = milliseconds(phase_last_ns[static_cast<std::size_t>(ActivationPhase::LevelShutdown)].load(std::memory_order_acquire));
    result.level_startup_ms = milliseconds(phase_last_ns[static_cast<std::size_t>(ActivationPhase::LevelStartup)].load(std::memory_order_acquire));
    result.section_startup_ms = milliseconds(phase_last_ns[static_cast<std::size_t>(ActivationPhase::SectionStartup)].load(std::memory_order_acquire));
    result.world_setup_ms = milliseconds(phase_last_ns[static_cast<std::size_t>(ActivationPhase::WorldSetup)].load(std::memory_order_acquire));
    const auto slowest_call = world_slowest_call.load(std::memory_order_acquire);
    result.world_slowest_call_pc = static_cast<std::uint32_t>(slowest_call);
    result.world_slowest_call_ms = static_cast<double>(slowest_call >> 32U) / 1'000.0;
    const auto a5c28_slowest = nested_slowest_call[0].load(std::memory_order_acquire);
    result.setup_a5c28_slowest_call_pc = static_cast<std::uint32_t>(a5c28_slowest);
    result.setup_a5c28_slowest_call_ms = static_cast<double>(a5c28_slowest >> 32U) / 1'000.0;
    result.setup_a5c28_observed_calls = nested_observed_calls[0].load(std::memory_order_acquire);
    const auto f73c4_slowest = nested_slowest_call[1].load(std::memory_order_acquire);
    result.setup_f73c4_slowest_call_pc = static_cast<std::uint32_t>(f73c4_slowest);
    result.setup_f73c4_slowest_call_ms = static_cast<double>(f73c4_slowest >> 32U) / 1'000.0;
    result.setup_f73c4_observed_calls = nested_observed_calls[1].load(std::memory_order_acquire);
    return result;
}

void reset() noexcept {
    map_id.store(0, std::memory_order_release);
    map_observed.store(false, std::memory_order_release);
    activation_started_ns.store(0, std::memory_order_release);
    activations_completed.store(0, std::memory_order_release);
    last_activation_ns.store(0, std::memory_order_release);
    max_activation_ns.store(0, std::memory_order_release);
    for (std::size_t index = 0; index < phase_count; ++index) {
        phase_started_ns[index].store(0, std::memory_order_release);
        phase_last_ns[index].store(0, std::memory_order_release);
    }
    world_call_started_ns.store(0, std::memory_order_release);
    world_call_started_pc.store(0, std::memory_order_release);
    world_slowest_call.store(0, std::memory_order_release);
    for (std::size_t index = 0; index < nested_parent_count; ++index) {
        nested_call_started_ns[index].store(0, std::memory_order_release);
        nested_call_started_pc[index].store(0, std::memory_order_release);
        nested_observed_calls[index].store(0, std::memory_order_release);
        nested_slowest_call[index].store(0, std::memory_order_release);
    }
}

} // namespace tooie::scene

extern "C" void tooie_scene_observe_map(std::uint16_t map) noexcept { tooie::scene::observe_map(map); }
extern "C" void tooie_scene_activation_started() noexcept { tooie::scene::activation_started(); }
extern "C" void tooie_scene_activation_finished() noexcept { tooie::scene::activation_finished(); }
extern "C" void tooie_scene_activation_phase_started(std::uint32_t phase) noexcept {
    tooie::scene::activation_phase_started(static_cast<tooie::scene::ActivationPhase>(phase));
}
extern "C" void tooie_scene_activation_phase_finished(std::uint32_t phase) noexcept {
    tooie::scene::activation_phase_finished(static_cast<tooie::scene::ActivationPhase>(phase));
}
extern "C" void tooie_scene_world_call_started(std::uint32_t call_pc) noexcept {
    tooie::scene::world_call_started(call_pc);
}
extern "C" void tooie_scene_world_call_finished(std::uint32_t call_pc) noexcept {
    tooie::scene::world_call_finished(call_pc);
}
extern "C" void tooie_scene_nested_call_started(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept {
    tooie::scene::nested_call_started(parent_pc, child_pc);
}
extern "C" void tooie_scene_nested_call_finished(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept {
    tooie::scene::nested_call_finished(parent_pc, child_pc);
}
