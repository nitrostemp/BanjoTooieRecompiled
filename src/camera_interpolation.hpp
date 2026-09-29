#pragma once

#include <cstddef>
#include <cstdint>

namespace tooie::camera_interpolation {

inline constexpr std::size_t kTaskBindingCapacity = 64;

enum class CutsceneMotion : std::uint8_t {
    Interpolated = 0,
    Original = 1,
};

// Captured at graphics-task submission. ProjectionReset is the existing
// one-task history guard; OriginalMotion additionally requires the renderer to
// apply ID_IGNORE/all-SKIP groups to both projection and model matrix stacks.
enum class TaskInterpolation : std::uint8_t {
    Interpolated = 0,
    ProjectionReset = 1,
    OriginalMotion = 2,
};

// This FIFO metadata is captured at the same exact OSTask submission seam as
// interpolation state. A zero rate means RT64 must retain automatic VI-rate
// detection for the task.
struct TaskMetadata {
    TaskInterpolation interpolation = TaskInterpolation::Interpolated;
    std::uint16_t replay_refresh_rate = 0;
};

struct Stats {
    std::uint64_t submitted_tasks = 0;
    std::uint64_t consumed_tasks = 0;
    std::uint64_t consumed_skips = 0;
    std::uint64_t consumed_boundary_skips = 0;
    std::uint64_t consumed_original_motion_tasks = 0;
    std::uint64_t consumed_replay_rate_tasks = 0;
    std::uint16_t last_replay_refresh_rate = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t overflows = 0;
    std::size_t queued_tasks = 0;
    bool disabled = false;
};

// Request that the next submitted graphics task use the current projection
// without blending it with the preceding task. Requests coalesce and are
// consumed once by the renderer thread.
void request_skip() noexcept;

// Original motion is an optional renderer fallback for source-identified
// cutscenes. It changes interpolation only; guest simulation and output rate
// remain owned by their existing paths. Interpolated is the default.
void configure_cutscene_motion(CutsceneMotion motion) noexcept;
CutsceneMotion configured_cutscene_motion() noexcept;
void set_cutscene_active(bool active) noexcept;
bool cutscene_active() noexcept;

// Bind a pending request to the exact graphics display list at the original
// submit_rsp_task boundary, then consume it only for that list in send_dl.
// Every graphics submission is recorded so requests retain FIFO task identity.
// Overflow or order mismatch disables this optional guard for the session;
// baseline rendering continues and bind_task returns false.
bool bind_task(std::uint32_t display_list) noexcept;
TaskMetadata consume_task_metadata(std::uint32_t display_list) noexcept;
TaskInterpolation consume_task_interpolation(std::uint32_t display_list) noexcept;
bool consume_skip_for_task(std::uint32_t display_list) noexcept;
std::uint64_t consumed_skip_count() noexcept;
Stats stats() noexcept;

// Observe the handle selected by Tooie's original active-camera setter.
// The setter legitimately alternates world/HUD/overlay buffers, so this seam
// is diagnostic only. Explicit lifecycle and cutscene boundaries request skips.
void observe_active_camera(std::uint32_t handle) noexcept;
void reset() noexcept;

} // namespace tooie::camera_interpolation

extern "C" void tooie_camera_interpolation_observe_active(std::uint32_t handle) noexcept;
extern "C" bool tooie_camera_interpolation_bind_task(std::uint32_t display_list,
    std::uint32_t display_list_size) noexcept;
