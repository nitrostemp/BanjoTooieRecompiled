#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tooie::model_interpolation {

// Physical RDRAM addresses, end exclusive. Only complete Mtx allocations
// produced by a CPU-skinned func_800DE498 invocation are admitted.
struct MatrixRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    // Captured with the guest draw; the renderer must not infer scene state
    // later, after this task may have crossed a scene boundary.
    bool interpolate_camera = false;
    // Explicit task-bound ID for a verified moving CPU actor's single root.
    // Zero preserves the original-pose policy.
    std::uint32_t root_id = 0;
};

inline constexpr std::size_t kMaxRangesPerTask = 128;
inline constexpr std::size_t kMaxQueuedTasks = 64;

struct Stats {
    std::uint64_t recorded_ranges = 0;
    std::uint64_t rejected_ranges = 0;
    std::uint64_t submitted_tasks = 0;
    std::uint64_t consumed_tasks = 0;
    std::uint64_t invalid_ranges = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t overflows = 0;
    std::size_t pending_ranges = 0;
    std::size_t queued_tasks = 0;
    bool disabled = false;
};

// Task binding is independent of the camera/replay metadata guard. Every
// submission consumes pending ranges; only draws whose complete emitted Gfx
// span falls inside this task's validated display-list span are admitted.
bool bind_task_ranges(std::uint32_t display_list,
    std::uint32_t data_size) noexcept;
std::vector<MatrixRange> consume_task_ranges(std::uint32_t display_list) noexcept;

// Native RSP processing is synchronous on the calling thread. A scope confines
// the range test to that task and restores a preceding scope if nested.
class TaskScope {
public:
    explicit TaskScope(const std::vector<MatrixRange>& ranges) noexcept;
    ~TaskScope();
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
private:
    const std::vector<MatrixRange>* previous_ = nullptr;
};

bool original_pose_for_matrix(std::uint32_t physical_address) noexcept;
std::uint32_t original_pose_root_id_for_matrix(std::uint32_t physical_address) noexcept;
bool original_pose_camera_interpolation_for_matrix(std::uint32_t physical_address) noexcept;
Stats stats() noexcept;
void reset() noexcept;

// Optional F4 diagnostic. The caller supplies the existing profile log directory;
// drawing and task submission remain unchanged if the file cannot be opened.
void set_trace_directory(const std::filesystem::path& directory) noexcept;
void arm_trace() noexcept;

} // namespace tooie::model_interpolation

extern "C" void tooie_model_intro_draw_begin() noexcept;
extern "C" void tooie_model_intro_draw_end() noexcept;
extern "C" void tooie_model_backpack_draw_begin(std::uint32_t owner) noexcept;
extern "C" void tooie_model_backpack_draw_end() noexcept;
