#include "free_camera.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>

#include "funcs.h"

namespace {
constexpr std::uint32_t k_camera_owner = 0x8012D500U;
constexpr std::uint32_t k_game_mode = 0x8012762CU;
constexpr std::uint32_t k_active_slot = 0x8012B3F1U;
constexpr float k_default_speed = 600.0f;

std::atomic_bool enabled{false};
std::atomic_bool input_active{false};
std::atomic<std::uint32_t> motion_right{std::bit_cast<std::uint32_t>(0.0f)};
std::atomic<std::uint32_t> motion_up{std::bit_cast<std::uint32_t>(0.0f)};
std::atomic<std::uint32_t> motion_forward{std::bit_cast<std::uint32_t>(0.0f)};
std::atomic<std::uint32_t> speed_bits{std::bit_cast<std::uint32_t>(k_default_speed)};
std::atomic<std::uint64_t> configuration_epoch{0};
std::array<std::atomic<std::uint32_t>, 3> published_offset{
    std::atomic<std::uint32_t>{std::bit_cast<std::uint32_t>(0.0f)},
    std::atomic<std::uint32_t>{std::bit_cast<std::uint32_t>(0.0f)},
    std::atomic<std::uint32_t>{std::bit_cast<std::uint32_t>(0.0f)},
};

struct GuestState {
    std::array<float, 3> offset{};
    std::array<std::uint32_t, 3> saved_words{};
    std::uint32_t camera = 0;
    std::int32_t slot = -1;
    std::uint64_t epoch = 0;
    std::chrono::steady_clock::time_point last_time{};
    bool restore_pending = false;
};
GuestState guest;

gpr guest_address(std::uint32_t address) noexcept {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}

bool valid_player_file(std::uint8_t* rdram, std::int32_t& slot) noexcept {
    const auto mode = MEM_BU(0, guest_address(k_game_mode));
    const bool supported_mode = mode < 0x0FU || (mode < 0x1CU && mode != 0x11U);
    slot = MEM_B(0, guest_address(k_active_slot));
    return supported_mode && slot >= 0;
}

float load_float(std::uint8_t* rdram, gpr base, std::uint32_t offset) noexcept {
    return std::bit_cast<float>(static_cast<std::uint32_t>(MEM_W(offset, base)));
}

void store_float(std::uint8_t* rdram, gpr base, std::uint32_t offset, float value) noexcept {
    MEM_W(offset, base) = std::bit_cast<std::uint32_t>(value);
}

void restore_camera(std::uint8_t* rdram) noexcept {
    if (!guest.restore_pending) return;
    const auto camera = guest_address(guest.camera);
    constexpr std::array<std::uint32_t, 3> offsets{0, 4, 8};
    for (std::size_t i = 0; i < offsets.size(); ++i)
        MEM_W(offsets[i], camera) = guest.saved_words[i];
    guest.restore_pending = false;
}

void reset_detached_state(std::uint64_t epoch, std::int32_t slot) noexcept {
    guest.offset = {};
    for (auto& axis : published_offset)
        axis.store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_release);
    guest.slot = slot;
    guest.epoch = epoch;
    guest.last_time = std::chrono::steady_clock::now();
}

void rebuild_camera(std::uint8_t* rdram, recomp_context* ctx, std::uint32_t camera) {
    const auto saved_context = *ctx;
    try {
        ctx->r4 = guest_address(camera);
        func_800CAF34(rdram, ctx);
        *ctx = saved_context;
    } catch (...) {
        *ctx = saved_context;
        throw;
    }
}
} // namespace

void tooie::camera::set_free_camera_enabled(bool value) noexcept {
    if (enabled.exchange(value, std::memory_order_acq_rel) != value)
        configuration_epoch.fetch_add(1, std::memory_order_acq_rel);
}

bool tooie::camera::free_camera_enabled() noexcept {
    return enabled.load(std::memory_order_acquire);
}

void tooie::camera::set_free_camera_input_active(bool value) noexcept {
    if (input_active.exchange(value, std::memory_order_acq_rel) != value)
        configuration_epoch.fetch_add(1, std::memory_order_acq_rel);
}

void tooie::camera::set_free_camera_motion(float right, float up, float forward) noexcept {
    const auto finite_or_zero = [](float value) { return std::isfinite(value) ? value : 0.0f; };
    motion_right.store(std::bit_cast<std::uint32_t>(std::clamp(finite_or_zero(right), -1.0f, 1.0f)),
        std::memory_order_release);
    motion_up.store(std::bit_cast<std::uint32_t>(std::clamp(finite_or_zero(up), -1.0f, 1.0f)),
        std::memory_order_release);
    motion_forward.store(std::bit_cast<std::uint32_t>(std::clamp(finite_or_zero(forward), -1.0f, 1.0f)),
        std::memory_order_release);
}

void tooie::camera::request_free_camera_reset() noexcept {
    configuration_epoch.fetch_add(1, std::memory_order_acq_rel);
}

tooie::camera::FreeCameraDebugSnapshot tooie::camera::free_camera_debug_snapshot() noexcept {
    FreeCameraDebugSnapshot snapshot;
    for (std::size_t axis = 0; axis < snapshot.offset.size(); ++axis)
        snapshot.offset[axis] = std::bit_cast<float>(
            published_offset[axis].load(std::memory_order_acquire));
    snapshot.enabled = enabled.load(std::memory_order_acquire);
    snapshot.input_active = input_active.load(std::memory_order_acquire);
    return snapshot;
}

void tooie::camera::set_free_camera_speed(float value) noexcept {
    if (!std::isfinite(value)) value = k_default_speed;
    speed_bits.store(std::bit_cast<std::uint32_t>(std::clamp(value, 50.0f, 3000.0f)),
        std::memory_order_release);
}

float tooie::camera::free_camera_speed() noexcept {
    return std::bit_cast<float>(speed_bits.load(std::memory_order_acquire));
}

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool tooie::camera::persistent_neutral() noexcept {
    if (enabled.load(std::memory_order_acquire) ||
        input_active.load(std::memory_order_acquire) || guest.restore_pending)
        return false;
    return std::all_of(guest.offset.begin(), guest.offset.end(),
        [](float value) { return value == 0.0f; });
}

void tooie::camera::persistent_reset_epoch() noexcept {
    // The old saved_words belong to newer guest RAM. Never call
    // restore_camera() after replacement: that would corrupt the image.
    guest = {};
    guest.slot = -1;
    input_active.store(false, std::memory_order_release);
    motion_right.store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_release);
    motion_up.store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_release);
    motion_forward.store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_release);
    for (auto& axis : published_offset)
        axis.store(std::bit_cast<std::uint32_t>(0.0f), std::memory_order_release);
    configuration_epoch.fetch_add(1, std::memory_order_acq_rel);
}
#endif

extern "C" void tooie_free_camera_begin(std::uint8_t* rdram, recomp_context* ctx) {
    // A previous frame can only remain pending if a transformed function
    // exited abnormally. Restore first so offsets never accumulate in RDRAM.
    restore_camera(rdram);

    std::int32_t slot = -1;
    const auto epoch = configuration_epoch.load(std::memory_order_acquire);
    if (!valid_player_file(rdram, slot) || !enabled.load(std::memory_order_acquire) ||
        !input_active.load(std::memory_order_acquire)) {
        reset_detached_state(epoch, slot);
        return;
    }
    if (slot != guest.slot || epoch != guest.epoch)
        reset_detached_state(epoch, slot);

    const auto camera = static_cast<std::uint32_t>(MEM_W(0, guest_address(k_camera_owner)));
    if (camera < 0x80000000U || camera > 0x807FFF00U) return;
    const auto camera_address = guest_address(camera);
    constexpr std::array<std::uint32_t, 3> offsets{0, 4, 8};
    for (std::size_t i = 0; i < offsets.size(); ++i)
        guest.saved_words[i] = static_cast<std::uint32_t>(MEM_W(offsets[i], camera_address));

    std::array<float, 3> eye{load_float(rdram, camera_address, 0), load_float(rdram, camera_address, 4),
                             load_float(rdram, camera_address, 8)};
    // CA688/E3C58 identify +0x18 as the original Euler-angle vector (the
    // initialization path writes -30, +30, 0). It is direction, not a look-at
    // point, and must never receive the translation offset.
    const float pitch_degrees = load_float(rdram, camera_address, 0x18);
    const float yaw_degrees = load_float(rdram, camera_address, 0x1C);
    if (!std::all_of(eye.begin(), eye.end(), [](float v) { return std::isfinite(v); }) ||
        !std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees)) return;

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - guest.last_time).count(), 0.0f, 1.0f / 15.0f);
    guest.last_time = now;
    constexpr float degrees_to_radians = 3.14159265358979323846f / 180.0f;
    const float pitch = pitch_degrees * degrees_to_radians;
    const float yaw = yaw_degrees * degrees_to_radians;
    const std::array<float, 3> forward{
        std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const std::array<float, 3> right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const float right_input = std::bit_cast<float>(motion_right.load(std::memory_order_acquire));
    const float up_input = std::bit_cast<float>(motion_up.load(std::memory_order_acquire));
    const float forward_input = std::bit_cast<float>(motion_forward.load(std::memory_order_acquire));
    std::array<float, 3> direction{
        right[0] * right_input + forward[0] * forward_input,
        up_input + forward[1] * forward_input,
        right[2] * right_input + forward[2] * forward_input,
    };
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length > 1.0f)
        for (auto& value : direction) value /= length;
    const float distance = tooie::camera::free_camera_speed() * dt;
    for (std::size_t i = 0; i < guest.offset.size(); ++i)
        guest.offset[i] += direction[i] * distance;
    for (std::size_t axis = 0; axis < guest.offset.size(); ++axis)
        published_offset[axis].store(std::bit_cast<std::uint32_t>(guest.offset[axis]),
            std::memory_order_release);

    for (std::size_t axis = 0; axis < 3; ++axis)
        store_float(rdram, camera_address, static_cast<std::uint32_t>(axis * 4), eye[axis] + guest.offset[axis]);
    guest.camera = camera;
    guest.restore_pending = true;
    try {
        // CAF34 is the original finalizer used after the position/angle
        // setters. It rebuilds the view basis and CPU frustum for the detached
        // position before the world draw begins.
        rebuild_camera(rdram, ctx, camera);
    } catch (...) {
        restore_camera(rdram);
        throw;
    }
}

extern "C" void tooie_free_camera_end(std::uint8_t* rdram, recomp_context* ctx) {
    const auto camera = guest.camera;
    const bool had_offset = guest.restore_pending;
    restore_camera(rdram);
    if (had_offset)
        rebuild_camera(rdram, ctx, camera);
}
