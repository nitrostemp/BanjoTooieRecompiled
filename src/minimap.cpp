#include "minimap.hpp"

#include "scene_observer.hpp"
#include "practice_state.hpp"
#ifdef TOOIE_NATIVE_HOST
#include "practice_travel.hpp"
#include "practice_forms.hpp"
#endif

#ifdef TOOIE_NATIVE_HOST
#include "imgui/imgui.h"
#endif
#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

#ifndef TOOIE_NATIVE_HOST
// Generated guest selectors are present in all builds. Headless targets do
// not include the native practice-form action, so preserve original values.
extern "C" void tooie_practice_form_restore_override(std::uint8_t*, recomp_context*) noexcept {}
extern "C" void tooie_practice_form_basetup_override(std::uint8_t*, recomp_context*) noexcept {}
extern "C" void tooie_practice_form_basetup_final_override(std::uint8_t*, recomp_context*) noexcept {}
#endif

namespace {
// Match the runtime's 8 MiB RDRAM extent and the repository's existing live
// fixture guard: the first 0x400 bytes are exception/boot vectors, not heap.
constexpr std::uint32_t rdram_first = 0x80000400U;
constexpr std::uint32_t rdram_end = 0x80800000U;
#ifdef TOOIE_NATIVE_HOST
constexpr int radar_size = 152;
constexpr int radar_radius_pixels = radar_size / 2 - 7;
#endif

std::atomic_uint configured_mode{static_cast<unsigned>(tooie::minimap::Mode::Off)};
std::atomic_uint configured_corner{static_cast<unsigned>(tooie::minimap::Corner::BottomLeft)};
std::atomic_uint configured_heading{static_cast<unsigned>(tooie::minimap::Heading::Camera)};
tooie::minimap::TrailState trail;

bool guest_range(std::uint32_t address, std::uint32_t bytes) noexcept {
    return address >= rdram_first && bytes <= rdram_end - rdram_first &&
        address <= rdram_end - bytes && (address & 3U) == 0;
}

std::uint32_t guest_word(std::uint8_t* rdram, std::uint32_t address) noexcept {
    return static_cast<std::uint32_t>(MEM_W(0,
        static_cast<gpr>(static_cast<std::int32_t>(address))));
}

std::uint8_t guest_byte(std::uint8_t* rdram, std::uint32_t address,
    std::uint32_t offset) noexcept {
    return static_cast<std::uint8_t>(MEM_BU(
        static_cast<gpr>(static_cast<std::int32_t>(address)), offset));
}

float guest_float(std::uint8_t* rdram, std::uint32_t address) noexcept {
    return std::bit_cast<float>(guest_word(rdram, address));
}

}

namespace tooie::minimap {

void set_mode(Mode value) noexcept {
    const auto previous = configured_mode.exchange(
        static_cast<unsigned>(value), std::memory_order_acq_rel);
    if (value == Mode::Off && previous != static_cast<unsigned>(Mode::Off)) {
        trail.reset();
    }
}

void set_corner(Corner value) noexcept {
    configured_corner.store(static_cast<unsigned>(value), std::memory_order_release);
}

Mode mode() noexcept {
    return static_cast<Mode>(configured_mode.load(std::memory_order_acquire));
}

Corner corner() noexcept {
    return static_cast<Corner>(configured_corner.load(std::memory_order_acquire));
}

bool visible() noexcept { return mode() == Mode::Trail; }
TrailSnapshot trail_snapshot() noexcept { return trail.snapshot(); }

void render(int width, int height) {
#ifdef TOOIE_NATIVE_HOST
    if (!visible()) return;
    const auto scene = tooie::scene::snapshot();
    const auto state = trail.snapshot();
    const bool stale = !scene.map_available || scene.activation_active ||
        (state.current && state.map_id != scene.map_id);
    if (stale || !state.current) {
        if (stale && (state.current || state.count != 0)) trail.reset();
        return;
    }
    const float scale = std::clamp(static_cast<float>(height) / 720.0f, 0.8f, 1.8f);
    const float size = radar_size * scale;
    const float radius = radar_radius_pixels * scale;
    const float left = corner() == Corner::BottomRight ? width - size - 22.0f : 22.0f;
    const ImVec2 center(left + size * 0.5f, height - size * 0.5f - 22.0f);
    auto* draw = ImGui::GetBackgroundDrawList();
    draw->AddCircleFilled(center, size * 0.5f, IM_COL32(13, 24, 32, 220), 64);
    draw->AddCircle(center, size * 0.5f, IM_COL32(190, 207, 218, 160), 64);
    draw->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(190, 207, 218, 40));
    draw->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(190, 207, 218, 40));
    const bool camera_up = heading() == Heading::Camera && state.camera_heading_available;
    const float yaw = camera_up ? state.camera_yaw_degrees : state.yaw_degrees;
    for (std::size_t index = 0; index < state.count; ++index) {
        const auto point = project_heading_up(state.position, state.points[index], yaw);
        if (!point.visible) continue;
        const int alpha = 64 + static_cast<int>(166 * (index + 1) / state.count);
        draw->AddCircleFilled(ImVec2(center.x + point.x * radius, center.y + point.y * radius),
            2.2f * scale, IM_COL32(121, 217, 255, alpha));
    }
    const float angle = std::remainder(state.yaw_degrees - yaw, 360.0f) * 0.01745329252f;
    const auto rotated = [&](float x, float y) {
        return ImVec2(center.x + (x * std::cos(angle) - y * std::sin(angle)) * scale,
            center.y + (x * std::sin(angle) + y * std::cos(angle)) * scale);
    };
    draw->AddTriangleFilled(rotated(0, -9), rotated(-6, 6), rotated(6, 6), IM_COL32(255, 224, 107, 255));
    draw->AddText(ImVec2(left, center.y - size * 0.5f - ImGui::GetFontSize() - 4),
        IM_COL32(245, 247, 250, 255), camera_up ? "Trail / Camera" : "Trail / Banjo");
#else
    (void)width;
    (void)height;
#endif
}

void shutdown() noexcept {
    trail.reset();
}

} // namespace tooie::minimap

void tooie::minimap::set_heading(Heading value) noexcept {
    configured_heading.store(static_cast<unsigned>(value == Heading::Character ? value : Heading::Camera));
}
tooie::minimap::Heading tooie::minimap::heading() noexcept {
    return static_cast<Heading>(configured_heading.load());
}

extern "C" void tooie_minimap_tick(std::uint8_t* rdram,
    recomp_context* ctx) {
    if (!rdram || !ctx) return;
#ifdef TOOIE_NATIVE_HOST
    // Transition owners need guest-thread delivery even while PlayerState is
    // temporarily absent during a reload or area activation.
    tooie::practice::forms::tick(rdram, ctx);
    tooie::practice::travel::tick(rdram, ctx);
#endif
    const auto scene = tooie::scene::snapshot();
    tooie::practice::Sample practice{};
    const auto player = static_cast<std::uint32_t>(ctx->r4);
    // bainput_update can run once per character. The controlled index and the
    // two-element PlayerState* table are source-defined in core2/1ECE0B0.c
    // and core2bss2.s; never combine two characters' motion into one delta.
    const auto controlled_index = guest_word(rdram, 0x801354DCU);
    if (controlled_index >= 2U) {
        tooie::practice::observe(practice);
        if (tooie::minimap::visible()) trail.observe(false, 0, scene.activation_active, {}, 0.0f);
        return;
    }
    const auto controlled_player = guest_word(rdram, 0x80135490U + controlled_index * 4U);
    if (player != controlled_player) return;
    practice.player_address = player;
    if (!guest_range(player, 0x12CU)) {
        tooie::practice::observe(practice);
        if (tooie::minimap::visible()) trail.observe(false, 0, scene.activation_active, {}, 0.0f);
        return;
    }

    // Tooie's func_8009C128 reads PlayerState+0xE4 and copies that vec3 as
    // the player position. playerstate.h identifies +0xF8 as BaYaw*, whose
    // first field is the value returned by the original yaw_get routine.
    const auto position = guest_word(rdram, player + 0xE4U);
    const auto yaw = guest_word(rdram, player + 0xF8U);
    if (!guest_range(position, 12U) || !guest_range(yaw, 4U)) {
        tooie::practice::observe(practice);
        if (tooie::minimap::visible()) trail.observe(false, 0, scene.activation_active, {}, 0.0f);
        return;
    }
    // Same camera owner/Euler-yaw layout used by the existing free-camera hook.
    // Read only on this guest observation thread, with the normal RDRAM guards.
    const auto camera = guest_word(rdram, 0x8012D500U);
    const float camera_yaw = guest_range(camera, 0x20U)
        ? guest_float(rdram, camera + 0x1CU) : std::numeric_limits<float>::quiet_NaN();
    practice.valid = scene.map_available && !scene.activation_active;
    practice.map_id = scene.map_id;
    practice.position = {guest_float(rdram, position), guest_float(rdram, position + 4U),
        guest_float(rdram, position + 8U)};
    practice.facing_degrees = guest_float(rdram, yaw);
    practice.camera_valid = guest_range(camera, 0x20U);
    if (practice.camera_valid) {
        practice.camera_pitch_degrees = guest_float(rdram, camera + 0x18U);
        practice.camera_yaw_degrees = camera_yaw;
        practice.camera_valid = std::isfinite(practice.camera_pitch_degrees) && std::isfinite(camera_yaw);
    }
    // These fields are read at bainput_update entry, so they describe the
    // latest input state already consumed by the previous guest update.
    const auto stick = guest_word(rdram, player + 0x128U);
    practice.stick_valid = guest_range(stick, 0x5CU);
    if (practice.stick_valid) {
        practice.stick_x = guest_float(rdram, stick + 0x54U);
        practice.stick_y = guest_float(rdram, stick + 0x58U);
        practice.stick_valid = std::isfinite(practice.stick_x) && std::isfinite(practice.stick_y);
    }
    const auto key = guest_word(rdram, player + 0x40U);
    practice.buttons_valid = guest_range(key, 0x28U);
    if (practice.buttons_valid) {
        // BaKey::prev_state is a suppression latch, not button input.
        // func_800BCE84 instead selects a ButtonData ring entry through
        // unkfunc_800BCE84::unk8[id] and reads the low pressed bit. Match
        // bakey_held by applying the suppression latch after that read.
        const auto data = guest_word(rdram, key);
        practice.buttons_valid = guest_range(data, 0x24U);
        const auto ring_count = practice.buttons_valid ? guest_byte(rdram, data, 4U) : 0U;
        practice.buttons_valid = practice.buttons_valid && ring_count >= 1U && ring_count <= 4U &&
            guest_range(data, 0x24U + ring_count * 0x38U + 8U);
        for (std::uint32_t button = 0; button < 14; ++button) {
            if (!practice.buttons_valid) break;
            const auto index = guest_byte(rdram, data, 8U + button);
            if (index >= ring_count) { practice.buttons_valid = false; break; }
            const auto word = guest_word(rdram, data + 0x24U + index * 0x38U + button * 4U);
            if ((word & 1U) != 0U && guest_byte(rdram, key, 0x18U + button) == 0U)
                practice.held_buttons |= static_cast<std::uint16_t>(1U << button);
        }
        if (!practice.buttons_valid) practice.held_buttons = 0;
    }
    const auto physics = guest_word(rdram, player + 0xC8U);
    practice.vertical_velocity_valid = guest_range(physics, 0x18U);
    if (practice.vertical_velocity_valid) {
        practice.vertical_velocity = guest_float(rdram, physics + 0x14U);
        practice.vertical_velocity_valid = std::isfinite(practice.vertical_velocity);
    }
    tooie::practice::observe(practice);
    if (tooie::minimap::visible()) trail.observe(practice.valid, scene.map_id, scene.activation_active,
        {practice.position.x, practice.position.y, practice.position.z}, practice.facing_degrees, camera_yaw);
}

extern "C" bool tooie_minimap_visible() noexcept {
    return tooie::minimap::visible();
}

extern "C" void tooie_minimap_render(int width, int height) {
    tooie::minimap::render(width, height);
}

extern "C" void tooie_minimap_shutdown() noexcept {
    tooie::minimap::shutdown();
}
