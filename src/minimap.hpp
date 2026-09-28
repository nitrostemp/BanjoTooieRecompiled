#pragma once

#include "minimap_state.hpp"
#include "recomp.h"

#include <cstdint>

namespace tooie::minimap {

enum class Mode : std::uint32_t { Off = 0, Trail = 1 };
enum class Corner : std::uint32_t { BottomLeft = 0, BottomRight = 1 };
enum class Heading : std::uint32_t { Camera = 0, Character = 1 };

void set_mode(Mode mode) noexcept;
void set_corner(Corner corner) noexcept;
void set_heading(Heading heading) noexcept;
Heading heading() noexcept;
Mode mode() noexcept;
Corner corner() noexcept;
bool visible() noexcept;
TrailSnapshot trail_snapshot() noexcept;
// Draw only while gameplay is visible, on the ImGui frame owner thread.
void render(int width, int height);
void shutdown() noexcept;

} // namespace tooie::minimap

extern "C" void tooie_minimap_tick(std::uint8_t*, recomp_context*);
extern "C" bool tooie_minimap_visible() noexcept;
extern "C" void tooie_minimap_render(int, int);
extern "C" void tooie_minimap_shutdown() noexcept;
