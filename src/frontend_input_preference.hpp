#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "SDL.h"

namespace tooie::input {
enum class Action : std::uint8_t {
    A, B, Z, Start, L, R, CUp, CDown, CLeft, CRight,
    DUp, DDown, DLeft, DRight, MoveUp, MoveDown, MoveLeft, MoveRight,
    Menu, SaveProgress, Diagnostics, IssueMarker, FastForward, SkipCutscene,
    FreeCameraReset, FreeCameraLeft, FreeCameraRight, FreeCameraUp,
    FreeCameraDown, FreeCameraForward, FreeCameraBack, Count
};
enum class Device : std::uint8_t { Keyboard, Button, AxisPositive, AxisNegative };
struct Binding { Device device = Device::Keyboard; int code = -1; };
enum class ControllerType : std::uint8_t { N64, DualSense, Xbox, NintendoPro, Steam, Other };
struct ControllerInfo {
    SDL_JoystickID instance = -1;
    std::string guid, name; // Legacy identity and hardware product name (SDL fallback).
    bool selected = false, rumble = false;
    std::string stable_id, friendly_name, serial, path;
    std::string mapping_name; // SDL GameController mapping label, for diagnostics only.
    std::string sdl_guid;
    std::uint16_t vendor = 0, product = 0;
    bool ambiguous = false; // Identical identities cannot be restored individually.
};
struct CaptureStatus {
    bool active = false, arming = false;
    int remaining_seconds = 0;
    Action action = Action::A;
    Device device = Device::Keyboard;
    unsigned slot = 0;
};
struct Snapshot { std::uint16_t buttons = 0; float x = 0, y = 0, right_x = 0, right_y = 0; bool game_enabled = false; };
struct MenuGamepadState { bool connected = false; std::uint32_t buttons = 0; float left_x = 0, left_y = 0; };
const char* action_name(Action action) noexcept;
Binding get_binding(Action action, Device device, unsigned slot = 0);
void set_binding(Action action, Binding binding, unsigned slot = 0);
void reset_bindings();
std::string binding_label(Binding binding);
std::string binding_label_for_type(Binding binding, ControllerType type);
const char* controller_type_name(ControllerType type) noexcept;
void begin_capture(Action action, Device device, unsigned slot = 0);
void cancel_capture() noexcept;
bool capture_active() noexcept;
CaptureStatus capture_status() noexcept;
bool input_suppressed() noexcept;
std::vector<ControllerInfo> controllers();
SDL_JoystickID selected_controller_instance() noexcept;
bool select_controller(SDL_JoystickID instance);
std::string selected_device_id();
std::string preferred_device_id(); // Saved identity, even while the device is missing.
bool controller_auto_selection() noexcept;
std::string selected_friendly_name();
void set_selected_friendly_name(std::string name);
ControllerType selected_controller_type();
void set_selected_controller_type(ControllerType type);
std::vector<std::string> controller_profiles();
std::string active_controller_profile();
bool create_controller_profile(std::string name);
bool select_controller_profile(std::string name);
bool rename_controller_profile(std::string name);
void reset_active_controller_profile();
std::string preferred_guid();
void set_preferred_guid(std::string guid);
void initialize_settings(const std::filesystem::path& config_root);
void shutdown() noexcept;
void handle_event(const SDL_Event& event);
void tick(SDL_Window* window, bool gameplay, bool menu_open);
Snapshot snapshot() noexcept;
std::uint16_t consume_transient_buttons() noexcept;
MenuGamepadState menu_gamepad_state() noexcept;
bool game_input_disabled() noexcept;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
// May be requested off the SDL thread while the guest lease is held; the UI
// thread applies edge/capture cleanup before its next event or input poll.
void persistent_reset_transients() noexcept;
#endif
void get_right_analog(int port, float* x, float* y) noexcept;
void set_rumble(int port, bool enabled) noexcept;
bool physical_controller() noexcept;
bool rumble_capable() noexcept;
void post_diagnostics_toggle() noexcept;
bool consume_diagnostics_toggle() noexcept;
void post_issue_marker() noexcept;
bool consume_issue_marker() noexcept;
enum class DiscreteInputAction : std::uint32_t { SaveProgress, ToggleDiagnostics, MarkIssue, FastForward, SkipCutscene };
struct InputDebugSnapshot {
    std::uint64_t total_keydowns = 0, relevant_keydowns = 0, escape_keydowns = 0,
        return_keydowns = 0, function_keydowns = 0, accepted_save_progress = 0,
        accepted_diagnostics = 0, accepted_issue_markers = 0,
        accepted_fast_forward = 0, accepted_cutscene_skips = 0;
    std::uint32_t last_scancode = 0;
    bool last_repeat = false, last_game_started = false, last_keyboard_focus = false,
        last_all_input_disabled = false, last_context_capture = false,
        last_binding_scan = false, last_skip_events = false;
};
void note_relevant_keydown(std::uint32_t scancode, bool repeat, bool game_started,
    bool keyboard_focus, bool all_input_disabled, bool context_capture,
    bool binding_scan, bool skip_events) noexcept;
void note_discrete_action(DiscreteInputAction action) noexcept;
InputDebugSnapshot input_debug_snapshot() noexcept;
} // namespace tooie::input
