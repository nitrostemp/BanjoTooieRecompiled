#pragma once
#include "frontend_settings.hpp"
#include "ultramodern/ultramodern.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
namespace tooie::persistent_state::devices { struct AudioState; }
#endif

namespace tooie::native_host {
using Log = std::function<void(const char*, const nlohmann::json&)>;
struct Options {
    std::filesystem::path data_directory;
    std::string title = "Banjo-Tooie native mission";
    int width = 960;
    int height = 720;
    bool audio = true;
    bool keyboard_controller = true;
    bool frontend = false;
    Log log;
    // Opt-in evidence of PCM accepted by SDL (not proof of speaker playback).
    // Both fields must enable capture. Keep the directory new/empty per run.
    // Buffer at most this total duration across actual rates (maximum 1 hour).
    // WAV/summary files are finalized by shutdown after producers are joined.
    std::filesystem::path audio_capture_directory;
    double max_capture_seconds = 0.0;
    // Opt-in bounded timing evidence; numeric records only, serialized after
    // joined-producer shutdown. Empty path disables host and guest observations.
    std::filesystem::path audio_pacing_directory;
    size_t audio_pacing_capacity = 32768;
    // Private SDL numeric observer. Empty path is off; fixed capacity262144.
    std::filesystem::path sdl_observation_directory;
};
// All SDL/device cleanup has completed, but evidence finalization failed.
// Continuous host must retain failure while continuing remaining runtime cleanup.
class FinalizationError:public std::runtime_error {public:using std::runtime_error::runtime_error;};
struct Counters {
    uint64_t graphics_tasks_submitted;
    uint64_t graphics_tasks_parsed;
    uint64_t vi_updates_submitted;
    uint64_t audio_frames_queued;
};
struct DisplayModeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 0;
};
struct DisplayInfo {
    frontend::OutputDisplayId id = frontend::OutputDisplayId::Current;
    std::string name;
    uint32_t desktop_width = 0;
    uint32_t desktop_height = 0;
    uint32_t desktop_refresh_hz = 0;
    std::vector<DisplayModeInfo> modes;
};
struct DisplayCapabilities {
    std::vector<DisplayInfo> displays;
    frontend::OutputDisplayId current_display = frontend::OutputDisplayId::Current;
};
struct PacingReadout {
    bool audio_queue_available = false;
    uint32_t audio_queued_frames = 0;
    uint32_t audio_frequency = 0;
    uint64_t audio_empty_queue_queries = 0;
    bool renderer_ready = false;
    bool gameplay_active = false;
    uint32_t display_hz = 0;
    uint32_t swapchain_hz = 0;
    uint32_t source_hz = 0;
    uint32_t requested_target_hz = 0;
    uint32_t effective_target_hz = 0;
    bool vsync_requested = true;
    bool vsync_actual = true;
    frontend::PresentationMode presentation_requested = frontend::PresentationMode::Console;
    frontend::PresentationMode presentation_effective = frontend::PresentationMode::Console;
    double present_interval_ms = 0.0;
    uint64_t present_samples = 0;
    bool timing_available = false;
    double guest_update_hz = 0.0;
    double host_vi_hz = 0.0;
    double graphics_task_hz = 0.0;
    uint64_t guest_wait_bypassed = 0;
    double cadence_sample_seconds = 0.0;
    frontend::OutputMode output_mode = frontend::OutputMode::Windowed;
    frontend::OutputDisplayId effective_display = frontend::OutputDisplayId::Current;
    std::string effective_display_name;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bool display_fallback = false;
};
// SDL/video calls belong to the main thread. Initialize before preinit; poll
// throughout execution. Join runtime/renderer/audio producers before shutdown.
void initialize(const Options& options);
ultramodern::renderer::WindowHandle create_window();
#ifdef _WIN32
// Opens an owned Windows dialog on the SDL thread. Empty path means Cancel.
std::filesystem::path choose_rom_file();
#endif
void poll_events();
void poll_frontend_events();
// Thread-safe handoff from the runtime launcher wait. The SDL owner consumes
// it and applies the saved gameplay profile only after Start Game succeeds.
void request_gameplay_profile() noexcept;
bool gameplay_profile_applied() noexcept;
bool close_requested() noexcept;
void shutdown();
Counters counters() noexcept;
// Captures actual client pixels where supported; success does not verify game
// content, occlusion, or correctness. Must run on the SDL owning thread.
bool capture_visible_client_bmp(const std::filesystem::path& output);
ultramodern::renderer::GraphicsConfig vanilla_graphics_config();
void request_graphics_settings(frontend::GraphicsSettings settings) noexcept;
frontend::GraphicsSettings requested_graphics_settings() noexcept;
void request_display_settings(const frontend::DisplaySettings& settings) noexcept;
frontend::DisplaySettings requested_display_settings() noexcept;
frontend::GraphicsCapabilities graphics_capabilities() noexcept;
DisplayCapabilities display_capabilities();
PacingReadout pacing_readout();
ultramodern::renderer::callbacks_t renderer_callbacks();
ultramodern::audio_callbacks_t audio_callbacks();
ultramodern::input::callbacks_t input_callbacks();
ultramodern::input::callbacks_t frontend_input_callbacks();
ultramodern::gfx_callbacks_t gfx_callbacks();
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
// These are experimental, process-private checkpoint hooks. The caller must
// freeze guest producers first and invoke resume on the quiescing thread.
bool persistent_audio_quiesce(uint64_t epoch, std::chrono::milliseconds timeout) noexcept;
void persistent_audio_resume(uint64_t epoch) noexcept;
void persistent_audio_abandon_for_shutdown(uint64_t epoch) noexcept;
bool persistent_audio_export(persistent_state::devices::AudioState& output, uint64_t epoch) noexcept;
bool persistent_audio_restore_epoch(const persistent_state::devices::AudioState& input, uint64_t epoch) noexcept;
bool persistent_renderer_quiesce(uint64_t epoch, std::chrono::milliseconds timeout) noexcept;
void persistent_renderer_resume(uint64_t epoch) noexcept;
void persistent_renderer_abandon_for_shutdown(uint64_t epoch) noexcept;
bool persistent_renderer_export(std::vector<uint8_t>& output, uint64_t epoch) noexcept;
bool persistent_renderer_validate(const std::vector<uint8_t>& input) noexcept;
bool persistent_renderer_restore_epoch(const std::vector<uint8_t>& input, uint64_t epoch) noexcept;
// Diagnostic reason for the most recent experimental renderer refusal.
const char* persistent_renderer_refusal_reason() noexcept;
#endif
}
