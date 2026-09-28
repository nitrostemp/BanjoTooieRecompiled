#pragma once

#include <cstdint>
#include "recomp.h"

namespace tooie::camera {
void set_analog_enabled(bool enabled) noexcept;
bool analog_enabled() noexcept;
void set_horizontal_inverted(bool inverted) noexcept;
bool horizontal_inverted() noexcept;
void set_vertical_inverted(bool inverted) noexcept;
bool vertical_inverted() noexcept;
}

// Called only from the pinned, already-gated camera path in func_800A4878.
// It may propagate the runtime's cooperative thread-termination exception.
// Returns -1 when the original C-left/C-right path must run. Otherwise returns
// the original downstream command result after consuming this analog frame.
extern "C" int tooie_camera_analog_apply(std::uint8_t* rdram, recomp_context* ctx);

// Called after Tooie's original bastick_getX/Y calls, inside bafpctrl's own
// active-mode gate. A zero/disabled right axis preserves the original value.
extern "C" float tooie_camera_first_person_axis(float original_value, int vertical) noexcept;
