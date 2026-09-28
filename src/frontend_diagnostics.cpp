#include "frontend_diagnostics.hpp"

#include "frontend_settings.hpp"
#include "game_features.hpp"
#include "minimap.hpp"
#include "pacing_game_delta_observer.hpp"
#include "pacing_present_probe.hpp"
#include "pacing_render_work.hpp"
#include "scene_observer.hpp"
#include "platform_support.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace {
std::string fps(double value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "Present %.1f FPS", value);
    return buffer;
}

std::string bytes(std::uint64_t value, tooie::platform::ProcessMemoryKind kind) {
    const char* label = "Memory";
    switch (kind) {
    case tooie::platform::ProcessMemoryKind::PrivateBytes: label = "Memory private"; break;
    case tooie::platform::ProcessMemoryKind::ResidentSet: label = "Memory RSS"; break;
    case tooie::platform::ProcessMemoryKind::PhysicalFootprint: label = "Memory physical footprint"; break;
    case tooie::platform::ProcessMemoryKind::Unavailable: break;
    }
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s %.1f MiB", label, static_cast<double>(value) / (1024.0 * 1024.0));
    return buffer;
}

std::string msaa(std::uint32_t samples) {
    return samples == 0 ? "MSAA off" : "MSAA " + std::to_string(samples) + "x";
}
} // namespace

namespace tooie::diagnostics {

Snapshot snapshot(const native_host::PacingReadout& pacing) noexcept {
    Snapshot result{};
    result.audio_queue_available = pacing.audio_queue_available;
    result.audio_queued_frames = pacing.audio_queued_frames;
    result.audio_frequency = pacing.audio_frequency;
    result.audio_empty_queue_queries = pacing.audio_empty_queue_queries;
    result.presentation_available = pacing.present_samples != 0 && pacing.present_interval_ms > 0.0;
    result.presentation_hz = result.presentation_available ? 1000.0 / pacing.present_interval_ms : 0.0;
    result.present_interval_ms = pacing.present_interval_ms;
    result.present_samples = pacing.present_samples;
    const auto presents = pacing::present_snapshot();
    result.present_max_interval_ms = presents.max_interval_ms;
    result.present_intervals_over_50ms = presents.intervals_over_50ms;
    result.present_session_max_interval_ms = presents.session_max_interval_ms;
    result.present_session_intervals_over_50ms = presents.session_intervals_over_50ms;
    result.present_session_last_long_interval_ms = presents.session_last_long_interval_ms;
    result.present_session_last_long_elapsed_ms = presents.session_last_long_elapsed_ms;
    result.guest_update_available = pacing.cadence_sample_seconds > 0.0;
    result.guest_update_hz = pacing.guest_update_hz;
    const auto game_delta = pacing::game_delta_snapshot();
    result.game_delta_available = game_delta.samples != 0 && std::isfinite(game_delta.latest_seconds) && game_delta.latest_seconds >= 0.0f;
    result.game_delta_ms = result.game_delta_available ? game_delta.latest_seconds * 1000.0 : 0.0;
    result.host_vi_hz = pacing.host_vi_hz;
    result.graphics_task_hz = pacing.graphics_task_hz;
    result.guest_wait_bypassed = pacing.guest_wait_bypassed;
    result.output_width = pacing.output_width;
    result.output_height = pacing.output_height;
    result.display_hz = pacing.display_hz;
    result.target_hz = pacing.effective_target_hz;
    result.vsync_requested = pacing.vsync_requested;
    result.vsync_actual = pacing.vsync_actual;
    result.gameplay_active = pacing.gameplay_active;
    result.cutscene_active = features::cutscene_active();
    const auto scene = scene::snapshot();
    result.map_available = scene.map_available;
    result.map_id = scene.map_id;
    result.scene_activation_active = scene.activation_active;
    result.scene_activations_completed = scene.activations_completed;
    result.scene_last_activation_ms = scene.last_activation_ms;
    result.scene_max_activation_ms = scene.max_activation_ms;
    result.scene_section_shutdown_ms = scene.section_shutdown_ms;
    result.scene_level_shutdown_ms = scene.level_shutdown_ms;
    result.scene_level_startup_ms = scene.level_startup_ms;
    result.scene_section_startup_ms = scene.section_startup_ms;
    result.scene_world_setup_ms = scene.world_setup_ms;
    result.scene_world_slowest_call_pc = scene.world_slowest_call_pc;
    result.scene_world_slowest_call_ms = scene.world_slowest_call_ms;
    const auto render_work = pacing::render_work_snapshot();
    result.display_list_host_last_ms = render_work.process_display_lists.last_ms;
    result.display_list_host_max_ms = render_work.process_display_lists.max_ms;
    result.display_list_host_recent_average_ms = render_work.process_display_lists.recent_average_ms;
    result.display_list_host_recent_max_ms = render_work.process_display_lists.recent_max_ms;
    result.display_list_host_recent_samples = render_work.process_display_lists.recent_samples;
    result.display_list_host_over_50ms = render_work.process_display_lists.samples_over_50ms;
    result.display_list_cpu_last_ms = render_work.display_list_cpu_last_ms;
    result.display_list_cpu_average_ms = render_work.display_list_cpu_average_ms;
    result.update_screen_host_last_ms = render_work.update_screen.last_ms;
    result.update_screen_host_max_ms = render_work.update_screen.max_ms;
    result.update_screen_host_recent_average_ms = render_work.update_screen.recent_average_ms;
    result.update_screen_host_recent_max_ms = render_work.update_screen.recent_max_ms;
    result.update_screen_host_recent_samples = render_work.update_screen.recent_samples;
    result.update_screen_host_over_50ms = render_work.update_screen.samples_over_50ms;
    // The frontend base scale is multiplied by its downsample factor before
    // RT64 receives it. Report that normalized RT64 request, not the base
    // selector as though it were the internal scale.
    const auto normalized_graphics = frontend::normalize_graphics(
        native_host::requested_graphics_settings(), native_host::graphics_capabilities());
    const auto rt64_graphics = frontend::to_rt64_user_values(normalized_graphics.settings);
    result.internal_resolution_auto = !rt64_graphics.manual_resolution;
    result.internal_resolution_multiplier = static_cast<unsigned>(rt64_graphics.resolution_multiplier);
    result.downsample_multiplier = rt64_graphics.downsample_multiplier;
    result.msaa_samples = rt64_graphics.msaa_samples;
    const auto memory = platform::process_memory();
    result.process_memory_bytes = memory.bytes;
    result.process_memory_kind = memory.kind;
    result.process_memory_available = memory.kind != platform::ProcessMemoryKind::Unavailable;
    return result;
}

std::vector<std::string> lines(const Snapshot& value) {
    std::vector<std::string> result;
    if (value.presentation_available) {
        result.emplace_back(fps(value.presentation_hz));
        char interval[112]{};
        std::snprintf(interval, sizeof(interval), "Frame %.2f ms; max %.2f ms; %llu >50 ms",
            value.present_interval_ms, value.present_max_interval_ms,
            static_cast<unsigned long long>(value.present_intervals_over_50ms));
        result.emplace_back(interval);
    } else {
        result.emplace_back("Present unavailable");
    }
    if (value.guest_update_available || value.game_delta_available) {
        char guest[96]{};
        std::snprintf(guest, sizeof(guest), "Guest %.1f Hz; delta %.3f ms",
            value.guest_update_available ? value.guest_update_hz : 0.0,
            value.game_delta_available ? value.game_delta_ms : 0.0);
        result.emplace_back(guest);
    }
    if (value.guest_update_available && value.guest_wait_bypassed != 0) {
        result.emplace_back("Guest wait bypasses " + std::to_string(value.guest_wait_bypassed) + " in last sample");
    }
    if ((value.output_width != 0 && value.output_height != 0) || value.display_hz != 0 || value.target_hz != 0) {
        result.emplace_back("Output " + std::to_string(value.output_width) + "x" + std::to_string(value.output_height) +
            "; display " + std::to_string(value.display_hz) + " Hz; target " + std::to_string(value.target_hz) + " Hz");
    }
    if (value.gameplay_active && value.internal_resolution_auto) {
        result.emplace_back("Requested RT64 internal resolution Auto; " + msaa(value.msaa_samples));
    } else if (value.gameplay_active) {
        result.emplace_back("Requested RT64 internal " + std::to_string(value.internal_resolution_multiplier) + "x; downsample " +
            std::to_string(value.downsample_multiplier) + "x; " + msaa(value.msaa_samples));
    }
    result.emplace_back(std::string("VSync ") + (value.vsync_requested ? "requested" : "off") +
        (value.vsync_actual ? "; active" : "; unavailable") + "; " +
        (value.cutscene_active ? "cutscene" : (value.gameplay_active ? "game running" : "launcher")) +
        (value.map_available ? "; map " + std::to_string(value.map_id) : ""));
    if (value.scene_activation_active || value.scene_activations_completed != 0) {
        char activation[144]{};
        std::snprintf(activation, sizeof(activation),
            "Scene activation routine %s; last %.2f ms, max %.2f ms (%llu complete)",
            value.scene_activation_active ? "active" : "idle", value.scene_last_activation_ms,
            value.scene_max_activation_ms,
            static_cast<unsigned long long>(value.scene_activations_completed));
        result.emplace_back(activation);
    }
    if (value.display_list_host_max_ms > 0.0) {
        char display_lists[96]{};
        std::snprintf(display_lists, sizeof(display_lists),
            "RT64 list wall avg/max %.1f/%.1f ms; CPU avg %.1f ms",
            value.display_list_host_recent_average_ms, value.display_list_host_recent_max_ms,
            value.display_list_cpu_average_ms);
        result.emplace_back(display_lists);
    }
    if (value.update_screen_host_max_ms > 0.0) {
        char update_screen[96]{};
        std::snprintf(update_screen, sizeof(update_screen),
            "RT64 screen wall avg/max %.1f/%.1f ms",
            value.update_screen_host_recent_average_ms, value.update_screen_host_recent_max_ms);
        result.emplace_back(update_screen);
    }
    if (value.audio_queue_available) {
        const double milliseconds = 1000.0 * static_cast<double>(value.audio_queued_frames) / value.audio_frequency;
        char audio[128]{};
        std::snprintf(audio, sizeof(audio), "Audio %u frames (%.1f ms @ %u Hz); empty queries %llu",
            value.audio_queued_frames, milliseconds, value.audio_frequency,
            static_cast<unsigned long long>(value.audio_empty_queue_queries));
        result.emplace_back(audio);
    }
    if (value.process_memory_available) result.emplace_back(bytes(value.process_memory_bytes, value.process_memory_kind));
    return result;
}

std::string issue_summary(const Snapshot& value) {
    std::ostringstream out;
    out << "present=";
    if (value.presentation_available) out << value.presentation_hz << "Hz," << value.present_interval_ms << "ms";
    else out << "unavailable";
    if (value.present_max_interval_ms > 0.0) {
        out << " present_max=" << value.present_max_interval_ms << "ms"
            << " present_over50=" << value.present_intervals_over_50ms;
    }
    if (value.present_session_max_interval_ms > 0.0) {
        out << " present_session_max=" << value.present_session_max_interval_ms << "ms"
            << " present_session_over50=" << value.present_session_intervals_over_50ms
            << " present_session_last_long=" << value.present_session_last_long_interval_ms << "ms"
            << " present_session_last_long_elapsed=" << value.present_session_last_long_elapsed_ms << "ms";
    }
    out << " guest=";
    if (value.guest_update_available) out << value.guest_update_hz << "Hz";
    else out << "unavailable";
    if (value.game_delta_available) out << " game_delta=" << value.game_delta_ms << "ms";
    out << " output=" << value.output_width << 'x' << value.output_height;
    out << " cutscene=" << (value.cutscene_active ? "active" : "inactive");
    if (value.map_available) out << " map=" << value.map_id;
    if (value.scene_activation_active || value.scene_activations_completed != 0) {
        out << " scene_activation=" << (value.scene_activation_active ? "active" : "idle")
            << " scene_last=" << value.scene_last_activation_ms << "ms"
            << " scene_max=" << value.scene_max_activation_ms << "ms"
            << " scene_completed=" << value.scene_activations_completed;
        out << " scene_section_shutdown=" << value.scene_section_shutdown_ms << "ms"
            << " scene_level_shutdown=" << value.scene_level_shutdown_ms << "ms"
            << " scene_level_startup=" << value.scene_level_startup_ms << "ms"
            << " scene_section_startup=" << value.scene_section_startup_ms << "ms"
            << " scene_world_setup=" << value.scene_world_setup_ms << "ms";
        if (value.scene_world_slowest_call_pc != 0) {
            out << " scene_world_slowest_pc=0x" << std::hex << value.scene_world_slowest_call_pc << std::dec
                << " scene_world_slowest=" << value.scene_world_slowest_call_ms << "ms";
        }
    }
    if (value.display_list_host_max_ms > 0.0) {
        out << " rt64_call_measurement=host_wall_includes_waits_not_gpu_timer";
        out << " rt64_process_call_last=" << value.display_list_host_last_ms << "ms"
            << " rt64_process_call_max=" << value.display_list_host_max_ms << "ms"
            << " rt64_process_call_over50=" << value.display_list_host_over_50ms
            << " rt64_process_recent_avg=" << value.display_list_host_recent_average_ms << "ms"
            << " rt64_process_recent_max=" << value.display_list_host_recent_max_ms << "ms"
            << " rt64_process_recent_samples=" << value.display_list_host_recent_samples
            << " rt64_dl_cpu_last=" << value.display_list_cpu_last_ms << "ms"
            << " rt64_dl_cpu_recent_avg=" << value.display_list_cpu_average_ms << "ms";
    }
    if (value.update_screen_host_max_ms > 0.0) {
        out << " rt64_update_call_last=" << value.update_screen_host_last_ms << "ms"
            << " rt64_update_call_max=" << value.update_screen_host_max_ms << "ms"
            << " rt64_update_call_over50=" << value.update_screen_host_over_50ms
            << " rt64_update_recent_avg=" << value.update_screen_host_recent_average_ms << "ms"
            << " rt64_update_recent_max=" << value.update_screen_host_recent_max_ms << "ms"
            << " rt64_update_recent_samples=" << value.update_screen_host_recent_samples;
    }
    out << " internal=" << (value.internal_resolution_auto ? "auto" : std::to_string(value.internal_resolution_multiplier) + "x")
        << " downsample=" << value.downsample_multiplier << 'x';
    out << " msaa=" << value.msaa_samples << 'x';
    if (value.audio_queue_available) {
        out << " audio_queue=" << value.audio_queued_frames << "frames@" << value.audio_frequency << "Hz";
    }
    if (value.audio_empty_queue_queries != 0) out << " audio_empty_queries=" << value.audio_empty_queue_queries;
    if (value.process_memory_available) {
        const char* label = value.process_memory_kind == platform::ProcessMemoryKind::PrivateBytes ? "private_bytes" :
            value.process_memory_kind == platform::ProcessMemoryKind::ResidentSet ? "resident_set_bytes" :
            "physical_footprint_bytes";
        out << ' ' << label << '=' << value.process_memory_bytes;
    }
    if (minimap::visible()) {
        const auto radar = minimap::trail_snapshot();
        out << " trail_radar_current=" << (radar.current ? 1 : 0)
            << " trail_radar_map=" << radar.map_id
            << " trail_radar_count=" << radar.count
            << " trail_radar_generation=" << radar.generation
            << " trail_radar_x=" << radar.position.x
            << " trail_radar_y=" << radar.position.y
            << " trail_radar_z=" << radar.position.z
            << " trail_radar_yaw=" << radar.yaw_degrees;
    }
    return out.str();
}

} // namespace tooie::diagnostics
