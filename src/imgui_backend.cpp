#include "imgui_backend.hpp"
#include "imgui_backend_rt64.hpp"

#include "gui/rt64_inspector.h"
#include "imgui/imgui.h"
#include "imgui_menu.hpp"
#include "frontend_input_preference.hpp"
#include "platform_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>

namespace tooie::imgui_backend {
namespace {
std::atomic<SDL_Window*> s_window{nullptr};
std::mutex s_events_mutex;
std::deque<SDL_Event> s_events;
std::unique_ptr<RT64::Inspector> s_inspector;
bool s_reset_input = false;
bool s_was_suppressed = false;
std::atomic<bool> s_gamepad_connected{false};
std::atomic<std::uint32_t> s_gamepad_buttons{0};
std::atomic<float> s_gamepad_left_x{0.0f};
std::atomic<float> s_gamepad_left_y{0.0f};
constexpr size_t kMaxPendingEvents = 1024;

void apply_gamepad() {
    ImGuiIO& io = ImGui::GetIO();
    const bool connected = !input::input_suppressed() && s_gamepad_connected.load(std::memory_order_acquire);
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    const std::uint32_t buttons = connected
        ? s_gamepad_buttons.load(std::memory_order_acquire) : 0;
    const auto button = [buttons](SDL_GameControllerButton sdl, ImGuiKey imgui) {
        ImGui::GetIO().AddKeyEvent(imgui, (buttons & (1u << sdl)) != 0);
    };
    button(SDL_CONTROLLER_BUTTON_START, ImGuiKey_GamepadStart);
    button(SDL_CONTROLLER_BUTTON_BACK, ImGuiKey_GamepadBack);
    button(SDL_CONTROLLER_BUTTON_X, ImGuiKey_GamepadFaceLeft);
    button(SDL_CONTROLLER_BUTTON_B, ImGuiKey_GamepadFaceRight);
    button(SDL_CONTROLLER_BUTTON_Y, ImGuiKey_GamepadFaceUp);
    button(SDL_CONTROLLER_BUTTON_A, ImGuiKey_GamepadFaceDown);
    button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft);
    button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight);
    button(SDL_CONTROLLER_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp);
    button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ImGuiKey_GamepadL1);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1);
    button(SDL_CONTROLLER_BUTTON_LEFTSTICK, ImGuiKey_GamepadL3);
    button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, ImGuiKey_GamepadR3);
    constexpr float dead_zone = 0.18f;
    const float x = connected ? s_gamepad_left_x.load(std::memory_order_acquire) : 0.0f;
    const float y = connected ? s_gamepad_left_y.load(std::memory_order_acquire) : 0.0f;
    const auto analog = [&io, dead_zone](ImGuiKey key, float value) {
        const float strength = value <= dead_zone ? 0.0f
            : std::clamp((value - dead_zone) / (1.0f - dead_zone), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, strength > 0.0f, strength);
    };
    analog(ImGuiKey_GamepadLStickLeft, -x);
    analog(ImGuiKey_GamepadLStickRight, x);
    analog(ImGuiKey_GamepadLStickUp, -y);
    analog(ImGuiKey_GamepadLStickDown, y);
}

void load_player_font() {
    const auto path = tooie::platform::executable_path().parent_path() / "assets" / "InterVariable.ttf";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return; // ImGui's built-in font keeps startup usable.
    const auto bytes = static_cast<std::streamoff>(file.tellg());
    if (bytes <= 0 || bytes > std::numeric_limits<int>::max()) return;
    void* data = ImGui::MemAlloc(static_cast<size_t>(bytes));
    file.seekg(0);
    if (!file.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes))) {
        ImGui::MemFree(data);
        return;
    }
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes), 18.0f);
}
}

void set_window(SDL_Window* window) noexcept {
    s_window.store(window, std::memory_order_release);
}

void handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL: case SDL_WINDOWEVENT:
        break;
    default:
        return;
    }
    std::lock_guard lock(s_events_mutex);
    if (s_events.size() == kMaxPendingEvents) {
        s_events.clear();
        s_reset_input = true;
    }
    s_events.push_back(event);
}

void set_gamepad_state(MenuGamepadState state) noexcept {
    s_gamepad_buttons.store(state.buttons, std::memory_order_release);
    s_gamepad_left_x.store(state.left_x, std::memory_order_release);
    s_gamepad_left_y.store(state.left_y, std::memory_order_release);
    s_gamepad_connected.store(state.connected, std::memory_order_release);
}

void initialize(plume::RenderDevice* device, const plume::RenderSwapChain* swap_chain,
                RT64::UserConfiguration::GraphicsAPI api) {
    if (api != RT64::UserConfiguration::GraphicsAPI::D3D12 &&
        api != RT64::UserConfiguration::GraphicsAPI::Vulkan) {
        std::fputs("ImGui player menu: this RT64 graphics API has no Inspector backend.\n", stderr);
        return;
    }
    SDL_Window* window = s_window.load(std::memory_order_acquire);
    if (window && device && swap_chain) {
        s_inspector = std::make_unique<RT64::Inspector>(device, swap_chain, api, window);
        // Persist player preferences in project config, not imgui.ini.
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                                       ImGuiConfigFlags_NavEnableGamepad |
                                       ImGuiConfigFlags_NoMouseCursorChange;
        load_player_font();
    }
}

void draw(RT64::RenderWorker* worker, plume::RenderCommandList* command_list) {
    if (!s_inspector || !worker || !command_list) return;
    std::deque<SDL_Event> events;
    bool reset_input = false;
    {
        std::lock_guard lock(s_events_mutex);
        events.swap(s_events);
        reset_input = s_reset_input;
        s_reset_input = false;
    }
    const bool suppressed = input::input_suppressed();
    if (reset_input || (suppressed && !s_was_suppressed)) ImGui::GetIO().ClearInputKeys();
    s_was_suppressed = suppressed;
    for (SDL_Event& event : events) {
        if (suppressed &&
            (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP || event.type == SDL_TEXTINPUT)) continue;
        s_inspector->handleSdlEvent(&event);
    }
    apply_gamepad();
    s_inspector->newFrame(worker);
    tooie::menu::draw();
    s_inspector->endFrame();
    s_inspector->draw(command_list);
}

void renderer_shutdown() noexcept {
    s_inspector.reset();
    std::lock_guard lock(s_events_mutex);
    s_events.clear();
    s_reset_input = false;
    s_was_suppressed = false;
}

void shutdown() noexcept {
    renderer_shutdown();
    s_window.store(nullptr, std::memory_order_release);
    set_gamepad_state({});
}

} // namespace tooie::imgui_backend
