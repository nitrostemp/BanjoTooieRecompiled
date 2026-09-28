#pragma once

#include <SDL.h>
#include <cstdint>

namespace tooie::imgui_backend {

// SDL owns the window and polls events on the main thread. Events are copied
// into a bounded queue and consumed by ImGui on RT64's present thread.
void set_window(SDL_Window* window) noexcept;
void handle_event(const SDL_Event& event);
struct MenuGamepadState {
    bool connected = false;
    std::uint32_t buttons = 0; // Bit positions are SDL_GameControllerButton values.
    float left_x = 0.0f;       // Normalized to [-1, 1].
    float left_y = 0.0f;       // SDL sign: down is positive.
};
void set_gamepad_state(MenuGamepadState state) noexcept;
void shutdown() noexcept;

} // namespace tooie::imgui_backend
