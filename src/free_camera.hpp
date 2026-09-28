#pragma once

#include <array>
#include <cstdint>

#include "recomp.h"

namespace tooie::camera {

struct FreeCameraDebugSnapshot {
    std::array<float, 3> offset{};
    bool enabled = false;
    bool input_active = false;
};

// Detached translation only: the player, collision and game-owned camera
// update remain untouched. The frontend must zero motion on focus loss.
void set_free_camera_enabled(bool enabled) noexcept;
bool free_camera_enabled() noexcept;
// Runtime focus/UI gate, separate from the persisted user setting. False
// restores the original camera on the next active frame and clears the offset.
void set_free_camera_input_active(bool active) noexcept;
void set_free_camera_motion(float right, float up, float forward) noexcept;
// One-shot recenter request. The guest thread consumes it before applying the
// next detached-camera frame; no guest memory is touched by the caller.
void request_free_camera_reset() noexcept;
// Thread-safe host diagnostics copy. It never reads guest memory.
FreeCameraDebugSnapshot free_camera_debug_snapshot() noexcept;
void set_free_camera_speed(float units_per_second) noexcept;
float free_camera_speed() noexcept;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
// Call only while all guest workers are parked. Neutral admission forbids
// stale detached-camera native words from entering a checkpoint.
bool persistent_neutral() noexcept;
void persistent_reset_epoch() noexcept;
#endif

} // namespace tooie::camera

// Pinned around the active world-frame composer. begin temporarily offsets the
// original camera position; end restores its exact guest words. Original Euler
// angles at +0x18 remain game-owned.
extern "C" void tooie_free_camera_begin(std::uint8_t* rdram, recomp_context* ctx);
extern "C" void tooie_free_camera_end(std::uint8_t* rdram, recomp_context* ctx);
