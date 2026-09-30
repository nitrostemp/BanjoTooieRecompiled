#include "camera_interpolation.hpp"
#include "model_interpolation.hpp"
#include "replay_timing.hpp"

#include <array>
#include <atomic>
#include <mutex>

namespace {
std::atomic_bool skip_requested{false};
constexpr std::uint32_t cutscene_active_bit = 1U << 0;
constexpr std::uint32_t cutscene_original_motion_bit = 1U << 1;
std::atomic_uint32_t cutscene_state{0};
std::mutex binding_mutex;
struct Binding {
    std::uint32_t display_list = 0;
    tooie::camera_interpolation::TaskInterpolation interpolation =
        tooie::camera_interpolation::TaskInterpolation::Interpolated;
    std::uint16_t replay_refresh_rate = 0;
};
std::array<Binding, tooie::camera_interpolation::kTaskBindingCapacity> bindings{};
std::size_t binding_head = 0;
std::size_t binding_count = 0;
tooie::camera_interpolation::Stats binding_stats{};

constexpr std::uint32_t normalize_display_list(std::uint32_t address) noexcept {
    return address & 0x03FFFFFFU;
}

void disable_binding_locked(bool overflow) noexcept {
    binding_stats.disabled = true;
    binding_stats.mismatches += overflow ? 0 : 1;
    binding_stats.overflows += overflow ? 1 : 0;
    binding_count = 0;
    binding_head = 0;
    binding_stats.queued_tasks = 0;
    skip_requested.store(false, std::memory_order_release);
}
}

namespace tooie::camera_interpolation {

void request_skip() noexcept {
    skip_requested.store(true, std::memory_order_release);
}

void configure_cutscene_motion(CutsceneMotion motion) noexcept {
    if (motion == CutsceneMotion::Original)
        cutscene_state.fetch_or(cutscene_original_motion_bit, std::memory_order_acq_rel);
    else
        cutscene_state.fetch_and(~cutscene_original_motion_bit, std::memory_order_acq_rel);
}

CutsceneMotion configured_cutscene_motion() noexcept {
    return (cutscene_state.load(std::memory_order_acquire) & cutscene_original_motion_bit)
        ? CutsceneMotion::Original : CutsceneMotion::Interpolated;
}

void set_cutscene_active(bool active) noexcept {
    if (active)
        cutscene_state.fetch_or(cutscene_active_bit, std::memory_order_acq_rel);
    else
        cutscene_state.fetch_and(~cutscene_active_bit, std::memory_order_acq_rel);
}

bool cutscene_active() noexcept {
    return (cutscene_state.load(std::memory_order_acquire) & cutscene_active_bit) != 0;
}

bool bind_task(std::uint32_t display_list) noexcept {
    std::lock_guard lock(binding_mutex);
    if (binding_stats.disabled) {
        skip_requested.store(false, std::memory_order_release);
        return false;
    }
    ++binding_stats.submitted_tasks;
    if (binding_count == bindings.size()) {
        disable_binding_locked(true);
        return false;
    }
    const auto tail = (binding_head + binding_count) % bindings.size();
    const bool boundary_skip = skip_requested.exchange(false, std::memory_order_acq_rel);
    const std::uint32_t captured_cutscene = cutscene_state.load(std::memory_order_acquire);
    const bool original_motion =
        (captured_cutscene & (cutscene_active_bit | cutscene_original_motion_bit)) ==
        (cutscene_active_bit | cutscene_original_motion_bit);
    bindings[tail] = {normalize_display_list(display_list),
        original_motion ? TaskInterpolation::OriginalMotion :
        (boundary_skip ? TaskInterpolation::ProjectionReset : TaskInterpolation::Interpolated),
        tooie::replay_timing::frame_refresh_rate()};
    ++binding_count;
    binding_stats.queued_tasks = binding_count;
    return true;
}

TaskMetadata consume_task_metadata(std::uint32_t display_list) noexcept {
    std::lock_guard lock(binding_mutex);
    if (binding_stats.disabled) return {};
    if (binding_count == 0 ||
        bindings[binding_head].display_list != normalize_display_list(display_list)) {
        disable_binding_locked(false);
        return {};
    }
    const TaskMetadata metadata{bindings[binding_head].interpolation,
        bindings[binding_head].replay_refresh_rate};
    binding_head = (binding_head + 1) % bindings.size();
    --binding_count;
    ++binding_stats.consumed_tasks;
    if (metadata.interpolation != TaskInterpolation::Interpolated) ++binding_stats.consumed_skips;
    if (metadata.interpolation == TaskInterpolation::ProjectionReset) ++binding_stats.consumed_boundary_skips;
    if (metadata.interpolation == TaskInterpolation::OriginalMotion)
        ++binding_stats.consumed_original_motion_tasks;
    if (metadata.replay_refresh_rate != 0) {
        ++binding_stats.consumed_replay_rate_tasks;
        binding_stats.last_replay_refresh_rate = metadata.replay_refresh_rate;
    }
    binding_stats.queued_tasks = binding_count;
    return metadata;
}

TaskInterpolation consume_task_interpolation(std::uint32_t display_list) noexcept {
    return consume_task_metadata(display_list).interpolation;
}

bool consume_skip_for_task(std::uint32_t display_list) noexcept {
    return consume_task_interpolation(display_list) != TaskInterpolation::Interpolated;
}

std::uint64_t consumed_skip_count() noexcept {
    std::lock_guard lock(binding_mutex);
    return binding_stats.consumed_skips;
}

Stats stats() noexcept {
    std::lock_guard lock(binding_mutex);
    return binding_stats;
}

void observe_active_camera(std::uint32_t handle) noexcept {
    // func_800E42B4 selects multiple camera buffers during an ordinary frame
    // (world, HUD and overlays), so pointer changes do not identify a camera
    // cut. Keep the source hook as a classified observation seam, but only
    // explicit lifecycle/cutscene boundaries may request a history reset.
    (void)handle;
}

void reset() noexcept {
    std::lock_guard lock(binding_mutex);
    skip_requested.store(false, std::memory_order_release);
    cutscene_state.fetch_and(~cutscene_active_bit, std::memory_order_acq_rel);
    binding_head = 0;
    binding_count = 0;
    binding_stats = {};
}

} // namespace tooie::camera_interpolation

extern "C" void tooie_camera_interpolation_observe_active(std::uint32_t handle) noexcept {
    tooie::camera_interpolation::observe_active_camera(handle);
}

extern "C" void tooie_camera_interpolation_request_skip() noexcept {
    tooie::camera_interpolation::request_skip();
}

extern "C" bool tooie_camera_interpolation_bind_task(std::uint32_t display_list,
    std::uint32_t display_list_size) noexcept {
    // Both guards bind at the original graphics-task submission seam. Model
    // range admission is independent of camera metadata admission/failure.
    tooie::model_interpolation::bind_task_ranges(display_list, display_list_size);
    return tooie::camera_interpolation::bind_task(display_list);
}
