#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace tooie::diagnostics::overlay {
enum class Mode : std::uint32_t { Off = 0, Fps = 1, Detailed = 2, Practice = 3 };
enum class Corner : std::uint32_t { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };
void set_mode(Mode mode) noexcept;
void set_corner(Corner corner) noexcept;
void set_issue_marker_sink(std::function<bool(std::string_view)> sink);
Mode mode() noexcept;
Corner corner() noexcept;
bool visible() noexcept;
void poll_input() noexcept;
void render(int width, int height);
void shutdown() noexcept;
}

extern "C" bool tooie_diagnostics_overlay_visible() noexcept;
extern "C" void tooie_diagnostics_overlay_poll_input() noexcept;
extern "C" void tooie_diagnostics_overlay_render(int, int);
extern "C" void tooie_diagnostics_overlay_shutdown() noexcept;
