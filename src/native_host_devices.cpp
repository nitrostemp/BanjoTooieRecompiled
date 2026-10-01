// Minimal host adapter. RT64 register wiring follows RecompFrontend at
// d0d90ba49f46f4896aaeda362056c21b1e342561; see mission platform attribution.
#include "hle/rt64_application.h"
#include "hle/rt64_rsp.h"
#include "native_host_devices.hpp"
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "persistent_state_devices.hpp"
#include "persistent_state_host_bridge.hpp"
#endif
#include "audio_pacing_observer.hpp"
#include "sdl_audio_observation.hpp"
#include "platform_support.hpp"
#include "pacing_present_probe.hpp"
#include "pacing_render_work.hpp"
#include "pacing_guest_observer.hpp"
#include "pacing_game_delta_observer.hpp"
#include "virtual_clock.hpp"
#include "camera_interpolation.hpp"
#include "model_interpolation.hpp"
#include "artifact_capture.hpp"
#include "graphics_branch_capture.hpp"
#include "continuous_internal.hpp"
#include "free_camera.hpp"
#include "save_progress.hpp"
#include "scene_observer.hpp"
#include "visibility.hpp"
#include "widescreen.hpp"
#include "hud_layout.hpp"
#include "game_features.hpp"
#include "frontend_diagnostics.hpp"
#include "frontend_input_preference.hpp"
#include "imgui_menu.hpp"
#include "imgui_backend.hpp"
#include "librecomp/game.hpp"
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_syswm.h"
#ifdef _WIN32
#include <shobjidl.h>
#include <wrl/client.h>
#endif
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

extern std::atomic_bool exited;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
extern "C" bool tooie_persistent_events_renderer_action(uint64_t epoch,
    bool (*callback)(ultramodern::renderer::RendererContext*, uint64_t) noexcept,
    std::chrono::milliseconds timeout) noexcept;
#endif
// NativeHost remains the sole creator, event owner, and destroyer.
SDL_Window* window = nullptr;

namespace tooie::native_host {
namespace {
RT64::UserConfiguration::AspectRatio hud_extended_aspect(bool widescreen) {
    // Only the counter callback emits side-anchored EX commands. Ordinary
    // NONE origins bypass this setting, preserving all existing projections.
    return widescreen &&
        tooie::hud_layout::latched_counter_layout() == tooie::hud_layout::CounterLayout::Expanded &&
        tooie::hud_layout::latched_proportions() == tooie::hud_layout::Proportions::Original
        ? RT64::UserConfiguration::AspectRatio::Expand
        : RT64::UserConfiguration::AspectRatio::Original;
}
using Json = nlohmann::json;
constexpr uint32_t rdram_bytes = 8 * 1024 * 1024;
// Largest lookup spans in pinned rt64_gbi.cpp textSegments/dataSegments.
constexpr uint32_t gbi_text_scan_bytes = 0x18d0;
constexpr uint32_t gbi_data_scan_bytes = 0x1000; // Also covers unknown-GBI inspection.
struct SignalChannel {
    uint64_t samples = 0, nonzero = 0, negative_rail = 0, positive_rail = 0;
    int32_t peak = 0;
    long double squares = 0;
};
struct SignalStats { std::array<SignalChannel, 2> channels{}; };
struct AudioSegment {
    uint32_t frequency;
    std::vector<int16_t> samples;
    SignalStats signal;
};
struct AudioCapture {
    std::filesystem::path directory;
    uint64_t limit_ns = 0, completed_segment_ns = 0;
    uint64_t submitted_frames = 0, captured_frames = 0, chunks_seen = 0;
    long double submitted_seconds = 0;
    SignalStats submitted_signal, captured_signal;
    std::vector<AudioSegment> segments;
    Json chunks = Json::array();
    bool finalized = false, capped = false;
};
struct Host {
    Options options;
    SDL_Window* window = nullptr;
#ifdef __APPLE__
    SDL_MetalView metal_view = nullptr;
#endif
    SDL_AudioDeviceID audio = 0;
    std::thread::id main_thread;
    uint32_t frequency = 48000;
    std::atomic_uint display_rate{60};
    std::atomic_bool initialized{false}, close{false};
    bool frontend_input_initialized = false;
    std::atomic<uint32_t> keyboard{0}, renderer_count{0};
    std::atomic<uint64_t> submitted{0}, parsed{0}, vi_updates{0}, audio_frames{0};
    std::atomic_int logged_main_volume{-1};
    save_progress::Status save_status = save_progress::Status::Idle;
    std::mutex audio_mutex, log_mutex;
    std::unique_ptr<AudioCapture> capture;
    sdl_observation::Session sdl_observation;
} host;

std::atomic_bool requested_fullscreen{false};
std::atomic_uint requested_resolution{1}, requested_msaa{0};
std::atomic_bool requested_auto_resolution{false};
std::atomic_uint requested_downsample{1};
std::atomic_bool requested_vsync{true};
std::atomic_uint requested_rate_mode{uint32_t(frontend::OutputRateMode::Display)};
std::atomic_uint requested_custom_rate{60};
std::atomic_uint requested_presentation{uint32_t(frontend::PresentationMode::Early)};
std::atomic_uint requested_output_mode{uint32_t(frontend::OutputMode::Windowed)};
std::atomic_uint requested_output_display{uint32_t(frontend::OutputDisplayId::Current)};
std::atomic_uint requested_output_resolution{uint32_t(frontend::OutputResolutionId::DefaultWindowed)};
std::atomic_uint requested_output_fit{uint32_t(frontend::OutputFit::PreserveGameAspect)};
std::atomic_uint64_t requested_display_generation{1};
uint64_t applied_display_generation = 0;
std::atomic_bool sample_positions_supported{false};
std::atomic_uint max_msaa_samples{0};
std::atomic_bool renderer_ready{false};
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
class NativeRenderer;
std::atomic<NativeRenderer*> persistent_active_renderer{nullptr};
std::atomic_uint64_t persistent_renderer_lease_epoch{0};
std::atomic<const char*> persistent_renderer_refusal{"not_requested"};
std::mutex persistent_renderer_restore_mutex;
std::shared_ptr<const std::vector<uint8_t>> persistent_renderer_restore_blob;
std::atomic_uint64_t persistent_renderer_restore_epoch_active{0};
#endif
std::atomic_bool frontend_gameplay_active{false};
std::atomic_bool frontend_gameplay_requested{false};
std::atomic_bool frontend_gameplay_graphics_applied{false};
std::atomic_uint actual_swapchain_hz{0}, actual_requested_target_hz{0}, actual_effective_target_hz{0};
std::atomic_uint actual_presentation{uint32_t(frontend::PresentationMode::Console)};
std::atomic<double> measured_guest_update_hz{0.0}, measured_host_vi_hz{0.0}, measured_graphics_task_hz{0.0};
std::atomic<double> measured_cadence_seconds{0.0};
std::atomic_uint64_t measured_guest_wait_bypassed{0};
std::mutex display_state_mutex;
DisplayCapabilities current_display_capabilities;
PacingReadout current_display_readout;
// Only the SDL owner thread inspects SDL controller state. Guest
// callbacks consume this published snapshot and never race controller hotplug.
std::atomic_bool frontend_physical_controller{false};
std::atomic_bool frontend_rumble_capable{false};
std::atomic_int frontend_presence_log_state{-1};
std::atomic_uint64_t audio_queue_sample{0};
std::atomic_uint64_t empty_audio_queue_queries{0};
// Protected by audio_mutex. Intentional speedup empties the queue; exclude its
// normal refill interval from the diagnostic starvation observation count.
bool audio_rebuffering_after_fast_forward = false;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
// SDL's playback queue cannot be read back. Mirror only PCM accepted by
// SDL_QueueAudio, then reconcile its consumed prefix against the paused queue.
// The limit matches AudioState's two-second maximum at 192 kHz stereo.
constexpr size_t persistent_audio_max_samples = 2U * 192000U * 2U;
std::vector<int16_t> persistent_audio_ledger;
bool persistent_audio_ledger_valid = true;
std::atomic_uint64_t persistent_audio_lease_epoch{0};
std::unique_lock<std::mutex> persistent_audio_lease;

bool persistent_audio_reconcile_locked() noexcept {
    if (!persistent_audio_ledger_valid) return false;
    const Uint32 bytes = host.audio ? SDL_GetQueuedAudioSize(host.audio) : 0;
    if ((bytes % (sizeof(int16_t) * 2U)) != 0) return false;
    const size_t queued = bytes / sizeof(int16_t);
    if (queued > persistent_audio_ledger.size()) return false;
    const size_t consumed = persistent_audio_ledger.size() - queued;
    if ((consumed & 1U) != 0) return false;
    persistent_audio_ledger.erase(persistent_audio_ledger.begin(),
        persistent_audio_ledger.begin() + consumed);
    return true;
}
#endif
std::atomic_bool save_notice_owned{false};
std::chrono::steady_clock::time_point save_notice_deadline{};

enum class InterpolationDiagnosticMode : uint8_t {
    Automatic,
    Off,
    CameraOnly,
    ModelOnly,
    ModelLinear,
};
struct InterpolationDiagnostic {
    InterpolationDiagnosticMode mode = InterpolationDiagnosticMode::ModelLinear;
    bool requested = false;
    bool valid = true;
};
const InterpolationDiagnostic& interpolation_diagnostic() noexcept {
    static const InterpolationDiagnostic selection = [] {
        const char* value = std::getenv("TOOIE_INTERPOLATION_DIAGNOSTIC");
        if (!value) return InterpolationDiagnostic{};
        const std::string_view name(value);
        if (name == "normal" || name == "default")
            return InterpolationDiagnostic{InterpolationDiagnosticMode::ModelLinear, true, true};
        if (name == "auto") return InterpolationDiagnostic{InterpolationDiagnosticMode::Automatic, true, true};
        if (name == "off") return InterpolationDiagnostic{InterpolationDiagnosticMode::Off, true, true};
        if (name == "camera-only") return InterpolationDiagnostic{InterpolationDiagnosticMode::CameraOnly, true, true};
        if (name == "model-only") return InterpolationDiagnostic{InterpolationDiagnosticMode::ModelOnly, true, true};
        if (name == "model-linear") return InterpolationDiagnostic{InterpolationDiagnosticMode::ModelLinear, true, true};
        return InterpolationDiagnostic{InterpolationDiagnosticMode::ModelLinear, true, false};
    }();
    return selection;
}
const char* interpolation_diagnostic_name(InterpolationDiagnosticMode mode) noexcept {
    switch (mode) {
    case InterpolationDiagnosticMode::Automatic: return "auto";
    case InterpolationDiagnosticMode::Off: return "off";
    case InterpolationDiagnosticMode::CameraOnly: return "camera-only";
    case InterpolationDiagnosticMode::ModelOnly: return "model-only";
    case InterpolationDiagnosticMode::ModelLinear: return "model-linear";
    }
    return "normal";
}

void show_save_notice(const char* text, bool successful = false) {
    menu::show_notice("Save Progress", text, successful, successful ? 3.0 : 0.0);
}

void log(const char* event, Json details = Json::object()) {
    std::lock_guard guard(host.log_mutex);
    if (host.options.log) host.options.log(event, details);
    else std::fprintf(stdout, "%s\n", Json{{"event", event}, {"details", std::move(details)}}.dump().c_str());
}
const char* save_status_text(save_progress::Status status) noexcept {
    switch (status) {
        case save_progress::Status::Idle: return "idle";
        case save_progress::Status::PendingPauseMenu: return "received; open the original pause menu";
        case save_progress::Status::SubmittingToGame: return "submitting to original game";
        case save_progress::Status::SubmittedToGame: return "original manager returned; persistence unacknowledged";
        case save_progress::Status::ExpiredOutsidePause: return "expired outside pause; no save";
        case save_progress::Status::RejectedUnavailable: return "original manager unavailable; no save";
        case save_progress::Status::Persisted: return "save file finalized successfully";
        case save_progress::Status::PersistenceFailed: return "save file persistence failed";
    }
    return "unknown";
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void require_main() {
    require(host.initialized.load(), "Native host is not initialized");
    require(host.main_thread == std::this_thread::get_id(), "SDL window/input must run on owning main thread");
}
void sdl_require(bool value, const char* operation) {
    if (!value) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}
void check_range(uint32_t address, uint32_t length) {
    require(address < rdram_bytes && length <= rdram_bytes - address, "Graphics task range outside original 8 MiB RDRAM");
}
std::string hash_guest_bytes(const uint8_t* memory, uint32_t address, uint32_t length) {
    check_range(address, length);
    std::vector<uint8_t> bytes(length);
    for (uint32_t i = 0; i < length; ++i) bytes[i] = memory[(address + i) ^ 3];
    return hex64(XXH3_64bits(bytes.data(), bytes.size()));
}

SignalStats measure_audio(const int16_t* samples, size_t frames) {
    SignalStats result;
    for (size_t i = 0; i < frames * 2; ++i) {
        auto& channel = result.channels[i & 1];
        const int32_t sample = samples[i];
        ++channel.samples;
        channel.nonzero += sample != 0;
        channel.negative_rail += sample == -32768;
        channel.positive_rail += sample == 32767;
        channel.peak = std::max(channel.peak, std::abs(sample));
        channel.squares += static_cast<long double>(sample) * sample;
    }
    return result;
}
void add_signal(SignalStats& target, const SignalStats& source) {
    for (size_t i = 0; i < 2; ++i) {
        auto& a = target.channels[i]; const auto& b = source.channels[i];
        a.samples += b.samples; a.nonzero += b.nonzero;
        a.negative_rail += b.negative_rail; a.positive_rail += b.positive_rail;
        a.peak = std::max(a.peak, b.peak); a.squares += b.squares;
    }
}
Json channel_json(const SignalChannel& channel) {
    return {{"samples", channel.samples}, {"nonzero_samples", channel.nonzero},
        {"has_samples", channel.samples != 0}, {"silence", channel.samples != 0 && channel.nonzero == 0}, {"peak_s16", channel.peak},
        {"peak_full_scale", double(channel.peak) / 32768.0},
        {"rms_s16", channel.samples ? double(std::sqrt(channel.squares / channel.samples)) : 0.0},
        {"rms_full_scale", channel.samples ? double(std::sqrt(channel.squares / channel.samples) / 32768.0L) : 0.0},
        {"negative_rail_samples", channel.negative_rail}, {"positive_rail_samples", channel.positive_rail},
        {"clipping_indicator_samples", channel.negative_rail + channel.positive_rail}};
}
Json signal_json(const SignalStats& stats) {
    SignalChannel combined = stats.channels[0];
    const auto& right = stats.channels[1];
    combined.samples += right.samples; combined.nonzero += right.nonzero;
    combined.negative_rail += right.negative_rail; combined.positive_rail += right.positive_rail;
    combined.peak = std::max(combined.peak, right.peak); combined.squares += right.squares;
    return {{"combined", channel_json(combined)}, {"left", channel_json(stats.channels[0])},
        {"right", channel_json(right)}};
}
uint64_t segment_duration_ns(const AudioSegment& segment) {
    // Round each complete rate segment upward once, never once per callback.
    // This conservatively limits total duration without per-chunk drift.
    return ((segment.samples.size() / 2) * 1000000000ULL + segment.frequency - 1) / segment.frequency;
}
void capture_audio(const std::vector<int16_t>& samples) {
    if (!host.capture) return;
    auto& capture = *host.capture;
    const size_t frames = samples.size() / 2;
    const auto submitted = measure_audio(samples.data(), frames);
    ++capture.chunks_seen;
    capture.submitted_frames += frames;
    capture.submitted_seconds += static_cast<long double>(frames) / host.frequency;
    add_signal(capture.submitted_signal, submitted);
    if (capture.capped) return;
    uint64_t completed = capture.completed_segment_ns;
    const bool rate_changed = capture.segments.empty() || capture.segments.back().frequency != host.frequency;
    if (rate_changed && !capture.segments.empty()) completed += segment_duration_ns(capture.segments.back());
    const uint64_t existing = rate_changed ? 0 : capture.segments.back().samples.size() / 2;
    const uint64_t allowed = completed < capture.limit_ns ? (capture.limit_ns - completed) * host.frequency / 1000000000ULL : 0;
    const size_t take = size_t(std::min<uint64_t>(frames, allowed > existing ? allowed - existing : 0));
    if (!take) { capture.capped = true; return; }
    if (take < frames) capture.capped = true;
    if (rate_changed) {
        capture.completed_segment_ns = completed;
        capture.segments.push_back({host.frequency, {}, {}});
    }
    auto& segment = capture.segments.back();
    const auto captured = take == frames ? submitted : measure_audio(samples.data(), take);
    segment.samples.insert(segment.samples.end(), samples.begin(), samples.begin() + take * 2);
    add_signal(segment.signal, captured);
    add_signal(capture.captured_signal, captured);
    capture.captured_frames += take;
    capture.chunks.push_back({{"submission", capture.chunks_seen}, {"segment", capture.segments.size()},
        {"frequency", host.frequency}, {"submitted_frames", frames}, {"captured_frames", take},
        {"first_frame_in_segment", existing}, {"submitted_signal", signal_json(submitted)},
        {"captured_signal", signal_json(captured)}});
}
void write_le16(std::ostream& output, uint16_t value) {
    const char bytes[]{char(value), char(value >> 8)}; output.write(bytes, 2);
}
void write_le32(std::ostream& output, uint32_t value) {
    const char bytes[]{char(value), char(value >> 8), char(value >> 16), char(value >> 24)}; output.write(bytes, 4);
}
void finish_audio_capture() {
    if (!host.capture || host.capture->finalized) return;
    auto& capture = *host.capture;
    Json files = Json::array();
    long double captured_seconds = 0;
    for (size_t index = 0; index < capture.segments.size(); ++index) {
        const auto& segment = capture.segments[index];
        std::ostringstream name;
        name << "audio-" << std::setfill('0') << std::setw(4) << index + 1 << '-' << segment.frequency << "Hz.wav";
        const auto path = capture.directory / name.str();
        require(!std::filesystem::exists(path), "Refusing to overwrite an audio capture");
        const uint64_t bytes = segment.samples.size() * sizeof(int16_t);
        require(bytes <= UINT32_MAX - 36, "Captured WAV exceeds RIFF capacity");
        std::ofstream file(path, std::ios::binary);
        require(bool(file), "Cannot create captured WAV");
        file.write("RIFF", 4); write_le32(file, uint32_t(bytes + 36)); file.write("WAVEfmt ", 8);
        write_le32(file, 16); write_le16(file, 1); write_le16(file, 2);
        write_le32(file, segment.frequency); write_le32(file, segment.frequency * 4);
        write_le16(file, 4); write_le16(file, 16); file.write("data", 4); write_le32(file, uint32_t(bytes));
        // Serialize explicitly little endian; SDL's native-endian input is intact.
        std::vector<char> pcm(static_cast<size_t>(bytes));
        for (size_t i = 0; i < segment.samples.size(); ++i) {
            const auto sample = uint16_t(segment.samples[i]);
            pcm[i * 2] = char(sample); pcm[i * 2 + 1] = char(sample >> 8);
        }
        file.write(pcm.data(), std::streamsize(pcm.size())); file.close();
        require(bool(file), "Failed to finalize captured WAV");
        const auto frames = segment.samples.size() / 2;
        const double duration = double(frames) / segment.frequency;
        captured_seconds += static_cast<long double>(frames) / segment.frequency;
        files.push_back({{"file", name.str()}, {"frequency", segment.frequency}, {"frames", frames},
            {"pcm_bytes", bytes}, {"seconds", duration}, {"signal", signal_json(segment.signal)}});
    }
    Json summary{{"schema", "tooie-submitted-pcm-v1"},
        {"capture_stage", "S16 stereo accepted by SDL_QueueAudio, after guest channel ordering, before device conversion"},
        {"playback_verified", false}, {"game_audio_verified", false},
        {"clipping_measure", "Samples equal to -32768 or +32767 are indicators, not proof of clipping"},
        {"max_capture_seconds", double(capture.limit_ns) / 1e9}, {"submitted_chunks", capture.chunks_seen},
        {"submitted_frames", capture.submitted_frames}, {"submitted_seconds", double(capture.submitted_seconds)},
        {"captured_frames", capture.captured_frames}, {"captured_seconds", double(captured_seconds)},
        {"frames_omitted_from_capture", capture.submitted_frames - capture.captured_frames},
        {"submitted_signal", signal_json(capture.submitted_signal)}, {"captured_signal", signal_json(capture.captured_signal)},
        {"files", files}, {"chunks", capture.chunks}, {"finalized_after_producers_joined", true}};
    const auto path = capture.directory / "summary.json";
    require(!std::filesystem::exists(path), "Refusing to overwrite audio capture summary");
    std::ofstream file(path); file << summary.dump(2) << '\n'; file.close();
    require(bool(file), "Failed to finalize audio capture summary");
    capture.finalized = true;
    log("native_audio_capture_finalized", {{"directory", capture.directory.string()}, {"files", files.size()},
        {"captured_frames", capture.captured_frames}, {"captured_seconds", double(captured_seconds)},
        {"submitted_frames", capture.submitted_frames}, {"signal", signal_json(capture.captured_signal)},
        {"playback_verified", false}, {"game_audio_verified", false}});
}

void set_frequency(uint32_t frequency) {
    require(host.initialized.load() && host.options.audio, "Audio requested without an enabled SDL host");
    require(frequency >= 1000 && frequency <= 192000, "Unsupported guest audio sample frequency");
    std::lock_guard guard(host.audio_mutex);
    if (host.audio && host.frequency == frequency) return;
    // Do not throw away old-rate samples on a rate change. This ordinarily runs
    // once before the game queues audio. A stalled output is an explicit error.
    if (host.audio) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (SDL_GetQueuedAudioSize(host.audio) != 0) {
            require(std::chrono::steady_clock::now() < deadline, "Audio did not drain before frequency change");
            SDL_Delay(1);
        }
        SDL_CloseAudioDevice(host.audio);
        host.audio = 0;
    }
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = int(frequency);
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = nullptr;
    // Zero allowed_changes keeps the application queue in original-rate S16
    // stereo units; SDL performs any conversion required by the hardware.
    host.audio = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    sdl_require(host.audio != 0, "SDL_OpenAudioDevice");
    require(obtained.freq == int(frequency) && obtained.channels == 2 && obtained.format == AUDIO_S16SYS,
            "SDL changed the application audio queue format");
    host.frequency = frequency;
    audio_rebuffering_after_fast_forward = false;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    persistent_audio_ledger.clear();
    persistent_audio_ledger_valid = true;
#endif
    SDL_PauseAudioDevice(host.audio, 0);
    if (audio_pacing::enabled()) {
        audio_pacing::Record record; record.kind = audio_pacing::Kind::opened;
        record.times[0] = audio_pacing::now_ns();
        record.values = {frequency, obtained.channels, obtained.format, obtained.samples, obtained.size,
                         desired.samples, desired.size, host.audio};
        audio_pacing::append(record);
    }
    log("native_audio_opened", {{"frequency", frequency}, {"channels", 2}, {"format", "S16 native endian"},
        {"driver", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : ""}, {"game_audio_verified", false}});
}
void queue_samples(int16_t* samples, size_t sample_count) {
    const bool measure = audio_pacing::enabled();
    audio_pacing::Record record; record.kind = audio_pacing::Kind::queue;
    if (measure) record.times[0] = audio_pacing::now_ns();
    struct StoreRecordOnExit {
        bool measure;
        audio_pacing::Record& record;
        ~StoreRecordOnExit() {
            if (measure) { record.times[7] = audio_pacing::now_ns(); audio_pacing::append(record); }
        }
    } store_record{measure, record};
    require(host.initialized.load() && host.options.audio, "Audio samples submitted without native audio");
    require((sample_count & 1) == 0, "Audio sample count is not complete stereo frames");
    require(sample_count <= std::numeric_limits<Uint32>::max() / sizeof(int16_t), "Audio queue chunk is too large");
    if (sample_count == 0) return;
    require(samples != nullptr, "Null guest audio sample buffer");
    std::lock_guard guard(host.audio_mutex);
    if (measure) record.times[1] = audio_pacing::now_ns();
    require(host.audio != 0, "Audio device has not been opened");
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if (!persistent_audio_reconcile_locked()) persistent_audio_ledger_valid = false;
#endif
    if (host.options.frontend && tooie::timing::rate() > 1U) {
        // Intentional fast-forward advances the original audio engine too.
        // Discard its accelerated output instead of growing a real-time SDL
        // queue or playing it back later at the wrong speed.
        SDL_ClearQueuedAudio(host.audio);
        audio_rebuffering_after_fast_forward = true;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        persistent_audio_ledger.clear();
        persistent_audio_ledger_valid = true;
#endif
        audio_queue_sample.store(uint64_t(host.frequency) << 32, std::memory_order_release);
        host.audio_frames.fetch_add(sample_count / 2);
        return;
    }
    std::vector<int16_t> corrected(sample_count);
    // RDRAM stores native words; the two int16 samples within each stereo frame
    // are reversed in host memory. Preserve sample amplitude and ordering.
    for (size_t i = 0; i < sample_count; i += 2) {
        corrected[i] = samples[i + 1];
        corrected[i + 1] = samples[i];
    }
    const auto main_volume = host.options.frontend
        ? static_cast<unsigned>(std::clamp(menu::get_number("main_volume", 100.0), 0.0, 100.0))
        : 100U;
    frontend::apply_main_volume(corrected, main_volume);
    if (measure) {
        record.values[0] = SDL_GetQueuedAudioSize(host.audio);
        record.times[2] = audio_pacing::now_ns();
        record.values[2] = sample_count / 2;
        record.values[3] = host.frequency;
        record.values[4] = host.audio;
    }
    const int queue_result = SDL_QueueAudio(host.audio, corrected.data(), Uint32(sample_count * sizeof(int16_t)));
    if (measure) {
        record.times[3] = audio_pacing::now_ns();
        record.values[1] = SDL_GetQueuedAudioSize(host.audio);
        record.times[4] = audio_pacing::now_ns();
        record.values[5] = uint32_t(queue_result);
        record.flags = queue_result != 0 ? 1 : 4;
    }
    sdl_require(queue_result == 0, "SDL_QueueAudio");
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if (persistent_audio_ledger_valid) {
        try {
            if (sample_count > persistent_audio_max_samples - persistent_audio_ledger.size()) {
                persistent_audio_ledger_valid = false;
            } else {
                persistent_audio_ledger.insert(persistent_audio_ledger.end(), corrected.begin(), corrected.end());
                if (!persistent_audio_reconcile_locked()) persistent_audio_ledger_valid = false;
            }
        } catch (...) {
            persistent_audio_ledger_valid = false;
        }
    }
#endif
    audio_rebuffering_after_fast_forward = false;
    audio_queue_sample.store((uint64_t(host.frequency) << 32) |
        uint32_t(SDL_GetQueuedAudioSize(host.audio) / 4), std::memory_order_release);
    const auto prior_audio_frames = host.audio_frames.fetch_add(sample_count / 2);
    capture_audio(corrected);
    if (measure) record.times[5] = audio_pacing::now_ns();
    const int prior_logged_volume = host.logged_main_volume.exchange(static_cast<int>(main_volume));
    if (!host.options.frontend || prior_audio_frames == 0 || prior_logged_volume != static_cast<int>(main_volume)) {
        log("native_audio_queued", {{"frames", sample_count / 2}, {"frequency", host.frequency},
            {"main_volume_percent", main_volume},
            {"queued_frames", SDL_GetQueuedAudioSize(host.audio) / 4},
            {"xxh3_64_host_s16", hex64(XXH3_64bits(corrected.data(), corrected.size() * sizeof(int16_t)))}});
    }
    if (measure) {
        record.times[6] = audio_pacing::now_ns();
        record.flags = 2; // Queue, capture and existing trace call all returned.
    }
}
size_t frames_remaining() {
    const bool measure = audio_pacing::enabled();
    audio_pacing::Record record; record.kind = audio_pacing::Kind::query;
    if (measure) record.times[0] = audio_pacing::now_ns();
    std::lock_guard guard(host.audio_mutex);
    if (measure) record.times[1] = audio_pacing::now_ns();
    // Controlled B experiment: match the pinned frontend's additional one-VI
    // source-frame reporting bias. The runtime's separate byte bias is unchanged.
    if (host.audio && host.options.frontend && tooie::timing::rate() > 1U) {
        SDL_ClearQueuedAudio(host.audio);
        audio_rebuffering_after_fast_forward = true;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        persistent_audio_ledger.clear();
        persistent_audio_ledger_valid = true;
#endif
    }
    const uint32_t bytes = host.audio ? SDL_GetQueuedAudioSize(host.audio) : 0;
    const size_t raw_frames = bytes / (2 * sizeof(int16_t));
    if (host.audio && raw_frames == 0 && host.audio_frames.load(std::memory_order_relaxed) != 0 &&
        tooie::timing::rate() == 1U && !audio_rebuffering_after_fast_forward) {
        // An empty SDL queue observed by a query is a useful starvation clue,
        // not proof that the hardware emitted an audible underrun.
        empty_audio_queue_queries.fetch_add(1, std::memory_order_relaxed);
    }
    audio_queue_sample.store(host.audio ? (uint64_t(host.frequency) << 32) | uint32_t(raw_frames) : 0,
        std::memory_order_release);
    const size_t reporting_bias = host.frequency / 60;
    const size_t frames = raw_frames > reporting_bias ? raw_frames - reporting_bias : 0;
    if (measure) {
        record.times[2] = audio_pacing::now_ns();
        record.values = {bytes, frames, host.frequency, host.audio};
        audio_pacing::append(record);
    }
    return frames;
}
void poll_input_snapshot() {} // Main-thread poll_events publishes the snapshot.
bool get_input(int port, uint16_t* buttons, float* x, float* y) {
    if (port != 0 || !host.initialized.load() || !host.options.keyboard_controller) return false;
    require(buttons && x && y, "Null controller output pointer");
    const uint32_t state = host.keyboard.load(std::memory_order_acquire);
    *buttons = uint16_t(state);
    *x = float(int8_t(state >> 16)) / 85.0f;
    *y = float(int8_t(state >> 24)) / 85.0f;
    return true;
}
ultramodern::input::connected_device_info_t get_connected(int port) {
    const bool present = port == 0 && host.initialized.load() && host.options.keyboard_controller;
    return {present ? ultramodern::input::Device::Controller : ultramodern::input::Device::None,
            ultramodern::input::Pak::None};
}
void no_interrupt_callback() {
    // Pinned runtime events.cpp owns SP/DP completion around send_dl; RT64 must
    // not enqueue duplicate guest events from this optional plugin callback.
}
ultramodern::renderer::SetupResult map_result(RT64::Application::SetupResult result) {
    using Out = ultramodern::renderer::SetupResult;
    using In = RT64::Application::SetupResult;
    switch (result) {
    case In::Success: return Out::Success;
    case In::DynamicLibrariesNotFound: return Out::DynamicLibrariesNotFound;
    case In::InvalidGraphicsAPI: return Out::InvalidGraphicsAPI;
    case In::GraphicsAPINotFound: return Out::GraphicsAPINotFound;
    case In::GraphicsDeviceNotFound: return Out::GraphicsDeviceNotFound;
    }
    return Out::GraphicsDeviceNotFound;
}

class NativeRenderer final : public ultramodern::renderer::RendererContext {
    std::array<uint8_t, 0x40> header{};
    std::array<uint8_t, 0x1000> dmem{}, imem{};
    std::array<uint32_t, 9> registers{};
    std::unique_ptr<RT64::Application> app;
    bool registered = false;
    bool dummy_logged = false;
    uint64_t last_ucode_text_hash = 0, last_ucode_data_hash = 0;
    frontend::GraphicsSettings applied_settings{};
    bool applied_widescreen = false;
    bool applied_cutscene_pillarbox = false;
    double applied_aspect_target = 16.0 / 9.0;
    std::chrono::steady_clock::time_point cadence_start{};
    uint64_t cadence_vi = 0, cadence_tasks = 0;
    pacing::GuestUpdateSnapshot cadence_guest{};
    bool cadence_active = false;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    std::thread::id persistent_gfx_thread{};
    uint32_t persistent_window_thread_id = 0;
#endif

    void sample_guest_cadence(bool write_log) {
        const auto now = std::chrono::steady_clock::now();
        const auto current_guest = pacing::guest_update_snapshot();
        const auto current_vi = host.vi_updates.load(std::memory_order_acquire);
        const auto current_tasks = host.parsed.load(std::memory_order_acquire);
        if (!cadence_active) {
            cadence_start = now; cadence_vi = current_vi; cadence_tasks = current_tasks; cadence_guest = current_guest;
            cadence_active = true;
            return;
        }
        const double seconds = std::chrono::duration<double>(now - cadence_start).count();
        if (!write_log && seconds < 1.0) return;
        if (write_log && seconds < 0.25) {
            cadence_start = now; cadence_vi = current_vi; cadence_tasks = current_tasks; cadence_guest = current_guest;
            return;
        }
        const auto update_delta = current_guest.updates - cadence_guest.updates;
        const auto bypass_delta = current_guest.wait_bypassed - cadence_guest.wait_bypassed;
        const auto vi_delta = current_vi - cadence_vi;
        const auto task_delta = current_tasks - cadence_tasks;
        const double update_hz = update_delta / seconds;
        const double vi_hz = vi_delta / seconds;
        const double task_hz = task_delta / seconds;
        measured_guest_update_hz.store(update_hz, std::memory_order_release);
        measured_host_vi_hz.store(vi_hz, std::memory_order_release);
        measured_graphics_task_hz.store(task_hz, std::memory_order_release);
        measured_guest_wait_bypassed.store(bypass_delta, std::memory_order_release);
        measured_cadence_seconds.store(seconds, std::memory_order_release);
        if (write_log) {
            log("native_guest_cadence", {{"sample_seconds", seconds}, {"guest_update_entries", update_delta},
                {"guest_update_hz", update_hz}, {"guest_wait_bypassed", bypass_delta},
                {"host_vi_callbacks", vi_delta}, {"host_vi_hz", vi_hz},
                {"graphics_tasks_parsed", task_delta}, {"graphics_task_hz", task_hz},
                {"output_rate_mode", uint32_t(applied_settings.output_rate_mode)},
                {"presentation", applied_settings.presentation_mode == frontend::PresentationMode::Early ? "early" : "console"},
                {"measurement", "guest update entry and wait-bypass counts; not an inferred slowdown"}});
        }
        cadence_start = now; cadence_vi = current_vi; cadence_tasks = current_tasks; cadence_guest = current_guest;
    }

    RT64::UserConfiguration::Antialiasing rt64_msaa(unsigned samples) const {
        switch (samples) {
            case 2: return RT64::UserConfiguration::Antialiasing::MSAA2X;
            case 4: return RT64::UserConfiguration::Antialiasing::MSAA4X;
            case 8: return RT64::UserConfiguration::Antialiasing::MSAA8X;
            default: return RT64::UserConfiguration::Antialiasing::None;
        }
    }

    bool apply_graphics_settings(frontend::GraphicsSettings requested, bool initial, bool gameplay) {
        const auto normalized = frontend::normalize_graphics(requested, graphics_capabilities());
        const bool resolution_changed = initial || normalized.settings.resolution_multiplier != applied_settings.resolution_multiplier ||
            normalized.settings.auto_resolution != applied_settings.auto_resolution ||
            normalized.settings.downsample_multiplier != applied_settings.downsample_multiplier;
        const bool msaa_changed = initial || normalized.settings.msaa_samples != applied_settings.msaa_samples;
        const bool fullscreen_changed = initial || normalized.settings.fullscreen != applied_settings.fullscreen;
        const bool vsync_changed = initial || normalized.settings.vsync != applied_settings.vsync;
        const bool refresh_changed = initial || normalized.settings.output_rate_mode != applied_settings.output_rate_mode ||
            normalized.settings.custom_output_rate != applied_settings.custom_output_rate;
        const bool presentation_changed = initial || normalized.settings.presentation_mode != applied_settings.presentation_mode;
        const bool requested_widescreen = host.options.frontend && gameplay && tooie::widescreen::latched_enabled();
        const bool requested_pillarbox = requested_widescreen && tooie::features::cutscene_requires_pillarbox();
        const double requested_aspect = requested_pillarbox ? 4.0 / 3.0 : tooie::widescreen::latched_aspect_ratio();
        const bool aspect_changed = initial || requested_widescreen != applied_widescreen ||
            requested_pillarbox != applied_cutscene_pillarbox || requested_aspect != applied_aspect_target;
        if (!resolution_changed && !msaa_changed && !fullscreen_changed && !aspect_changed &&
            !vsync_changed && !refresh_changed && !presentation_changed) {
            frontend_gameplay_graphics_applied.store(gameplay, std::memory_order_release);
            return false;
        }

        if (!initial) sample_guest_cadence(true);

        // Window/display transitions are applied by the sole SDL event owner.
        // The renderer thread only applies RT64 configuration.
        const auto values = frontend::to_rt64_user_values(normalized.settings);
        app->userConfig.resolution = values.manual_resolution ? RT64::UserConfiguration::Resolution::Manual
                                                            : RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app->userConfig.resolutionMultiplier = values.resolution_multiplier;
        app->userConfig.downsampleMultiplier = values.downsample_multiplier;
        app->userConfig.antialiasing = rt64_msaa(values.msaa_samples);
        app->userConfig.aspectRatio = requested_widescreen ? RT64::UserConfiguration::AspectRatio::Manual
                                                           : RT64::UserConfiguration::AspectRatio::Original;
        app->userConfig.aspectTarget = requested_aspect;
        app->userConfig.extAspectRatio = hud_extended_aspect(requested_widescreen && !requested_pillarbox);
        app->userConfig.upscale2D = RT64::UserConfiguration::Upscale2D::Original;
        switch (normalized.settings.output_rate_mode) {
            case frontend::OutputRateMode::Display:
                app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Display;
                app->userConfig.refreshRateTarget = int(normalized.settings.custom_output_rate);
                break;
            case frontend::OutputRateMode::Custom:
                app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Manual;
                app->userConfig.refreshRateTarget = int(normalized.settings.custom_output_rate);
                break;
            default:
                app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
                app->userConfig.refreshRateTarget = int(normalized.settings.custom_output_rate);
                break;
        }
        app->enhancementConfig.presentation.mode = normalized.settings.presentation_mode == frontend::PresentationMode::Early
            ? RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly
            : RT64::EnhancementConfiguration::Presentation::Mode::Console;
        app->enhancementConfig.presentation.removeBlackBorders = false;
        if (vsync_changed) pacing::request_vsync(normalized.settings.vsync);
        app->updateUserConfig(resolution_changed || msaa_changed || aspect_changed);
        if (presentation_changed) app->updateEnhancementConfig();
        if (msaa_changed) app->updateMultisampling();
        applied_settings = normalized.settings;
        applied_widescreen = requested_widescreen;
        applied_cutscene_pillarbox = requested_pillarbox;
        applied_aspect_target = requested_aspect;
        frontend_gameplay_graphics_applied.store(gameplay, std::memory_order_release);
        sample_guest_cadence(false);

        std::ostringstream owner;
        owner << std::this_thread::get_id();
        log("native_graphics_applied", {
            {"owner", "RT64 gfx thread"}, {"owner_thread", owner.str()},
            {"resolution", applied_settings.auto_resolution ? "Auto" : "Manual"}, {"resolution_multiplier", applied_settings.resolution_multiplier},
            {"internal_resolution_multiplier", values.resolution_multiplier},
            {"downsample_multiplier", applied_settings.downsample_multiplier}, {"msaa_samples", applied_settings.msaa_samples},
            {"msaa_cap", max_msaa_samples.load()}, {"sample_positions", sample_positions_supported.load()},
            {"fullscreen", applied_settings.fullscreen}, {"widescreen_profile", applied_widescreen},
            {"aspect_ratio", applied_cutscene_pillarbox ? "manual 4:3 cutscene" : applied_widescreen ? "manual native widescreen" : "original"},
            {"aspect_target", applied_aspect_target},
            {"vsync_requested", applied_settings.vsync},
            {"vsync_actual", pacing::present_snapshot().vsync_actual},
            {"output_rate_mode", uint32_t(applied_settings.output_rate_mode)},
            {"custom_output_rate", applied_settings.custom_output_rate},
            {"presentation", applied_settings.presentation_mode == frontend::PresentationMode::Early ? "early" : "console"},
            {"phase", gameplay ? "gameplay" : "launcher"},
            {"unsupported_persisted_value_fallback", normalized.used_fallback}});
        return true;
    }

    bool apply_requested_graphics(bool initial) {
        return apply_graphics_settings(requested_graphics_settings(), initial, true);
    }

    bool apply_launcher_graphics(bool initial) {
        return apply_graphics_settings(frontend::launcher_graphics_settings(), initial, false);
    }
public:
    NativeRenderer(uint8_t* rdram, ultramodern::renderer::WindowHandle window) {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        persistent_gfx_thread = std::this_thread::get_id();
#ifdef _WIN32
        persistent_window_thread_id = window.thread_id;
#endif
#endif
        require(host.initialized.load() && host.window && rdram, "Renderer requires initialized window and RDRAM");
        require(host.renderer_count.load() == 0, "Only one native renderer is supported");
        setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
#ifdef __APPLE__
        chosen_api = ultramodern::renderer::GraphicsApi::Metal;
#endif
        RT64::Application::Core core{};
#ifdef _WIN32
        core.window = window.window;
#elif defined(__APPLE__)
        core.window.window = window.window;
        core.window.view = window.view;
#else
        core.window = window;
#endif
        core.HEADER = header.data(); // Same unused header placeholder as pinned frontend.
        core.RDRAM = rdram;
        core.DMEM = dmem.data();
        core.IMEM = imem.data();
        core.MI_INTR_REG = &registers[0];
        core.DPC_START_REG = &registers[1];
        core.DPC_END_REG = &registers[2];
        core.DPC_CURRENT_REG = &registers[3];
        core.DPC_STATUS_REG = &registers[4];
        core.DPC_CLOCK_REG = &registers[5];
        core.DPC_BUFBUSY_REG = &registers[6];
        core.DPC_PIPEBUSY_REG = &registers[7];
        core.DPC_TMEM_REG = &registers[8];
        core.checkInterrupts = no_interrupt_callback;
        auto* vi = ultramodern::renderer::get_vi_regs();
#define TOOIE_VI(name) core.name = &vi->name
        TOOIE_VI(VI_STATUS_REG); TOOIE_VI(VI_ORIGIN_REG); TOOIE_VI(VI_WIDTH_REG);
        TOOIE_VI(VI_INTR_REG); TOOIE_VI(VI_V_CURRENT_LINE_REG); TOOIE_VI(VI_TIMING_REG);
        TOOIE_VI(VI_V_SYNC_REG); TOOIE_VI(VI_H_SYNC_REG); TOOIE_VI(VI_LEAP_REG);
        TOOIE_VI(VI_H_START_REG); TOOIE_VI(VI_V_START_REG); TOOIE_VI(VI_V_BURST_REG);
        TOOIE_VI(VI_X_SCALE_REG); TOOIE_VI(VI_Y_SCALE_REG);
#undef TOOIE_VI
        RT64::ApplicationConfiguration config;
        config.appId = host.options.frontend ? "BanjoTooieRecompiled" : "tooie-mission-01";
        config.detectDataPath = false;
        config.dataPath = host.options.data_directory / "rt64";
        config.useConfigurationFile = false;
        app = std::make_unique<RT64::Application>(core, config);
        app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
#ifdef __APPLE__
        app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
#endif
        app->userConfig.resolution = host.options.frontend ? RT64::UserConfiguration::Resolution::Manual
                                                           : RT64::UserConfiguration::Resolution::Original;
        app->userConfig.resolutionMultiplier = 1;
        app->userConfig.downsampleMultiplier = 1;
        const bool widescreen = host.options.frontend && tooie::widescreen::latched_enabled();
        app->userConfig.aspectRatio = widescreen ? RT64::UserConfiguration::AspectRatio::Manual
                                                 : RT64::UserConfiguration::AspectRatio::Original;
        app->userConfig.aspectTarget = tooie::widescreen::latched_aspect_ratio();
        app->userConfig.extAspectRatio = hud_extended_aspect(widescreen);
        app->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
        app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
        app->userConfig.refreshRateTarget = 60;
        app->userConfig.upscale2D = RT64::UserConfiguration::Upscale2D::Original;
        app->userConfig.filtering = RT64::UserConfiguration::Filtering::Nearest;
        app->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
        app->userConfig.idleWorkActive = false;
        app->userConfig.developerMode = false;
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::Console;
        app->enhancementConfig.presentation.removeBlackBorders = false;
        app->enhancementConfig.f3dex.forceBranch = false;
        app->enhancementConfig.textureLOD.scale = false;
        // Keep renderer correctness fixes at their pinned defaults; no game
        // enhancement hooks, texture replacements or synthetic workloads.
#ifdef _WIN32
        const uint32_t thread_id = window.thread_id;
#else
        const uint32_t thread_id = 0;
#endif
        setup_result = map_result(app->setup(thread_id));
        // This pin incorrectly returns Success if dynamic-library load fails.
        if (setup_result == ultramodern::renderer::SetupResult::Success &&
            (!app->device || !app->swapChain || !app->interpreter || !app->state))
            setup_result = ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            log("native_renderer_setup_failed", {{"result", int(setup_result)}});
            app->end();
            app.reset();
            return;
        }
        registered = true;
        const auto& diagnostic = interpolation_diagnostic();
        log("native_model_interpolation_policy",
            {{"selected", interpolation_diagnostic_name(diagnostic.mode)},
             {"diagnostic_override", diagnostic.requested},
             {"cpu_skinned_pose", "task_scoped_original"}});
        if (diagnostic.requested) {
            log("native_interpolation_diagnostic_selected",
                {{"mode", interpolation_diagnostic_name(diagnostic.mode)},
                 {"valid_value", diagnostic.valid},
                 {"display_cadence_unchanged", true}});
        }
        tooie::features::set_cutscene_pillarbox_available(host.options.frontend);
        host.renderer_count.fetch_add(1);
        renderer_ready.store(true, std::memory_order_release);
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        persistent_active_renderer.store(this, std::memory_order_release);
#endif
        unsigned detected_max = 0;
        bool detected_positions = false;
        if (app->device->getCapabilities().sampleLocations) {
            const auto color = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
            const auto depth = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
            const auto common = color & depth;
            detected_positions = true;
            if (common & plume::RenderSampleCount::Bits::COUNT_2) detected_max = 2;
            if (common & plume::RenderSampleCount::Bits::COUNT_4) detected_max = 4;
            if (common & plume::RenderSampleCount::Bits::COUNT_8) detected_max = 8;
        }
        sample_positions_supported.store(detected_positions);
        max_msaa_samples.store(detected_max);
        if (host.options.frontend) {
            apply_launcher_graphics(true);
        }
        log("native_renderer_ready", {{"backend", app->userConfig.graphicsAPI == RT64::UserConfiguration::GraphicsAPI::Metal ? "Metal" : "Vulkan"}, {"device", app->device->getDescription().name},
            {"resolution_scale", 1}, {"aspect_ratio", widescreen ? "manual native widescreen" : "original"},
            {"aspect_target", app->userConfig.aspectTarget},
            {"widescreen_profile", widescreen}, {"refresh_rate", "original"},
            {"presentation", "console"}, {"force_branch", false}, {"texture_lod_scale", false},
            {"game_frame_verified", false}});
    }
    ~NativeRenderer() override { shutdown(); }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    RT64::Application* persistent_application() const noexcept { return app.get(); }
    bool persistent_recreate(const std::vector<uint8_t>& image) noexcept {
        if (std::this_thread::get_id() != persistent_gfx_thread || !valid() || image.empty())
            return false;
        try {
            const auto core = app->core;
            const auto config = app->appConfig;
            const auto user = app->userConfig;
            const auto emulator = app->emulatorConfig;
            const auto enhancement = app->enhancementConfig;
            app->end();
            app.reset();
            renderer_ready.store(false, std::memory_order_release);
            auto fresh = std::make_unique<RT64::Application>(core, config);
            fresh->userConfig = user;
            fresh->emulatorConfig = emulator;
            fresh->enhancementConfig = enhancement;
            const auto result = map_result(fresh->setup(persistent_window_thread_id));
            if (result != ultramodern::renderer::SetupResult::Success ||
                !fresh->device || !fresh->swapChain || !fresh->interpreter || !fresh->state) {
                fresh->end();
                setup_result = result;
                return false;
            }
            if (!persistent_bridge::import_image(*fresh->state, image)) {
                fresh->end();
                return false;
            }
            app = std::move(fresh);
            last_ucode_text_hash = 0;
            last_ucode_data_hash = 0;
            // Host task/VI counters remain monotonic and equal at quiesce;
            // only the cadence diagnostic baseline crosses an epoch here.
            cadence_active = false;
            renderer_ready.store(true, std::memory_order_release);
            return true;
        } catch (...) {
            renderer_ready.store(false, std::memory_order_release);
            return false;
        }
    }
#endif
    bool valid() override { return app != nullptr && registered; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
        if (host.options.frontend) {
            return frontend_gameplay_active.load(std::memory_order_acquire)
                ? apply_requested_graphics(false)
                : apply_launcher_graphics(false);
        }
        log("native_graphics_config_fixed", {{"vanilla_settings_retained", true}});
        return false;
    }
    void enable_instant_present() override {
        log("native_instant_present_ignored", {{"presentation", "console"}});
    }
    void send_dummy_workload(uint32_t fb_address) override {
        if (!host.options.frontend) {
            if (!dummy_logged) {
                log("native_launcher_workload_suppressed", {{"game_task", false}, {"rendered", false}});
                dummy_logged = true;
            }
            return;
        }
        app->state->listProcessBegin();
        app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
        app->state->rdp->setOtherMode(0x382C30, 0);
        app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
        app->state->fullSync();
        app->state->listProcessEnd();
    }
    void send_dl(const OSTask* task) override {
        require(valid() && task, "Invalid native renderer or null graphics task");
        if (host.options.frontend && frontend_gameplay_active.load(std::memory_order_acquire) &&
            applied_cutscene_pillarbox != tooie::features::cutscene_requires_pillarbox()) {
            apply_requested_graphics(false);
        }
        require(task->t.type == 1, "Non-graphics OSTask submitted to RT64");
        const uint32_t text = uint32_t(task->t.ucode) & 0x3ffffff;
        const uint32_t data = uint32_t(task->t.ucode_data) & 0x3ffffff;
        const uint32_t list = uint32_t(task->t.data_ptr) & 0x3ffffff;
        const auto model_ranges = tooie::model_interpolation::consume_task_ranges(list);
        const tooie::model_interpolation::TaskScope model_scope(model_ranges);
        check_range(text, gbi_text_scan_bytes);
        check_range(data, gbi_data_scan_bytes);
        check_range(list, task->t.data_size);
        require(task->t.ucode_size != 0 && task->t.ucode_data_size != 0 && task->t.data_size != 0,
                "Empty graphics task input");
        require((list & 7) == 0, "Unaligned graphics display list");
        const auto sequence = host.submitted.fetch_add(1) + 1;
        const bool log_success_detail = !host.options.frontend || sequence == 1;
        Json detail{{"graphics_task_sequence", sequence}, {"type", task->t.type}, {"flags", task->t.flags},
            {"ucode", text}, {"ucode_size", task->t.ucode_size}, {"ucode_data", data},
            {"ucode_data_size", task->t.ucode_data_size}, {"display_list", list}, {"display_list_size", task->t.data_size}};
        if (log_success_detail) {
            detail["hash_algorithm"] = "XXH3_64 canonical guest bytes";
            detail["ucode_hash"] = hash_guest_bytes(app->core.RDRAM, text, task->t.ucode_size);
            detail["ucode_data_hash"] = hash_guest_bytes(app->core.RDRAM, data, task->t.ucode_data_size);
            detail["display_list_hash"] = hash_guest_bytes(app->core.RDRAM, list, task->t.data_size);
            log("native_graphics_task_submitted", detail);
        }
        const auto code_hash = XXH3_64bits(app->core.RDRAM + text, gbi_text_scan_bytes);
        const auto data_hash = XXH3_64bits(app->core.RDRAM + data, gbi_data_scan_bytes);
        // Frontend caches only addresses. Original code can reuse those bytes;
        // invalidate the lookup if the resident microcode changes at one address.
        if (code_hash != last_ucode_text_hash || data_hash != last_ucode_data_hash) {
            app->interpreter->UCode.textAddress = UINT32_MAX;
            app->interpreter->UCode.dataAddress = UINT32_MAX;
            last_ucode_text_hash = code_hash;
            last_ucode_data_hash = data_hash;
        }
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(text, data, true);
        auto* gbi = app->interpreter->hleGBI;
        if (!gbi || gbi->ucode == RT64::GBIUCode::Unknown) {
            log("native_graphics_microcode_unsupported", detail);
            throw std::runtime_error("Pinned RT64 did not recognize the submitted microcode");
        }
        detail["gbi"] = uint32_t(gbi->ucode);
        detail["non"] = gbi->flags.NoN;
        if (log_success_detail) log("native_graphics_microcode_recognized", detail);
        const bool visibility_active = tooie::visibility::task_projection_override_active();
        const auto task_metadata = tooie::camera_interpolation::consume_task_metadata(list);
        const auto interpolation = task_metadata.interpolation;
        const auto& diagnostic = interpolation_diagnostic();
        const bool diagnostic_camera_off = diagnostic.mode == InterpolationDiagnosticMode::Off ||
            diagnostic.mode == InterpolationDiagnosticMode::ModelOnly;
        const bool diagnostic_model_off = diagnostic.mode == InterpolationDiagnosticMode::Off ||
            diagnostic.mode == InterpolationDiagnosticMode::CameraOnly;
        const bool skip_camera_interpolation =
            interpolation != tooie::camera_interpolation::TaskInterpolation::Interpolated || diagnostic_camera_off;
        const bool original_cutscene_motion = interpolation == tooie::camera_interpolation::TaskInterpolation::OriginalMotion;
        const bool skip_model_interpolation = original_cutscene_motion || diagnostic_model_off;
        if (visibility_active || skip_camera_interpolation) {
            // The per-task reset above clears RSP state. Preserve native projection
            // TransformGroup directly to the renderer, rather than encoding a
            // guest opcode or mutating the guest display list. The tuple is
            // RT64's normal automatic group with aspect set to Stretch.
            const uint8_t component = skip_camera_interpolation ? 0 : 2;
            app->state->rsp->matrixId(
                skip_camera_interpolation ? 0u : 0xFFFFFFFFu, false, true, true,
                component, component, component, component, component, 0, 0, component, component,
                1, visibility_active ? 1 : 0, 0, false, false);
            if (visibility_active) tooie::visibility::record_task_applied();
        }
        if (skip_model_interpolation) {
            // A projection-only reset still allows RT64 to match ordinary
            // world/model transforms. OriginalMotion or the diagnostic mode
            // ignores model matching and vertex/texture-coordinate interpolation.
            // RSP::reset above restores automatic groups for the next task.
            app->state->rsp->matrixId(
                0u, false, false, true,
                0, 0, 0, 0, 0, 0, 0, 0, 0,
                1, 0, 0, false, false);
        }
        if (!skip_model_interpolation &&
            (diagnostic.mode == InterpolationDiagnosticMode::ModelLinear ||
             diagnostic.mode == InterpolationDiagnosticMode::ModelOnly)) {
            // Tooie's bones share vertices across matrix slots. Blend complete
            // matrices at one weight: independently decomposing or skipping
            // components splits those connected edges between guest frames.
            app->state->rsp->matrixId(
                0xFFFFFFFFu, false, false, false,
                1, 1, 1, 1, 1, 0, 0, 2, 2,
                1, 0, 0, false, false);
        }
        if (diagnostic.requested && sequence <= 8) {
            log("native_interpolation_diagnostic_task",
                {{"graphics_task_sequence", sequence}, {"display_list", list},
                 {"task_interpolation", uint32_t(interpolation)},
                 {"camera_disabled", skip_camera_interpolation},
                 {"model_disabled", skip_model_interpolation},
                 {"visibility_aspect_override", visibility_active}});
        }
        // Same real HLE display-list path as pinned frontend. It may follow
        // nested lists; the root OSTask byte count is not a full graph bound.
        const bool capture_artifact = tooie::artifact_capture::try_claim();
        // Queue state is capture-only work; ordinary tasks must retain the
        // atomic request check as their sole diagnostic overhead.
        const uint32_t workload_cursor_before = capture_artifact ?
            app->state->ext.workloadQueue->writeCursor : 0u;
        // BK supplies each recorded demo's input rate through gEXSetRefreshRate.
        // Use RT64's equivalent setter with metadata bound to this exact task.
        // Clear only our rate hint on ordinary/mismatched tasks; this prevents a
        // no-full-sync task from leaking the optional hint into a later frame.
        app->state->setRefreshRate(task_metadata.replay_refresh_rate != 0
            ? task_metadata.replay_refresh_rate : std::numeric_limits<uint16_t>::max());
        std::unique_ptr<tooie::graphics_branch_capture::Scope> branch_capture;
        if (capture_artifact) {
            try {
                branch_capture = std::make_unique<tooie::graphics_branch_capture::Scope>(*app->interpreter);
            } catch (...) {
                // Optional diagnostics cannot prevent the real display list.
            }
        }
        const auto process_start = std::chrono::steady_clock::now();
        app->processDisplayLists(app->core.RDRAM, list, 0, true);
        if (branch_capture) branch_capture->stop();
        pacing::record_render_work(pacing::RenderWorkPhase::ProcessDisplayLists,
            std::chrono::steady_clock::now() - process_start);
        // RT64 logs this profiler before advanceToNextWorkload(), whose queue
        // barrier can wait on renderer/GPU consumption. Preserve both scopes.
        const auto& dl_cpu = app->state->dlCpuProfiler;
        double dl_cpu_last = 0.0;
        double dl_cpu_recent_total = 0.0;
        std::size_t dl_cpu_recent_samples = 0;
        if (dl_cpu.size() != 0) {
            dl_cpu_last = dl_cpu.data()[(dl_cpu.index() + dl_cpu.size() - 1) % dl_cpu.size()];
            // ProfilingTimer::average() includes its zero-filled cold-start
            // slots. Average only values RT64 has actually logged.
            for (std::size_t i = 0; i < dl_cpu.size(); ++i) {
                if (dl_cpu.data()[i] > 0.0) {
                    dl_cpu_recent_total += dl_cpu.data()[i];
                    ++dl_cpu_recent_samples;
                }
            }
        }
        pacing::record_display_list_cpu(dl_cpu_last,
            dl_cpu_recent_samples != 0 ? dl_cpu_recent_total / dl_cpu_recent_samples : 0.0);
        if (capture_artifact) {
            if (branch_capture) {
                try {
                    auto branches = branch_capture->snapshot_json();
                    branches["graphics_task_sequence"] = sequence;
                    branches["force_branch"] = app->enhancementConfig.f3dex.forceBranch;
                    log("artifact_branch_capture", std::move(branches));
                } catch (...) {
                    // A failed branch report must not suppress the draw capture.
                }
            }
            // This diagnostic must be fully contained: F4 cannot alter the
            // renderer's result when snapshot allocation, hashing, or logging fails.
            try {
                const auto scene = tooie::scene::snapshot();
                const auto free_camera = tooie::camera::free_camera_debug_snapshot();
                tooie::artifact_capture::capture_after_display_list(*app->state, workload_cursor_before,
                    app->state->ext.workloadQueue->writeCursor,
                    {.sequence = sequence,
                     .display_list = list,
                     .display_list_size = task->t.data_size,
                     // This is deliberately computed after processDisplayLists;
                     // it identifies the root OSTask byte range at capture time.
                     .display_list_hash = hash_guest_bytes(app->core.RDRAM, list, task->t.data_size),
                     .map_id = scene.map_available ? uint32_t(scene.map_id) : 0u,
                     .gbi = uint32_t(gbi->ucode),
                     .projection_override = visibility_active,
                     .camera_interpolated = !skip_camera_interpolation,
                     .original_cutscene_motion = original_cutscene_motion,
                     .free_camera_enabled = free_camera.enabled,
                     .free_camera_input_active = free_camera.input_active,
                     .free_camera_offset = free_camera.offset},
                    [](const char* event, Json packet) { log(event, std::move(packet)); });
            } catch (...) {
                try {
                    log("artifact_draw_capture_failure", {{"graphics_task_sequence", sequence},
                        {"phase", "post_process_metadata_or_capture"}});
                } catch (...) {
                }
            }
        }
        host.parsed.fetch_add(1);
        if (log_success_detail)
            log("native_graphics_task_parsed", {{"graphics_task_sequence", sequence}, {"presented_frame_claim", false}});
    }
    void update_screen() override {
        require(valid(), "VI update without a valid native renderer");
        const auto sequence = host.vi_updates.fetch_add(1) + 1;
        const auto* vi = ultramodern::renderer::get_vi_regs();
        const auto update_start = std::chrono::steady_clock::now();
        app->updateScreen();
        pacing::record_render_work(pacing::RenderWorkPhase::UpdateScreen,
            std::chrono::steady_clock::now() - update_start);
        sample_guest_cadence(false);
        actual_presentation.store(uint32_t(applied_settings.presentation_mode), std::memory_order_release);
        actual_requested_target_hz.store(applied_settings.output_rate_mode == frontend::OutputRateMode::Custom
            ? applied_settings.custom_output_rate : (applied_settings.output_rate_mode == frontend::OutputRateMode::Display
                ? host.display_rate.load(std::memory_order_acquire) : 0), std::memory_order_release);
        {
            std::scoped_lock lock(app->sharedQueueResources->configurationMutex);
            actual_swapchain_hz.store(app->sharedQueueResources->swapChainRate, std::memory_order_release);
            actual_effective_target_hz.store(app->sharedQueueResources->targetRate, std::memory_order_release);
        }
        if (sequence <= 8 || sequence % 60 == 0)
            log("native_vi_submitted", {{"vi_sequence", sequence}, {"origin", vi->VI_ORIGIN_REG},
                {"width", vi->VI_WIDTH_REG}, {"status", vi->VI_STATUS_REG}, {"h_start", vi->VI_H_START_REG},
                {"v_start", vi->VI_V_START_REG}, {"graphics_tasks_parsed", host.parsed.load()},
                {"presented_frame_claim", false}});
    }
    void shutdown() override {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        if (persistent_active_renderer.load(std::memory_order_acquire) == this)
            persistent_active_renderer.store(nullptr, std::memory_order_release);
#endif
        tooie::features::set_cutscene_pillarbox_available(false);
        sample_guest_cadence(true);
        renderer_ready.store(false, std::memory_order_release);
        if (app) { app->end(); app.reset(); }
        if (registered) {
            registered = false;
            host.renderer_count.fetch_sub(1);
            log("native_renderer_shutdown", {{"rt64_end_completed", true}});
        }
    }
    uint32_t get_display_framerate() const override { return host.display_rate.load(std::memory_order_acquire); }
    float get_resolution_scale() const override {
        if (app->userConfig.resolution == RT64::UserConfiguration::Resolution::WindowIntegerScale) {
            constexpr int reference_height = 240;
            return std::max(float((app->sharedQueueResources->swapChainHeight + reference_height - 1) / reference_height), 1.0f);
        }
        return float(app->userConfig.resolutionMultiplier);
    }
};

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_recreate_callback(ultramodern::renderer::RendererContext* context,
    uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_renderer_restore_epoch_active.load(std::memory_order_acquire) != epoch ||
        context != persistent_active_renderer.load(std::memory_order_acquire)) return false;
    std::shared_ptr<const std::vector<uint8_t>> image;
    {
        std::lock_guard guard(persistent_renderer_restore_mutex);
        image = persistent_renderer_restore_blob;
    }
    if (!image || persistent_renderer_restore_epoch_active.load(std::memory_order_acquire) != epoch)
        return false;
    return static_cast<NativeRenderer*>(context)->persistent_recreate(*image);
}
#endif

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool) {
    return std::make_unique<NativeRenderer>(rdram, window);
}

std::pair<int, int> output_resolution(frontend::OutputResolutionId id, const SDL_DisplayMode& desktop) {
    switch (id) {
        case frontend::OutputResolutionId::DefaultWindowed: return {960, 720};
        case frontend::OutputResolutionId::R1280x720: return {1280, 720};
        case frontend::OutputResolutionId::R1920x1080: return {1920, 1080};
        case frontend::OutputResolutionId::R2560x1440: return {2560, 1440};
        case frontend::OutputResolutionId::R3440x1440: return {3440, 1440};
        case frontend::OutputResolutionId::R3840x2160: return {3840, 2160};
        default: return {desktop.w, desktop.h};
    }
}

frontend::OutputDisplayId display_id_from_index(int index) {
    return index >= 0 && index < 4 ? static_cast<frontend::OutputDisplayId>(index + 1)
                                  : frontend::OutputDisplayId::Current;
}

const char* required_display_name() noexcept {
    return std::getenv("TOOIE_REQUIRED_DISPLAY_NAME");
}

void verify_required_display(int requested_index, const char* phase) {
    const char* required = required_display_name();
    if (!required) return;
    const int count = SDL_GetNumVideoDisplays();
    const char* requested_name = requested_index >= 0 && requested_index < count
        ? SDL_GetDisplayName(requested_index) : nullptr;
    const int actual_index = host.window ? SDL_GetWindowDisplayIndex(host.window) : -1;
    const char* actual_name = actual_index >= 0 && actual_index < count
        ? SDL_GetDisplayName(actual_index) : nullptr;
    SDL_Rect bounds{};
    int x = 0, y = 0, width = 0, height = 0;
    const bool have_bounds = requested_index >= 0 && requested_index < count &&
        SDL_GetDisplayBounds(requested_index, &bounds) == 0;
    if (host.window) {
        SDL_GetWindowPosition(host.window, &x, &y);
        SDL_GetWindowSize(host.window, &width, &height);
    }
    const bool inside = have_bounds && x >= bounds.x && y >= bounds.y && width > 0 && height > 0 &&
        int64_t(x) + width <= int64_t(bounds.x) + bounds.w &&
        int64_t(y) + height <= int64_t(bounds.y) + bounds.h;
    const bool matched = *required && have_bounds && bounds.w > 0 && bounds.h > 0 &&
        requested_name && std::string_view(requested_name) == required &&
        (!host.window || (actual_index == requested_index && actual_name &&
            std::string_view(actual_name) == required && inside));
    log("native_required_display_check", {{"phase", phase}, {"required_name", required},
        {"display_count", count}, {"requested_index", requested_index},
        {"requested_name", requested_name ? requested_name : ""}, {"actual_index", actual_index},
        {"actual_name", actual_name ? actual_name : ""}, {"window_x", x}, {"window_y", y},
        {"window_width", width}, {"window_height", height}, {"bounds_x", bounds.x},
        {"bounds_y", bounds.y}, {"bounds_width", bounds.w}, {"bounds_height", bounds.h},
        {"matched", matched}});
    if (!matched) throw std::runtime_error(std::string("Required display unavailable or window misplaced at ") + phase);
}

void show_verified_required_display(int requested_index, const char* phase) {
    SDL_ShowWindow(host.window);
    try {
        verify_required_display(requested_index, phase);
    } catch (...) {
        SDL_HideWindow(host.window);
        throw;
    }
}

void refresh_display_capabilities() {
    DisplayCapabilities snapshot;
    const int current_index = host.window ? SDL_GetWindowDisplayIndex(host.window) : 0;
    snapshot.current_display = display_id_from_index(current_index);
    const int count = std::max(0, SDL_GetNumVideoDisplays());
    for (int display_index = 0; display_index < std::min(count, 4); ++display_index) {
        DisplayInfo display;
        display.id = display_id_from_index(display_index);
        const char* name = SDL_GetDisplayName(display_index);
        display.name = name ? name : ("Display " + std::to_string(display_index + 1));
        SDL_DisplayMode desktop{};
        if (SDL_GetDesktopDisplayMode(display_index, &desktop) == 0) {
            display.desktop_width = uint32_t(std::max(0, desktop.w));
            display.desktop_height = uint32_t(std::max(0, desktop.h));
            display.desktop_refresh_hz = uint32_t(std::max(0, desktop.refresh_rate));
        }
        const int mode_count = std::max(0, SDL_GetNumDisplayModes(display_index));
        for (int mode_index = 0; mode_index < mode_count; ++mode_index) {
            SDL_DisplayMode mode{};
            if (SDL_GetDisplayMode(display_index, mode_index, &mode) != 0 || mode.w <= 0 || mode.h <= 0) continue;
            DisplayModeInfo candidate{uint32_t(mode.w), uint32_t(mode.h), uint32_t(std::max(0, mode.refresh_rate))};
            const auto duplicate = std::find_if(display.modes.begin(), display.modes.end(), [&](const auto& existing) {
                return existing.width == candidate.width && existing.height == candidate.height &&
                    existing.refresh_hz == candidate.refresh_hz;
            });
            if (duplicate == display.modes.end()) display.modes.push_back(candidate);
        }
        snapshot.displays.push_back(std::move(display));
    }
    std::lock_guard guard(display_state_mutex);
    current_display_capabilities = std::move(snapshot);
}

void apply_launcher_display() {
    if (!host.window) return;
    const int count = std::max(0, SDL_GetNumVideoDisplays());
    int display_index = std::max(0, SDL_GetWindowDisplayIndex(host.window));
    if (display_index >= count) display_index = 0;
    SDL_DisplayMode desktop{};
    const bool have_desktop = count > 0 && SDL_GetDesktopDisplayMode(display_index, &desktop) == 0;
    bool success = SDL_SetWindowFullscreen(host.window, 0) == 0;
    SDL_SetWindowDisplayMode(host.window, nullptr);
    SDL_SetWindowBordered(host.window, SDL_TRUE);
    int width = host.options.width;
    int height = host.options.height;
    SDL_Rect usable{};
    if (have_desktop && SDL_GetDisplayUsableBounds(display_index, &usable) == 0) {
        const double scale = std::min({1.0, double(usable.w) / width, double(usable.h) / height});
        width = std::max(1, int(std::lround(width * scale)));
        height = std::max(1, int(std::lround(height * scale)));
        SDL_SetWindowSize(host.window, width, height);
        SDL_SetWindowPosition(host.window, usable.x + (usable.w - width) / 2,
            usable.y + (usable.h - height) / 2);
    }
    else {
        SDL_SetWindowSize(host.window, width, height);
        SDL_SetWindowPosition(host.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
    }
    if (have_desktop && desktop.refresh_rate > 0)
        host.display_rate.store(uint32_t(desktop.refresh_rate), std::memory_order_release);
    {
        std::lock_guard guard(display_state_mutex);
        current_display_readout.output_mode = frontend::OutputMode::Windowed;
        current_display_readout.effective_display = display_id_from_index(display_index);
        const char* name = count > 0 ? SDL_GetDisplayName(display_index) : nullptr;
        current_display_readout.effective_display_name = name ? name : "";
        current_display_readout.output_width = uint32_t(width);
        current_display_readout.output_height = uint32_t(height);
        current_display_readout.display_fallback = !success;
    }
    refresh_display_capabilities();
    log("native_launcher_display_applied", {{"owner", "SDL/UI thread"},
        {"output_mode", "windowed"}, {"width", width}, {"height", height},
        {"centered", true}, {"saved_gameplay_display_deferred", true}, {"success", success}});
}

void apply_requested_display(bool force = false) {
    if (!host.window) return;
    const auto generation = requested_display_generation.load(std::memory_order_acquire);
    if (!force && generation == applied_display_generation) return;
    const auto normalized = frontend::normalize_display(requested_display_settings());
    const auto requested = normalized.settings;
    const int current_index = std::max(0, SDL_GetWindowDisplayIndex(host.window));
    const int count = std::max(0, SDL_GetNumVideoDisplays());
    int display_index = requested.display == frontend::OutputDisplayId::Current
        ? current_index : int(requested.display) - 1;
    if (required_display_name()) {
        verify_required_display(display_index, "gameplay preflight");
        SDL_HideWindow(host.window);
    }
    bool fallback = normalized.used_fallback || display_index < 0 || display_index >= count;
    if (fallback) display_index = current_index < count ? current_index : 0;
    SDL_DisplayMode desktop{};
    if (count == 0 || SDL_GetDesktopDisplayMode(display_index, &desktop) != 0) {
        log("native_display_apply_failed", {{"reason", SDL_GetError()}, {"display_index", display_index}});
        if (required_display_name()) throw std::runtime_error("Required display desktop mode unavailable");
        applied_display_generation = generation;
        return;
    }
    const auto [requested_width, requested_height] = output_resolution(requested.resolution, desktop);
    SDL_DisplayMode exclusive = desktop;
    if (requested.mode == frontend::OutputMode::ExclusiveFullscreen) {
        bool found = false;
        int best_distance = std::numeric_limits<int>::max();
        const int mode_count = std::max(0, SDL_GetNumDisplayModes(display_index));
        for (int i = 0; i < mode_count; ++i) {
            SDL_DisplayMode candidate{};
            if (SDL_GetDisplayMode(display_index, i, &candidate) != 0 || candidate.w != requested_width || candidate.h != requested_height) continue;
            const int distance = std::abs(candidate.refresh_rate - desktop.refresh_rate);
            if (!found || distance < best_distance) { exclusive = candidate; best_distance = distance; found = true; }
        }
        if (!found) fallback = true;
    }

    bool success = SDL_SetWindowFullscreen(host.window, 0) == 0;
    frontend::OutputMode effective_mode = requested.mode;
    int effective_width = requested_width, effective_height = requested_height;
    uint32_t effective_rate = uint32_t(std::max(0, desktop.refresh_rate));
    if (success && requested.mode == frontend::OutputMode::Windowed) {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(display_index, &usable) == 0) {
            const double scale = std::min({1.0, double(usable.w) / requested_width, double(usable.h) / requested_height});
            effective_width = std::max(1, int(std::lround(requested_width * scale)));
            effective_height = std::max(1, int(std::lround(requested_height * scale)));
            SDL_SetWindowBordered(host.window, SDL_TRUE);
            SDL_SetWindowSize(host.window, effective_width, effective_height);
            SDL_SetWindowPosition(host.window, usable.x + (usable.w - effective_width) / 2,
                usable.y + (usable.h - effective_height) / 2);
        }
        else {
            SDL_SetWindowBordered(host.window, SDL_TRUE);
            SDL_SetWindowSize(host.window, effective_width, effective_height);
            SDL_SetWindowPosition(host.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
                SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
        }
    }
    else if (success && requested.mode == frontend::OutputMode::BorderlessDesktop) {
        SDL_SetWindowPosition(host.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
        SDL_SetWindowDisplayMode(host.window, nullptr);
        success = SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
        effective_width = desktop.w; effective_height = desktop.h;
    }
    else if (success) {
        SDL_SetWindowPosition(host.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
            SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
        success = SDL_SetWindowDisplayMode(host.window, &exclusive) == 0 &&
            SDL_SetWindowFullscreen(host.window, SDL_WINDOW_FULLSCREEN) == 0;
        effective_width = exclusive.w; effective_height = exclusive.h;
        effective_rate = uint32_t(std::max(0, exclusive.refresh_rate));
    }
    if (!success) {
        const std::string error = SDL_GetError();
        SDL_SetWindowFullscreen(host.window, 0);
        SDL_SetWindowDisplayMode(host.window, nullptr);
        SDL_SetWindowBordered(host.window, SDL_TRUE);
        SDL_SetWindowSize(host.window, host.options.width, host.options.height);
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(display_index, &usable) == 0) {
            SDL_SetWindowPosition(host.window, usable.x + (usable.w - host.options.width) / 2,
                usable.y + (usable.h - host.options.height) / 2);
        }
        else {
            SDL_SetWindowPosition(host.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
                SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
        }
        effective_mode = frontend::OutputMode::Windowed;
        effective_width = host.options.width; effective_height = host.options.height;
        fallback = true;
        log("native_display_apply_failed", {{"reason", error}, {"fallback", "windowed"}});
    }
    if (required_display_name()) {
        if (fallback) throw std::runtime_error("Required display application fell back");
        verify_required_display(display_index, "gameplay placement");
        show_verified_required_display(display_index, "gameplay shown");
    }
    if (effective_rate > 0) host.display_rate.store(effective_rate, std::memory_order_release);
    {
        std::lock_guard guard(display_state_mutex);
        current_display_readout.output_mode = effective_mode;
        current_display_readout.effective_display = display_id_from_index(display_index);
        const char* name = SDL_GetDisplayName(display_index);
        current_display_readout.effective_display_name = name ? name : "";
        current_display_readout.output_width = uint32_t(std::max(0, effective_width));
        current_display_readout.output_height = uint32_t(std::max(0, effective_height));
        current_display_readout.display_fallback = fallback;
    }
    applied_display_generation = generation;
    refresh_display_capabilities();
    log("native_display_applied", {{"owner", "SDL/UI thread"}, {"display_index", display_index},
        {"display_name", SDL_GetDisplayName(display_index) ? SDL_GetDisplayName(display_index) : ""},
        {"width", effective_width}, {"height", effective_height}, {"refresh_hz", effective_rate},
        {"output_mode", uint32_t(effective_mode)}, {"fallback", fallback}, {"fit", "preserve game aspect"}});
}
void* gfx_create() {
    require(host.initialized.load(), "Call native_host::initialize before gfx callbacks");
    return &host;
}
ultramodern::renderer::WindowHandle gfx_window(void*) { return create_window(); }
void gfx_update(void*) { poll_events(); }
} // namespace

void initialize(const Options& options) {
    require(!host.initialized.load(), "Native host already initialized");
    require(!options.data_directory.empty(), "Native host requires an isolated data directory");
    require(options.width > 0 && options.height > 0, "Invalid native window dimensions");
    require(std::isfinite(options.max_capture_seconds) && options.max_capture_seconds >= 0 && options.max_capture_seconds <= 3600,
            "Audio capture duration must be finite and within 0..3600 seconds");
    host.options = options;
    host.options.data_directory = std::filesystem::absolute(options.data_directory);
    std::filesystem::create_directories(host.options.data_directory);
    host.capture.reset();
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    persistent_audio_ledger.clear();
    persistent_audio_ledger_valid = true;
    persistent_audio_lease_epoch.store(0, std::memory_order_release);
#endif
    if (!options.audio_capture_directory.empty() && options.max_capture_seconds > 0) {
        require(options.audio, "Audio capture requires enabled audio");
        auto capture = std::make_unique<AudioCapture>();
        capture->directory = std::filesystem::absolute(options.audio_capture_directory);
        require(!std::filesystem::exists(capture->directory) || std::filesystem::is_empty(capture->directory),
                "Audio capture directory must be new or empty");
        std::filesystem::create_directories(capture->directory);
        capture->limit_ns = uint64_t(std::llround(options.max_capture_seconds * 1e9));
        host.capture = std::move(capture);
    }
    host.main_thread = std::this_thread::get_id();
    host.close.store(false);
    host.keyboard.store(0);
    host.display_rate.store(60, std::memory_order_release);
    frontend_physical_controller.store(false, std::memory_order_release);
    frontend_rumble_capable.store(false, std::memory_order_release);
    frontend_presence_log_state.store(-1, std::memory_order_release);
    host.submitted.store(0); host.parsed.store(0); host.vi_updates.store(0); host.audio_frames.store(0);
    audio_queue_sample.store(0, std::memory_order_release);
    empty_audio_queue_queries.store(0, std::memory_order_relaxed);
    audio_rebuffering_after_fast_forward = false;
    host.logged_main_volume.store(-1);
    save_progress::reset();
    host.save_status = save_progress::Status::Idle;
    renderer_ready.store(false, std::memory_order_release);
    frontend_gameplay_active.store(false, std::memory_order_release);
    frontend_gameplay_requested.store(false, std::memory_order_release);
    frontend_gameplay_graphics_applied.store(false, std::memory_order_release);
    actual_swapchain_hz.store(0, std::memory_order_release);
    actual_requested_target_hz.store(0, std::memory_order_release);
    actual_effective_target_hz.store(0, std::memory_order_release);
    actual_presentation.store(uint32_t(frontend::PresentationMode::Console), std::memory_order_release);
    applied_display_generation = 0;
    pacing::reset_present_probe();
    pacing::reset_guest_update_observer();
    pacing::reset_game_delta_observer();
    measured_guest_update_hz.store(0.0, std::memory_order_release);
    measured_host_vi_hz.store(0.0, std::memory_order_release);
    measured_graphics_task_hz.store(0.0, std::memory_order_release);
    measured_guest_wait_bypassed.store(0, std::memory_order_release);
    measured_cadence_seconds.store(0.0, std::memory_order_release);
    require(options.sdl_observation_directory.empty()||options.audio,"SDL observation requires enabled audio");
    if(!options.sdl_observation_directory.empty()) {
#ifdef _WIN32
    host.sdl_observation.begin(options.sdl_observation_directory,sdl_observation::imported_anchor(),
        platform::executable_path().parent_path()/"SDL2.dll",TOOIE_SDL_BUILD_SHA256);
#else
    host.sdl_observation.begin(options.sdl_observation_directory,nullptr,{},{});
#endif
    }
    try {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS | (options.audio ? SDL_INIT_AUDIO : 0);
    if (options.frontend) sdl_flags |= SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
    sdl_require(SDL_Init(sdl_flags) == 0, "SDL_Init");
    if (options.frontend) {
        const auto controller_db = platform::resource_directory() / "recompcontrollerdb.txt";
        const int mappings = SDL_GameControllerAddMappingsFromFile(controller_db.string().c_str());
        log("frontend_controller_mappings", {{"path", controller_db.string()}, {"loaded", mappings},
            {"success", mappings >= 0}, {"error", mappings < 0 ? SDL_GetError() : ""}});
    }
    host.initialized.store(true);
    audio_pacing::initialize(options.audio_pacing_directory, options.audio_pacing_capacity);
    log("native_host_initialized", {{"video_driver", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : ""},
        {"keyboard_virtual_controller", options.keyboard_controller}, {"physical_controller_support", options.frontend},
        {"accessory", options.frontend ? "rumble when supported" : "none"}, {"audio_enabled", options.audio}});
    } catch(...) {
        auto error=std::current_exception();
        if(host.initialized.load())try{shutdown();}catch(const std::exception& e){std::fprintf(stderr,"Native initialization cleanup: %s\n",e.what());}
        else {SDL_Quit();try{host.sdl_observation.finish();}catch(const std::exception& e){std::fprintf(stderr,"SDL observation initialization cleanup: %s\n",e.what());}}
        std::rethrow_exception(error);
    }
}
ultramodern::renderer::WindowHandle create_window() {
    require_main();
    if (!host.window) {
        const bool guarded = required_display_name() != nullptr;
        Uint32 flags = (guarded ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN) | SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#elif !defined(_WIN32)
        flags |= SDL_WINDOW_VULKAN;
#endif
        // Honor an explicit saved monitor at creation, before showing the launcher.
        // Window size/mode still remain launcher-owned until Start Game.
        const auto launch_display = requested_display_settings().display;
        int launch_index = launch_display == frontend::OutputDisplayId::Current
            ? 0 : static_cast<int>(launch_display) - 1;
        if (guarded) verify_required_display(launch_index, "launcher preflight");
        if (launch_index < 0 || launch_index >= SDL_GetNumVideoDisplays()) launch_index = 0;
        const int launch_position = SDL_WINDOWPOS_CENTERED_DISPLAY(launch_index);
        host.window = SDL_CreateWindow(host.options.title.c_str(), launch_position, launch_position,
            host.options.width, host.options.height, flags);
        sdl_require(host.window != nullptr, "SDL_CreateWindow");
        if (host.options.frontend) imgui_backend::set_window(host.window);
        window = host.window;
        SDL_DisplayMode mode{};
        if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(host.window), &mode) == 0 && mode.refresh_rate > 0)
            host.display_rate.store(uint32_t(mode.refresh_rate), std::memory_order_release);
        refresh_display_capabilities();
        apply_launcher_display();
        if (guarded) {
            verify_required_display(launch_index, "launcher placement");
            show_verified_required_display(launch_index, "launcher shown");
        }
        log("native_window_created", {{"width", host.options.width}, {"height", host.options.height},
            {"display_refresh", host.display_rate.load(std::memory_order_acquire)}, {"game_frame_verified", false}});
    }
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    sdl_require(SDL_GetWindowWMInfo(host.window, &info) == SDL_TRUE, "SDL_GetWindowWMInfo");
    return {info.info.win.window, GetCurrentThreadId()};
#elif defined(__APPLE__)
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    sdl_require(SDL_GetWindowWMInfo(host.window, &info) == SDL_TRUE, "SDL_GetWindowWMInfo");
    if (!host.metal_view) host.metal_view = SDL_Metal_CreateView(host.window);
    sdl_require(host.metal_view != nullptr, "SDL_Metal_CreateView");
    return {info.info.cocoa.window, SDL_Metal_GetLayer(host.metal_view)};
#else
    return host.window;
#endif
}
#ifdef _WIN32
std::filesystem::path choose_rom_file() {
    require_main();
    require(host.window != nullptr, "ROM picker requires the launcher window");
    SDL_SysWMinfo window_info{};
    SDL_VERSION(&window_info.version);
    sdl_require(SDL_GetWindowWMInfo(host.window, &window_info) == SDL_TRUE,
        "Get ROM picker owner");
    const auto check = [](HRESULT result, const char* operation) {
        if (FAILED(result)) {
            std::ostringstream message;
            message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ")";
            throw std::runtime_error(message.str());
        }
    };
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    check(initialized, "Initialize Windows file picker");
    struct ComLifetime { ~ComLifetime() { CoUninitialize(); } } com_lifetime;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf())), "Create Windows file picker");
    FILEOPENDIALOGOPTIONS options{};
    check(dialog->GetOptions(&options), "Read file picker options");
    check(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
        FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR), "Set file picker options");
    const COMDLG_FILTERSPEC filters[] = {{L"Banjo-Tooie ROM (*.z64)", L"*.z64"}};
    check(dialog->SetFileTypes(1, filters), "Set ROM filter");
    check(dialog->SetTitle(L"Select your Banjo-Tooie NTSC-U 1.0 ROM"), "Set file picker title");
    // An explicit owner keeps the modal dialog associated with this launcher,
    // including its monitor, rather than the user's primary desktop window.
    const HRESULT shown = dialog->Show(window_info.info.win.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    check(shown, "Open Windows file picker");
    Microsoft::WRL::ComPtr<IShellItem> selection;
    check(dialog->GetResult(selection.GetAddressOf()), "Read ROM selection");
    PWSTR filename = nullptr;
    check(selection->GetDisplayName(SIGDN_FILESYSPATH, &filename), "Read ROM path");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> selected_path(filename, &CoTaskMemFree);
    return std::filesystem::path(selected_path.get());
}
#endif

void poll_events() {
    require_main();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
            host.close.store(true);
    }
    uint32_t snapshot = 0;
    if (host.window && SDL_GetKeyboardFocus() == host.window && host.options.keyboard_controller) {
        const auto* keys = SDL_GetKeyboardState(nullptr);
        uint16_t buttons = 0;
        if (keys[SDL_SCANCODE_J]) buttons |= 0x8000; // A
        if (keys[SDL_SCANCODE_K]) buttons |= 0x4000; // B
        if (keys[SDL_SCANCODE_SPACE]) buttons |= 0x2000; // Z
        if (keys[SDL_SCANCODE_RETURN]) buttons |= 0x1000; // Start
        if (keys[SDL_SCANCODE_Q]) buttons |= 0x0020; // L
        if (keys[SDL_SCANCODE_E]) buttons |= 0x0010; // R
        if (keys[SDL_SCANCODE_UP]) buttons |= 0x0008;
        if (keys[SDL_SCANCODE_DOWN]) buttons |= 0x0004;
        if (keys[SDL_SCANCODE_LEFT]) buttons |= 0x0002;
        if (keys[SDL_SCANCODE_RIGHT]) buttons |= 0x0001;
        float x = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
        float y = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
        if (x != 0 && y != 0) { x *= 0.70710678f; y *= 0.70710678f; }
        snapshot = buttons | (uint32_t(uint8_t(int8_t(std::lround(x * 85)))) << 16)
            | (uint32_t(uint8_t(int8_t(std::lround(y * 85)))) << 24);
    }
    host.keyboard.store(snapshot, std::memory_order_release);
}
void poll_frontend_events() {
    require_main();
    require(host.options.frontend, "Frontend event polling requires frontend mode");
    if (!host.frontend_input_initialized) {
        input::initialize_settings(recomp::get_config_path());
        host.frontend_input_initialized = true;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Capture and release cooldown own all keyboard navigation events.
        if (!input::input_suppressed() ||
            (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP && event.type != SDL_TEXTINPUT))
            imgui_backend::handle_event(event);
        if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
            host.close.store(true, std::memory_order_release);
        if (event.type == SDL_KEYDOWN) input::note_relevant_keydown(
            static_cast<std::uint32_t>(event.key.keysym.scancode), event.key.repeat != 0,
            ultramodern::is_game_started(), SDL_GetKeyboardFocus() == host.window,
            false, menu::is_open(), input::capture_active(), false);
        input::handle_event(event);
        menu::handle_event(event);
    }
    // Once window close is admitted, do not start a ROM or apply a queued save
    // action from the render thread. The lifecycle owner now performs shutdown.
    if (host.close.load(std::memory_order_acquire)) return;
    menu::tick();
    const bool begin_gameplay = frontend_gameplay_requested.exchange(false, std::memory_order_acq_rel);
    bool game_started = frontend_gameplay_active.load(std::memory_order_acquire);
    if (begin_gameplay && !game_started) {
        frontend_gameplay_graphics_applied.store(false, std::memory_order_release);
        frontend_gameplay_active.store(true, std::memory_order_release);
        game_started = true;
        save_progress::reset();
        host.save_status = save_progress::Status::Idle;
        apply_requested_display(true);
        // The RT64 owner consumes this request and selects the queued gameplay
        // profile or fixed launcher profile from frontend_gameplay_active.
        ultramodern::renderer::set_graphics_config(vanilla_graphics_config());
        log("native_frontend_phase", {{"phase", "gameplay"},
            {"trigger", "wait_for_game_started returned"}, {"saved_gameplay_profile_applied", true}});
    }
    else if (game_started) {
        apply_requested_display();
    }
    input::tick(host.window, game_started, menu::is_open());
    const bool show_cursor = !game_started ||
        (menu::is_open() && menu::get_bool("tooie_show_cursor_when_opening_settings", true));
    SDL_ShowCursor(show_cursor ? SDL_ENABLE : SDL_DISABLE);
    const auto menu_pad = input::menu_gamepad_state();
    imgui_backend::set_gamepad_state({menu_pad.connected, menu_pad.buttons, menu_pad.left_x, menu_pad.left_y});
    // The shared input profile owns the bindable Save Progress edge.
    // F5 remains its default; polling it here would bypass remapping and UI capture.
    const auto save_status = save_progress::status();
    if (save_status != host.save_status) {
        host.save_status = save_status;
        log("frontend_save_progress", {{"status", save_status_text(save_status)},
            {"file_persistence_acknowledged", save_status == save_progress::Status::Persisted}, {"owner", "SDL/UI thread status sample"}});
        switch (save_status) {
            case save_progress::Status::PendingPauseMenu:
                show_save_notice("Request received. Keep the original pause menu open.");
                break;
            case save_progress::Status::SubmittedToGame:
                show_save_notice("Saving progress. Waiting for the save file to finish writing.");
                break;
            case save_progress::Status::ExpiredOutsidePause:
                show_save_notice("No save was requested. Open the original pause menu, then use Save Progress.");
                break;
            case save_progress::Status::RejectedUnavailable:
                show_save_notice("Save Progress is unavailable here; no save write was confirmed.");
                break;
            case save_progress::Status::Persisted:
                show_save_notice("Progress saved successfully. This notice closes in 3 seconds.", true);
                break;
            case save_progress::Status::PersistenceFailed:
                show_save_notice("The save file could not be written. Your progress was not confirmed saved.");
                break;
            default: break;
        }
    }
    // Sample low-rate diagnostics without touching SDL from the renderer/UI
    // overlay. Edge events make transition hitches searchable in the log.
    static auto next_health_sample = std::chrono::steady_clock::time_point{};
    static bool last_cutscene = false;
    const auto now = std::chrono::steady_clock::now();
    const bool cutscene = features::cutscene_active();
    static unsigned last_speed_rate = 1U;
    const unsigned speed_rate = timing::rate();
    if (speed_rate != last_speed_rate) {
        log("intentional_speedup", {{"multiplier", speed_rate}, {"audio_muted", speed_rate > 1U}});
        last_speed_rate = speed_rate;
    }
    if (cutscene != last_cutscene) {
        log("cutscene_transition", {{"active", cutscene},
            {"diagnostics", diagnostics::issue_summary(diagnostics::snapshot(pacing_readout()))}});
        last_cutscene = cutscene;
    }
    if (game_started && now >= next_health_sample) {
        const auto camera_guard = camera_interpolation::stats();
        const auto model_guard = model_interpolation::stats();
        const auto input = tooie::input::input_debug_snapshot();
        const auto scene_setup = scene::snapshot();
        log("runtime_health", {{"diagnostics", diagnostics::issue_summary(diagnostics::snapshot(pacing_readout()))},
            {"title_attract_timing", tooie::title_timing_snapshot()},
            {"scene_setup_children", {
                {"activation_active", scene_setup.activation_active},
                {"completed_activations", scene_setup.activations_completed},
                {"subsystem_bootstrap", {{"parent_pc", 0x800A5C28U},
                    {"slowest_call_pc", scene_setup.setup_a5c28_slowest_call_pc},
                    {"slowest_call_ms", scene_setup.setup_a5c28_slowest_call_ms},
                    {"observed_calls", scene_setup.setup_a5c28_observed_calls}}},
                {"player_initialization", {{"parent_pc", 0x800F73C4U},
                    {"slowest_call_pc", scene_setup.setup_f73c4_slowest_call_pc},
                    {"slowest_call_ms", scene_setup.setup_f73c4_slowest_call_ms},
                    {"observed_calls", scene_setup.setup_f73c4_observed_calls}}}}},
            {"utility_input", {{"total_keydowns", input.total_keydowns}, {"recognized_utility_keydowns", input.relevant_keydowns},
                {"escape", input.escape_keydowns}, {"return", input.return_keydowns},
                {"sdl_keyboard_focus_now", SDL_GetKeyboardFocus() == host.window},
                {"sdl_mouse_focus_now", SDL_GetMouseFocus() == host.window},
                {"sdl_input_focus_flag", host.window && (SDL_GetWindowFlags(host.window) & SDL_WINDOW_INPUT_FOCUS) != 0},
                {"function_keys", input.function_keydowns}, {"last_scancode", input.last_scancode},
                {"repeat", input.last_repeat}, {"game_started", input.last_game_started},
                {"focus", input.last_keyboard_focus}, {"all_disabled", input.last_all_input_disabled},
                {"capture", input.last_context_capture}, {"binding", input.last_binding_scan},
                {"skip_events", input.last_skip_events},
                {"accepted_save", input.accepted_save_progress},
                {"accepted_diagnostics", input.accepted_diagnostics},
                {"accepted_markers", input.accepted_issue_markers},
                {"accepted_speedup", input.accepted_fast_forward},
                {"accepted_skip", input.accepted_cutscene_skips}}},
            {"cutscene", cutscene},
            {"empty_audio_queue_queries", empty_audio_queue_queries.load(std::memory_order_relaxed)},
            {"camera_cut_skips", camera_guard.consumed_boundary_skips},
            {"cutscene_original_motion_tasks", camera_guard.consumed_original_motion_tasks},
            {"cutscene_motion_requested", camera_interpolation::configured_cutscene_motion() == camera_interpolation::CutsceneMotion::Original ? "original" : "interpolated"},
            {"camera_guard_disabled", camera_guard.disabled},
            {"model_pose_ranges", model_guard.recorded_ranges},
            {"model_pose_tasks", model_guard.consumed_tasks},
            {"model_pose_invalid_ranges", model_guard.invalid_ranges},
            {"model_pose_rejected_ranges", model_guard.rejected_ranges},
            {"model_pose_disabled", model_guard.disabled},
            {"model_pose_mismatches", model_guard.mismatches},
            {"model_pose_overflows", model_guard.overflows},
            {"replay_rate_tasks", camera_guard.consumed_replay_rate_tasks},
            {"last_replay_refresh_rate", camera_guard.last_replay_refresh_rate},
            {"camera_guard_mismatches", camera_guard.mismatches},
            {"camera_guard_overflows", camera_guard.overflows}});
        next_health_sample = now + std::chrono::seconds{5};
    }
    const bool physical_controller = input::physical_controller();
    const bool rumble_capable = input::rumble_capable();
    frontend_physical_controller.store(physical_controller, std::memory_order_release);
    frontend_rumble_capable.store(rumble_capable, std::memory_order_release);
    const int state = int(physical_controller) | (int(rumble_capable) << 1);
    if (frontend_presence_log_state.exchange(state, std::memory_order_acq_rel) != state) {
        log("frontend_input_presence", {{"virtual_port", 0}, {"keyboard_fallback", true},
            {"physical_controller", physical_controller}, {"rumble_capable", rumble_capable},
            {"sample_owner", "SDL/UI thread"}});
    }
    if (exited.load()) host.close.store(true);
}
bool close_requested() noexcept { return host.close.load(); }
void request_gameplay_profile() noexcept {
    if (host.options.frontend) frontend_gameplay_requested.store(true, std::memory_order_release);
}
bool gameplay_profile_applied() noexcept {
    return frontend_gameplay_active.load(std::memory_order_acquire) &&
        frontend_gameplay_graphics_applied.load(std::memory_order_acquire);
}
Counters counters() noexcept { return {host.submitted.load(), host.parsed.load(), host.vi_updates.load(), host.audio_frames.load()}; }
void shutdown() {
    if (!host.initialized.load()) return;
    require_main();
    require(host.renderer_count.load() == 0, "Join and destroy native renderer before destroying SDL host");
    std::exception_ptr finalization_error;
    auto retain=[&]{if(!finalization_error)finalization_error=std::current_exception();};
    {
        std::lock_guard guard(host.audio_mutex);
        if (host.audio) {
            try{log("native_audio_shutdown", {{"remaining_frames", SDL_GetQueuedAudioSize(host.audio) / 4}});}catch(...){retain();}
            SDL_CloseAudioDevice(host.audio);
            host.audio = 0;
        }
        try{finish_audio_capture();}catch(...){retain();}
    }
#ifdef __APPLE__
    if (host.metal_view) { SDL_Metal_DestroyView(host.metal_view); host.metal_view = nullptr; }
#endif
    if (host.options.frontend) { input::shutdown(); imgui_backend::shutdown(); host.frontend_input_initialized = false; }
    if (host.window) { SDL_DestroyWindow(host.window); host.window = nullptr; window = nullptr; }
    host.keyboard.store(0);
    save_progress::reset();
    frontend_physical_controller.store(false, std::memory_order_release);
    frontend_rumble_capable.store(false, std::memory_order_release);
    frontend_presence_log_state.store(-1, std::memory_order_release);
    host.initialized.store(false);
    SDL_Quit();
    try{audio_pacing::finish();}catch(...){retain();} // Caller has already joined guest/runtime producers.
    try{
        auto observed=host.sdl_observation.finish();
        if(observed.enabled)log("sdl_audio_observation_finalized",{{"count",observed.count},{"attempted",observed.attempted},
            {"overflow",observed.overflow},{"directory",host.options.sdl_observation_directory.string()},
            {"after_sdl_close_and_quit",true},{"observer_released",true}});
    }catch(...){retain();}
    try{log("native_host_shutdown", {{"producers_must_already_be_joined", true}});}catch(...){retain();}
    if(finalization_error)try{std::rethrow_exception(finalization_error);}
    catch(const std::exception& e){throw FinalizationError(std::string("Native evidence finalization after device cleanup: ")+e.what());}
    catch(...){throw FinalizationError("Native evidence finalization after device cleanup: unknown exception");}
}
bool capture_visible_client_bmp(const std::filesystem::path& output) {
    require_main();
    require(host.window != nullptr, "Capture requires an existing window");
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    sdl_require(SDL_GetWindowWMInfo(host.window, &info) == SDL_TRUE, "SDL_GetWindowWMInfo");
    HWND window = info.info.win.window;
    RECT rect{};
    if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0) return false;
    HDC source = GetDC(window);
    if (!source) return false;
    HDC dest = CreateCompatibleDC(source);
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = rect.right;
    bitmap.bmiHeader.biHeight = -rect.bottom;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP image = CreateDIBSection(source, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool success = false;
    if (dest && image) {
        HGDIOBJ old = SelectObject(dest, image);
        if (BitBlt(dest, 0, 0, rect.right, rect.bottom, source, 0, 0, SRCCOPY)) {
            GdiFlush();
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            const uint32_t count = uint32_t(rect.right) * uint32_t(rect.bottom) * 4;
            header.bfSize = header.bfOffBits + count;
            std::ofstream file(output, std::ios::binary);
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(&bitmap.bmiHeader), sizeof(BITMAPINFOHEADER));
            file.write(static_cast<const char*>(bits), count);
            success = bool(file);
        }
        SelectObject(dest, old);
    }
    if (image) DeleteObject(image);
    if (dest) DeleteDC(dest);
    ReleaseDC(window, source);
    log("native_client_capture", {{"path", output.string()}, {"captured", success},
        {"source", "Windows client DC BitBlt; may be obscured or unsupported by compositor"},
        {"game_frame_verified", false}});
    return success;
#else
    log("native_client_capture_unsupported", {{"path", output.string()}});
    return false;
#endif
}
ultramodern::renderer::GraphicsConfig vanilla_graphics_config() {
    using namespace ultramodern::renderer;
    GraphicsConfig config{};
    config.developer_mode = false;
    config.res_option = Resolution::Original;
    config.wm_option = WindowMode::Windowed;
    config.hr_option = HUDRatioMode::Original;
    config.api_option = GraphicsApi::Vulkan;
    config.ar_option = AspectRatio::Original;
    config.msaa_option = Antialiasing::None;
    config.rr_option = RefreshRate::Original;
    config.hpfb_option = HighPrecisionFramebuffer::Off;
    config.rr_manual_value = 60;
    config.ds_option = 1;
    return config;
}
void request_graphics_settings(frontend::GraphicsSettings settings) noexcept {
    requested_fullscreen.store(settings.fullscreen);
    requested_resolution.store(settings.resolution_multiplier);
    requested_auto_resolution.store(settings.auto_resolution);
    requested_downsample.store(settings.downsample_multiplier);
    requested_msaa.store(settings.msaa_samples);
    requested_vsync.store(settings.vsync);
    requested_rate_mode.store(uint32_t(settings.output_rate_mode));
    requested_custom_rate.store(settings.custom_output_rate);
    requested_presentation.store(uint32_t(settings.presentation_mode));
}
frontend::GraphicsSettings requested_graphics_settings() noexcept {
    frontend::GraphicsSettings settings{};
    settings.fullscreen = requested_fullscreen.load();
    settings.resolution_multiplier = requested_resolution.load();
    settings.auto_resolution = requested_auto_resolution.load();
    settings.downsample_multiplier = requested_downsample.load();
    settings.msaa_samples = requested_msaa.load();
    settings.vsync = requested_vsync.load();
    settings.output_rate_mode = static_cast<frontend::OutputRateMode>(requested_rate_mode.load());
    settings.custom_output_rate = requested_custom_rate.load();
    settings.presentation_mode = static_cast<frontend::PresentationMode>(requested_presentation.load());
    return settings;
}
void request_display_settings(const frontend::DisplaySettings& settings) noexcept {
    const auto normalized = frontend::normalize_display(settings).settings;
    bool changed = requested_output_mode.exchange(uint32_t(normalized.mode)) != uint32_t(normalized.mode);
    changed |= requested_output_display.exchange(uint32_t(normalized.display)) != uint32_t(normalized.display);
    changed |= requested_output_resolution.exchange(uint32_t(normalized.resolution)) != uint32_t(normalized.resolution);
    changed |= requested_output_fit.exchange(uint32_t(normalized.fit)) != uint32_t(normalized.fit);
    if (changed) requested_display_generation.fetch_add(1, std::memory_order_release);
}
frontend::DisplaySettings requested_display_settings() noexcept {
    return {
        static_cast<frontend::OutputMode>(requested_output_mode.load()),
        static_cast<frontend::OutputDisplayId>(requested_output_display.load()),
        static_cast<frontend::OutputResolutionId>(requested_output_resolution.load()),
        static_cast<frontend::OutputFit>(requested_output_fit.load())};
}
frontend::GraphicsCapabilities graphics_capabilities() noexcept {
    return {sample_positions_supported.load(), max_msaa_samples.load(), true, true, true};
}
DisplayCapabilities display_capabilities() {
    std::lock_guard guard(display_state_mutex);
    return current_display_capabilities;
}
PacingReadout pacing_readout() {
    PacingReadout result;
    {
        std::lock_guard guard(display_state_mutex);
        result = current_display_readout;
    }
    const auto requested = requested_graphics_settings();
    const auto presents = pacing::present_snapshot();
    result.renderer_ready = renderer_ready.load(std::memory_order_acquire);
    result.gameplay_active = frontend_gameplay_active.load(std::memory_order_acquire);
    result.display_hz = host.display_rate.load(std::memory_order_acquire);
    result.swapchain_hz = actual_swapchain_hz.load(std::memory_order_acquire);
    result.source_hz = presents.source_hz;
    result.requested_target_hz = actual_requested_target_hz.load(std::memory_order_acquire);
    result.effective_target_hz = presents.target_hz != 0 ? presents.target_hz
        : actual_effective_target_hz.load(std::memory_order_acquire);
    result.vsync_requested = requested.vsync;
    result.vsync_actual = presents.vsync_actual;
    result.presentation_requested = requested.presentation_mode;
    result.presentation_effective = static_cast<frontend::PresentationMode>(actual_presentation.load(std::memory_order_acquire));
    result.present_interval_ms = presents.mean_interval_ms;
    result.present_samples = presents.interval_samples;
    result.timing_available = presents.interval_samples != 0;
    result.guest_update_hz = measured_guest_update_hz.load(std::memory_order_acquire);
    result.host_vi_hz = measured_host_vi_hz.load(std::memory_order_acquire);
    result.graphics_task_hz = measured_graphics_task_hz.load(std::memory_order_acquire);
    result.guest_wait_bypassed = measured_guest_wait_bypassed.load(std::memory_order_acquire);
    result.cadence_sample_seconds = measured_cadence_seconds.load(std::memory_order_acquire);
    const auto audio = audio_queue_sample.load(std::memory_order_acquire);
    result.audio_frequency = uint32_t(audio >> 32);
    result.audio_queued_frames = uint32_t(audio);
    result.audio_queue_available = result.audio_frequency != 0;
    result.audio_empty_queue_queries = empty_audio_queue_queries.load(std::memory_order_relaxed);
    return result;
}
ultramodern::input::connected_device_info_t frontend_connected(int port) {
    if (port == 0 && host.initialized.load()) {
        // The virtual N64 port remains connected so keyboard-only play works;
        // only advertise the accessory from the SDL-owner snapshot when a real
        // open controller can rumble.
        return {ultramodern::input::Device::Controller,
            frontend_rumble_capable.load(std::memory_order_acquire)
                ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}
ultramodern::renderer::callbacks_t renderer_callbacks() { return {create_renderer}; }
ultramodern::audio_callbacks_t audio_callbacks() { return {queue_samples, frames_remaining, set_frequency}; }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_renderer_quiesce(uint64_t epoch, std::chrono::milliseconds timeout) noexcept {
    if (epoch == 0 || timeout <= std::chrono::milliseconds::zero()) return false;
    uint64_t empty = 0;
    if (!persistent_renderer_lease_epoch.compare_exchange_strong(empty, epoch,
            std::memory_order_acq_rel)) {
        persistent_renderer_refusal.store("renderer_lease_busy", std::memory_order_release);
        return false;
    }
    auto* renderer = persistent_active_renderer.load(std::memory_order_acquire);
    auto* application = renderer ? renderer->persistent_application() : nullptr;
    if (!application || !application->state || !application->workloadQueue ||
        !application->presentQueue || !renderer_ready.load(std::memory_order_acquire) ||
        host.submitted.load(std::memory_order_acquire) != host.parsed.load(std::memory_order_acquire)) {
        persistent_renderer_refusal.store("renderer_or_gfx_action_unavailable", std::memory_order_release);
        persistent_renderer_lease_epoch.store(0, std::memory_order_release);
        return false;
    }
    auto& workloads = *application->workloadQueue;
    auto& presents = *application->presentQueue;
    if (const char* reason = persistent_bridge::clean_boundary_reason(*application->state, workloads)) {
        persistent_renderer_refusal.store(reason, std::memory_order_release);
        persistent_renderer_lease_epoch.store(0, std::memory_order_release);
        return false;
    }
    const auto target_workload = application->state->workloadId;
    const auto target_present = application->state->presentId;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        {
            // Both cursors advance before their worker takes threadMutex.
            // Try every lock; a blocking wait would violate the hook deadline.
            std::unique_lock<std::mutex> work_thread(workloads.threadMutex, std::try_to_lock);
            std::unique_lock<std::mutex> present_thread(presents.threadMutex, std::try_to_lock);
            std::unique_lock<std::mutex> idle_worker(workloads.workerMutex, std::try_to_lock);
            std::unique_lock<std::mutex> idle_state(workloads.idleMutex, std::try_to_lock);
            if (work_thread.owns_lock() && present_thread.owns_lock() &&
                idle_worker.owns_lock() && idle_state.owns_lock()) {
                std::unique_lock<std::mutex> work_cursor(workloads.cursorMutex, std::try_to_lock);
                std::unique_lock<std::mutex> present_cursor(presents.cursorMutex, std::try_to_lock);
                std::unique_lock<std::mutex> work_id(workloads.workloadIdMutex, std::try_to_lock);
                std::unique_lock<std::mutex> present_id(presents.presentIdMutex, std::try_to_lock);
                if (work_cursor.owns_lock() && present_cursor.owns_lock() &&
                    work_id.owns_lock() && present_id.owns_lock() &&
                    !workloads.idleActive && workloads.writeCursor == workloads.threadCursor &&
                    presents.writeCursor == presents.threadCursor &&
                    workloads.workloadId >= target_workload && presents.presentId >= target_present) {
                    persistent_renderer_refusal.store("none", std::memory_order_release);
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    persistent_renderer_refusal.store("renderer_workload_or_present_drain_timeout", std::memory_order_release);
    persistent_renderer_lease_epoch.store(0, std::memory_order_release);
    return false;
}

void persistent_renderer_resume(uint64_t epoch) noexcept {
    if (epoch != 0 && persistent_renderer_lease_epoch.load(std::memory_order_acquire) == epoch)
        persistent_renderer_lease_epoch.store(0, std::memory_order_release);
}

void persistent_renderer_abandon_for_shutdown(uint64_t epoch) noexcept {
    if (epoch != 0 && persistent_renderer_lease_epoch.load(std::memory_order_acquire) == epoch)
        persistent_renderer_lease_epoch.store(0, std::memory_order_release);
}

bool persistent_renderer_export(std::vector<uint8_t>& output, uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_renderer_lease_epoch.load(std::memory_order_acquire) != epoch)
        return false;
    auto* renderer = persistent_active_renderer.load(std::memory_order_acquire);
    auto* application = renderer ? renderer->persistent_application() : nullptr;
    if (!application || !application->state || !application->workloadQueue) return false;
    if (const char* reason = persistent_bridge::clean_boundary_reason(*application->state,
            *application->workloadQueue)) {
        persistent_renderer_refusal.store(reason, std::memory_order_release);
        return false;
    }
    if (!persistent_bridge::export_image(*application->state, output)) {
        persistent_renderer_refusal.store("renderer_image_export_failed", std::memory_order_release);
        return false;
    }
    return true;
}

bool persistent_renderer_validate(const std::vector<uint8_t>& input) noexcept {
    return persistent_bridge::valid_image_blob(input);
}

bool persistent_renderer_restore_epoch(const std::vector<uint8_t>& input,
    uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_renderer_lease_epoch.load(std::memory_order_acquire) != epoch ||
        !persistent_bridge::valid_image_blob(input)) return false;
    try {
        auto owned = std::make_shared<const std::vector<uint8_t>>(input);
        {
            std::lock_guard guard(persistent_renderer_restore_mutex);
            persistent_renderer_restore_blob = std::move(owned);
            persistent_renderer_restore_epoch_active.store(epoch, std::memory_order_release);
        }
        const bool recreated = tooie_persistent_events_renderer_action(epoch,
            &persistent_recreate_callback, std::chrono::seconds(30));
        persistent_renderer_restore_epoch_active.store(0, std::memory_order_release);
        {
            std::lock_guard guard(persistent_renderer_restore_mutex);
            persistent_renderer_restore_blob.reset();
        }
        if (!recreated)
            persistent_renderer_refusal.store("renderer_gfx_owner_recreate_failed", std::memory_order_release);
        return recreated;
    } catch (...) {
        persistent_renderer_restore_epoch_active.store(0, std::memory_order_release);
        persistent_renderer_refusal.store("renderer_restore_allocation_failed", std::memory_order_release);
        return false;
    }
}

const char* persistent_renderer_refusal_reason() noexcept {
    return persistent_renderer_refusal.load(std::memory_order_acquire);
}

bool persistent_audio_quiesce(uint64_t epoch, std::chrono::milliseconds timeout) noexcept {
    if (epoch == 0 || timeout <= std::chrono::milliseconds::zero() ||
        !host.initialized.load(std::memory_order_acquire)) return false;
    uint64_t empty = 0;
    if (!persistent_audio_lease_epoch.compare_exchange_strong(empty, epoch,
            std::memory_order_acq_rel)) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(host.audio_mutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            persistent_audio_lease_epoch.store(0, std::memory_order_release);
            return false;
        }
        std::this_thread::yield();
    }
    if (host.audio) SDL_PauseAudioDevice(host.audio, 1);
    if (!persistent_audio_reconcile_locked() ||
        persistent_audio_ledger.size() > persistent_audio_max_samples ||
        (host.audio && persistent_audio_ledger.size() > size_t(host.frequency) * 4U)) {
        if (host.audio) SDL_PauseAudioDevice(host.audio, 0);
        persistent_audio_lease_epoch.store(0, std::memory_order_release);
        return false;
    }
    persistent_audio_lease = std::move(lock);
    return true;
}

void persistent_audio_resume(uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_audio_lease_epoch.load(std::memory_order_acquire) != epoch)
        return;
    if (host.audio) SDL_PauseAudioDevice(host.audio, 0);
    persistent_audio_lease.unlock();
    persistent_audio_lease_epoch.store(0, std::memory_order_release);
}

void persistent_audio_abandon_for_shutdown(uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_audio_lease_epoch.load(std::memory_order_acquire) != epoch)
        return;
    // FatalParked cannot replay the old output. Leave SDL paused and release
    // only the host mutex so the mandatory shutdown path can close the device.
    persistent_audio_lease.unlock();
    persistent_audio_lease_epoch.store(0, std::memory_order_release);
}

bool persistent_audio_export(persistent_state::devices::AudioState& output,
    uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_audio_lease_epoch.load(std::memory_order_acquire) != epoch ||
        !persistent_audio_lease.owns_lock() || !persistent_audio_reconcile_locked()) return false;
    try {
        persistent_state::devices::AudioState candidate;
        candidate.frequency = host.audio ? host.frequency : 0;
        candidate.queued_stereo_samples = uint32_t(persistent_audio_ledger.size() / 2);
        candidate.rebuffering = audio_rebuffering_after_fast_forward;
        candidate.queued_pcm = persistent_audio_ledger;
        output = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

bool persistent_audio_restore_epoch(const persistent_state::devices::AudioState& input,
    uint64_t epoch) noexcept {
    if (epoch == 0 || persistent_audio_lease_epoch.load(std::memory_order_acquire) != epoch ||
        !persistent_audio_lease.owns_lock() ||
        input.queued_pcm.size() != size_t(input.queued_stereo_samples) * 2 ||
        input.queued_pcm.size() > persistent_audio_max_samples ||
        (input.frequency != 0 && input.queued_pcm.size() > size_t(input.frequency) * 4U) ||
        (input.frequency == 0 && !input.queued_pcm.empty()) ||
        (input.frequency != 0 && (input.frequency < 1000 || input.frequency > 192000))) return false;

    // The lease is held and the device is paused. Discard only the old epoch's
    // output after the restored guest/device bytes have been committed.
    if (host.audio) SDL_ClearQueuedAudio(host.audio);
    if (input.frequency == 0) {
        if (host.audio) SDL_CloseAudioDevice(host.audio);
        host.audio = 0;
        persistent_audio_ledger.clear();
        persistent_audio_ledger_valid = true;
        audio_rebuffering_after_fast_forward = input.rebuffering;
        audio_queue_sample.store(0, std::memory_order_release);
        return true;
    }
    if (host.audio && host.frequency != input.frequency) {
        SDL_CloseAudioDevice(host.audio);
        host.audio = 0;
    }
    if (!host.audio) {
        SDL_AudioSpec desired{}, obtained{};
        desired.freq = int(input.frequency);
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = nullptr;
        host.audio = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!host.audio || obtained.freq != desired.freq || obtained.format != desired.format ||
            obtained.channels != desired.channels) return false;
        SDL_PauseAudioDevice(host.audio, 1);
    }
    host.frequency = input.frequency;
    if (!input.queued_pcm.empty() && SDL_QueueAudio(host.audio, input.queued_pcm.data(),
            Uint32(input.queued_pcm.size() * sizeof(int16_t))) != 0) return false;
    if (SDL_GetQueuedAudioSize(host.audio) != input.queued_pcm.size() * sizeof(int16_t))
        return false;
    try {
        persistent_audio_ledger = input.queued_pcm;
    } catch (...) {
        return false;
    }
    persistent_audio_ledger_valid = true;
    audio_rebuffering_after_fast_forward = input.rebuffering;
    audio_queue_sample.store((uint64_t(host.frequency) << 32) | input.queued_stereo_samples,
        std::memory_order_release);
    return true;
}
#endif
ultramodern::input::callbacks_t input_callbacks() { return {poll_input_snapshot, get_input, nullptr, get_connected}; }
ultramodern::input::callbacks_t frontend_input_callbacks() {
    return {poll_input_snapshot, [](int port, uint16_t* buttons, float* x, float* y) -> bool {
        if (port != 0 || !host.initialized.load(std::memory_order_acquire)) return false;
        const auto value = input::snapshot();
        *buttons = value.buttons | input::consume_transient_buttons(); *x = value.x; *y = value.y;
        return true;
    }, input::set_rumble, frontend_connected};
}
ultramodern::gfx_callbacks_t gfx_callbacks() { return {gfx_create, gfx_window, gfx_update}; }
} // namespace tooie::native_host
