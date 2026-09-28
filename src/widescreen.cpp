#include "widescreen.hpp"

#include "boot_hooks.h"

#include <atomic>

namespace {
std::atomic_bool widescreen_profile_override_latched{false};
std::atomic_bool configured_widescreen_enabled{false};
std::atomic_bool latched_widescreen_enabled{false};
std::atomic<tooie::widescreen::NativeAspect> configured_native_aspect_value{
    tooie::widescreen::NativeAspect::Ratio16x9};
std::atomic<tooie::widescreen::NativeAspect> latched_native_aspect_value{
    tooie::widescreen::NativeAspect::Ratio16x9};

constexpr tooie::widescreen::NativeAspect normalize_aspect(
    tooie::widescreen::NativeAspect aspect) noexcept {
    switch (aspect) {
        case tooie::widescreen::NativeAspect::Ratio16x9:
        case tooie::widescreen::NativeAspect::Ratio21x9:
        case tooie::widescreen::NativeAspect::Ratio32x9:
        case tooie::widescreen::NativeAspect::Ratio43x18:
            return aspect;
        default:
            return tooie::widescreen::NativeAspect::Ratio16x9;
    }
}

constexpr double aspect_ratio(tooie::widescreen::NativeAspect aspect) noexcept {
    switch (normalize_aspect(aspect)) {
        case tooie::widescreen::NativeAspect::Ratio21x9: return 21.0 / 9.0;
        case tooie::widescreen::NativeAspect::Ratio32x9: return 32.0 / 9.0;
        case tooie::widescreen::NativeAspect::Ratio43x18: return 43.0 / 18.0;
        default: return 16.0 / 9.0;
    }
}
}

namespace tooie::widescreen {

void configure_profile(bool enabled) noexcept {
    configured_widescreen_enabled.store(enabled, std::memory_order_release);
}

void configure_native_aspect(NativeAspect aspect) noexcept {
    configured_native_aspect_value.store(normalize_aspect(aspect), std::memory_order_release);
}

void latch_for_game_start() noexcept {
    latched_widescreen_enabled.store(
        configured_widescreen_enabled.load(std::memory_order_acquire),
        std::memory_order_release);
    latched_native_aspect_value.store(
        configured_native_aspect_value.load(std::memory_order_acquire),
        std::memory_order_release);
    widescreen_profile_override_latched.store(true, std::memory_order_release);
}

NativeAspect configured_native_aspect() noexcept {
    return configured_native_aspect_value.load(std::memory_order_acquire);
}

NativeAspect latched_native_aspect() noexcept {
    return latched_native_aspect_value.load(std::memory_order_acquire);
}

double latched_aspect_ratio() noexcept {
    return aspect_ratio(latched_native_aspect());
}

bool latched_enabled() noexcept {
    return latched_widescreen_enabled.load(std::memory_order_acquire);
}

bool profile_override_active() noexcept {
    return widescreen_profile_override_latched.load(std::memory_order_acquire);
}

uint32_t resolve_global_setting(uint32_t game_value) noexcept {
    if (!profile_override_active()) return game_value;
    return latched_enabled() ? 1U : 0U;
}

float adjust_projection_aspect(float original_aspect, bool game_widescreen) noexcept {
    if (!game_widescreen || !latched_enabled()) return original_aspect;
    if (latched_native_aspect() == NativeAspect::Ratio16x9) return original_aspect;
    constexpr double original_native_aspect = 16.0 / 9.0;
    return static_cast<float>(original_aspect *
        (latched_aspect_ratio() / original_native_aspect));
}

} // namespace tooie::widescreen

extern "C" void tooie_widescreen_apply_profile(recomp_context* ctx) {
    // The following call remains the game-owned lifecycle: overlay 699 invokes
    // set_widescreen(a0) and then its own GFLAG_BB9 setter.
    ctx->r4 = tooie::widescreen::resolve_global_setting(ctx->r4);
}

extern "C" float tooie_widescreen_adjust_projection_aspect(
    float original_aspect, int game_widescreen) {
    return tooie::widescreen::adjust_projection_aspect(
        original_aspect, game_widescreen != 0);
}
