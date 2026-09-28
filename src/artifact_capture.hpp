#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <json/json.hpp>

namespace RT64 {
class State;
}

namespace tooie::artifact_capture {

// Called from the F4 edge before its existing issue marker is posted. The
// request is deliberately tiny: send_dl consumes it for no more than two
// following graphics tasks.
void request() noexcept;

// Claims one requested task. It must be called before gathering any task
// metadata so ordinary graphics tasks pay only one atomic load/CAS.
bool try_claim() noexcept;

struct TaskContext {
    uint64_t sequence = 0;
    uint32_t display_list = 0;
    uint32_t display_list_size = 0;
    std::string display_list_hash;
    const char* display_list_hash_algorithm = "XXH3_64 canonical guest bytes";
    const char* display_list_hash_scope = "root_ostask_post_process_guest_range";
    uint32_t map_id = 0;
    uint32_t gbi = 0;
    bool projection_override = false;
    bool camera_interpolated = false;
    bool original_cutscene_motion = false;
    bool free_camera_enabled = false;
    bool free_camera_input_active = false;
    std::array<float, 3> free_camera_offset{};
};

using LogSink = std::function<void(const char*, nlohmann::json)>;

// `cursor_before` and `cursor_after` identify whether the display list's
// full-sync submitted a workload. Captured draw descriptors are finalized at
// that boundary; rendering later reads them while deriving separate buffers.
void capture_after_display_list(RT64::State& state, uint32_t cursor_before,
    uint32_t cursor_after, const TaskContext& context, const LogSink& log);

} // namespace tooie::artifact_capture
