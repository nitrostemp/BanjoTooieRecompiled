#include "camera_analog.hpp"

#include <bit>
#include <atomic>
#include <cmath>

#include "recomp.h"
#include "funcs.h"
#ifdef TOOIE_NATIVE_HOST
#include "frontend_input_preference.hpp"
#endif

namespace {
constexpr float k_full_turn_target_degrees = 45.0f;
constexpr std::uint32_t k_camera_mode_rule_table = 0x801247ACu;
std::atomic_bool configured_analog_enabled{false};
std::atomic_bool configured_horizontal_inverted{false};
std::atomic_bool configured_vertical_inverted{false};
}

void tooie::camera::set_analog_enabled(bool enabled) noexcept {
    configured_analog_enabled.store(enabled, std::memory_order_release);
}

bool tooie::camera::analog_enabled() noexcept {
    return configured_analog_enabled.load(std::memory_order_acquire);
}

void tooie::camera::set_horizontal_inverted(bool inverted) noexcept {
    configured_horizontal_inverted.store(inverted, std::memory_order_release);
}

bool tooie::camera::horizontal_inverted() noexcept {
    return configured_horizontal_inverted.load(std::memory_order_acquire);
}

void tooie::camera::set_vertical_inverted(bool inverted) noexcept {
    configured_vertical_inverted.store(inverted, std::memory_order_release);
}

bool tooie::camera::vertical_inverted() noexcept {
    return configured_vertical_inverted.load(std::memory_order_acquire);
}

extern "C" float tooie_camera_first_person_axis(float original_value, int vertical) noexcept {
#ifndef TOOIE_NATIVE_HOST
    (void)vertical;
    return original_value;
#else
    if (!tooie::camera::analog_enabled() || tooie::input::game_input_disabled()) {
        return original_value;
    }

    float right_x = 0.0f;
    float right_y = 0.0f;
    tooie::input::get_right_analog(0, &right_x, &right_y);
    auto value = vertical != 0 ? right_y : right_x;
    if (!std::isfinite(value) || value == 0.0f) return original_value;
    const bool inverted = vertical != 0 ? tooie::camera::vertical_inverted()
                                        : tooie::camera::horizontal_inverted();
    return inverted ? -value : value;
#endif
}

extern "C" int tooie_camera_analog_apply(std::uint8_t* rdram, recomp_context* ctx) {
#ifndef TOOIE_NATIVE_HOST
    (void)rdram;
    (void)ctx;
    return -1;
#else
    if (!tooie::camera::analog_enabled() || tooie::input::game_input_disabled()) {
        return -1;
    }

    // This hook is placed after func_800A4878's original eligibility gates.
    // Its 0x24($sp) slot holds the active camera component saved at 0x800A48BC.
    const auto component = MEM_W(ctx->r29, 0x24);
    if (component == 0) {
        return -1;
    }

    // func_80110EFC, used by the original C-left/C-right paths, first admits
    // only components whose mode's table entry has a nonzero +0xC field.
    // Keep that mode gate before reproducing its direction-specific route below.
    const auto mode = static_cast<std::uint32_t>(MEM_W(component, 0x6C));
    const auto mode_rule_address = static_cast<gpr>(static_cast<std::int32_t>(
        k_camera_mode_rule_table + mode * 16u));
    if (MEM_H(0, mode_rule_address) == 0) {
        return -1;
    }

    float right_x = 0.0f;
    float right_y = 0.0f;
    tooie::input::get_right_analog(0, &right_x, &right_y);
    // get_right_analog already applies the user's configured joystick deadzone.
    auto horizontal = right_x;
    if (!std::isfinite(horizontal) || horizontal == 0.0f) {
        return -1;
    }
    if (tooie::camera::horizontal_inverted()) {
        horizontal=-horizontal;
    }

    // The original C-direction branches enter through func_80114D98 before
    // func_801111D0. Preserve that standard-mode pre-dispatch, then reuse the
    // downstream target/interpolation path with a proportional offset.
    // A generated guest call may scratch any caller-saved register. Preserve
    // the complete translation context while retaining its intended RDRAM
    // writes, then resume the original generated instruction stream exactly.
    const auto saved_context=*ctx;
    try {
        ctx->r4=component;
        // The vanilla C-left branch supplies command 0 / -45 degrees; C-right
        // supplies command 1 / +45 degrees. Keep its matching transient flag.
        ctx->r5=horizontal < 0.0f ? 0 : 1;
        func_80114D98(rdram, ctx);
        ctx->r4=component;
        ctx->r5=std::bit_cast<std::uint32_t>(horizontal*k_full_turn_target_degrees);
        func_801111D0(rdram, ctx);
        const auto result = static_cast<int>(static_cast<std::int32_t>(ctx->r2));
        *ctx=saved_context;
        return result;
    } catch (...) {
        // Generated guest calls may cooperatively unwind the current thread.
        // Restore all transient registers before the runtime observes it.
        *ctx=saved_context;
        throw;
    }
#endif
}
