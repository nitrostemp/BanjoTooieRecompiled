#pragma once

#include <cstdint>

namespace tooie::widescreen {

enum class NativeAspect : std::uint32_t {
    Ratio16x9 = 0,
    Ratio21x9 = 1,
    Ratio32x9 = 2,
    Ratio43x18 = 3,
};

// The frontend profile owns this optional preference. The value is latched when
// Start Game enters native Tooie; later UI saves take effect on the next game
// start. Diagnostic entry points retain the game's stored flag.
void configure_profile(bool enabled) noexcept;
void configure_native_aspect(NativeAspect aspect) noexcept;
void latch_for_game_start() noexcept;
bool latched_enabled() noexcept;
NativeAspect configured_native_aspect() noexcept;
NativeAspect latched_native_aspect() noexcept;
double latched_aspect_ratio() noexcept;
bool profile_override_active() noexcept;
uint32_t resolve_global_setting(uint32_t game_value) noexcept;

// Multiplies the original game's completed widescreen aspect. Callers must
// pass the live original widescreen flag from func_80015D14; original 4:3 and
// scripted 4:3 cutscene projections are returned unchanged.
float adjust_projection_aspect(float original_aspect, bool game_widescreen) noexcept;

} // namespace tooie::widescreen
