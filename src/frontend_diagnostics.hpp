#pragma once

#include "native_host_devices.hpp"
#include "platform_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tooie::diagnostics {

// A presentation observation is derived from successful host presents. Guest
// update rate is separately sampled at the original update boundary.
struct Snapshot {
    bool audio_queue_available = false;
    std::uint32_t audio_queued_frames = 0;
    std::uint32_t audio_frequency = 0;
    std::uint64_t audio_empty_queue_queries = 0;
    bool presentation_available = false;
    double presentation_hz = 0.0;
    double present_interval_ms = 0.0;
    std::uint64_t present_samples = 0;
    double present_max_interval_ms = 0.0;
    std::uint64_t present_intervals_over_50ms = 0;
    double present_session_max_interval_ms = 0.0;
    std::uint64_t present_session_intervals_over_50ms = 0;
    double present_session_last_long_interval_ms = 0.0;
    std::uint64_t present_session_last_long_elapsed_ms = 0;
    bool guest_update_available = false;
    double guest_update_hz = 0.0;
    bool game_delta_available = false;
    double game_delta_ms = 0.0;
    double host_vi_hz = 0.0;
    double graphics_task_hz = 0.0;
    std::uint64_t guest_wait_bypassed = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t display_hz = 0;
    std::uint32_t target_hz = 0;
    bool internal_resolution_auto = false;
    unsigned internal_resolution_multiplier = 1;
    unsigned downsample_multiplier = 1;
    unsigned msaa_samples = 0;
    bool vsync_requested = true;
    bool vsync_actual = true;
    bool gameplay_active = false;
    bool cutscene_active = false;
    bool map_available = false;
    std::uint16_t map_id = 0;
    bool scene_activation_active = false;
    std::uint64_t scene_activations_completed = 0;
    double scene_last_activation_ms = 0.0;
    double scene_max_activation_ms = 0.0;
    double scene_section_shutdown_ms = 0.0;
    double scene_level_shutdown_ms = 0.0;
    double scene_level_startup_ms = 0.0;
    double scene_section_startup_ms = 0.0;
    double scene_world_setup_ms = 0.0;
    std::uint32_t scene_world_slowest_call_pc = 0;
    double scene_world_slowest_call_ms = 0.0;
    double display_list_host_last_ms = 0.0;
    double display_list_host_max_ms = 0.0;
    double display_list_host_recent_average_ms = 0.0;
    double display_list_host_recent_max_ms = 0.0;
    std::uint64_t display_list_host_recent_samples = 0;
    std::uint64_t display_list_host_over_50ms = 0;
    double display_list_cpu_last_ms = 0.0;
    double display_list_cpu_average_ms = 0.0;
    double update_screen_host_last_ms = 0.0;
    double update_screen_host_max_ms = 0.0;
    double update_screen_host_recent_average_ms = 0.0;
    double update_screen_host_recent_max_ms = 0.0;
    std::uint64_t update_screen_host_recent_samples = 0;
    std::uint64_t update_screen_host_over_50ms = 0;
    std::uint64_t process_memory_bytes = 0;
    platform::ProcessMemoryKind process_memory_kind = platform::ProcessMemoryKind::Unavailable;
    bool process_memory_available = false;
};

Snapshot snapshot(const native_host::PacingReadout& pacing) noexcept;

// Lines are intentionally presentation-ready but UI-framework-neutral. Empty
// fields are omitted rather than replaced with invented telemetry values.
std::vector<std::string> lines(const Snapshot& snapshot);
std::string issue_summary(const Snapshot& snapshot);

} // namespace tooie::diagnostics
