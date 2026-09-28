#include "free_camera.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {
unsigned finalize_calls = 0;

gpr address(std::uint32_t value) {
    return static_cast<gpr>(static_cast<std::int32_t>(value));
}

void store_float(std::uint8_t* rdram, gpr base, std::uint32_t offset, float value) {
    MEM_W(offset, base) = std::bit_cast<std::uint32_t>(value);
}

float load_float(std::uint8_t* rdram, gpr base, std::uint32_t offset) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(MEM_W(offset, base)));
}
}

extern "C" void func_800CAF34(std::uint8_t*, recomp_context* ctx) {
    ++finalize_calls;
    *ctx = {};
    ctx->r2 = 0xDEADBEEF;
}

int main() {
    std::vector<std::uint8_t> storage(8 * 1024 * 1024);
    auto* rdram = storage.data();
    const auto camera = address(0x80010000U);
    MEM_W(0, address(0x8012D500U)) = static_cast<std::int32_t>(camera);
    MEM_B(0, address(0x8012762CU)) = 0;
    MEM_B(0, address(0x8012B3F1U)) = 0;
    store_float(rdram, camera, 0, 100.0f);
    store_float(rdram, camera, 4, 200.0f);
    store_float(rdram, camera, 8, 300.0f);
    store_float(rdram, camera, 0x18, -30.0f);
    store_float(rdram, camera, 0x1C, 30.0f);
    store_float(rdram, camera, 0x20, 7.0f);

    recomp_context ctx{};
    ctx.r1 = 0x11111111;
    ctx.r31 = 0x31313131;
    ctx.f12.u64 = 0x1212121212121212ULL;
    ctx.hi = 0xAAAAAAAAAAAAAAAAULL;
    ctx.lo = 0xBBBBBBBBBBBBBBBBULL;
    const recomp_context expected = ctx;

    tooie::camera::set_free_camera_enabled(true);
    tooie::camera::set_free_camera_input_active(true);
    tooie::camera::set_free_camera_speed(600.0f);
    tooie::camera::set_free_camera_motion(0.0f, 0.0f, 1.0f);

    // First frame resets the real-time integrator and must remain stationary.
    tooie_free_camera_begin(rdram, &ctx);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    tooie_free_camera_end(rdram, &ctx);
    assert(load_float(rdram, camera, 0) == 100.0f);
    assert(load_float(rdram, camera, 4) == 200.0f);
    assert(load_float(rdram, camera, 8) == 300.0f);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tooie_free_camera_begin(rdram, &ctx);
    // With pitch=-30/yaw=30, forward motion has positive X/Y/Z components.
    assert(load_float(rdram, camera, 0) > 100.0f);
    assert(load_float(rdram, camera, 4) > 200.0f);
    assert(load_float(rdram, camera, 8) > 300.0f);
    // Euler angles are game-owned and never treated as a look-at target.
    assert(load_float(rdram, camera, 0x18) == -30.0f);
    assert(load_float(rdram, camera, 0x1C) == 30.0f);
    assert(load_float(rdram, camera, 0x20) == 7.0f);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    tooie_free_camera_end(rdram, &ctx);
    assert(load_float(rdram, camera, 0) == 100.0f);
    assert(load_float(rdram, camera, 4) == 200.0f);
    assert(load_float(rdram, camera, 8) == 300.0f);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    assert(finalize_calls == 4);

    // A detached offset remains active after movement stops until the player
    // explicitly recenters it.
    tooie::camera::set_free_camera_motion(0.0f, 0.0f, 0.0f);
    tooie_free_camera_begin(rdram, &ctx);
    assert(load_float(rdram, camera, 0) > 100.0f);
    assert(load_float(rdram, camera, 4) > 200.0f);
    assert(load_float(rdram, camera, 8) > 300.0f);
    const auto moved_snapshot = tooie::camera::free_camera_debug_snapshot();
    assert(moved_snapshot.enabled);
    assert(moved_snapshot.input_active);
    assert(moved_snapshot.offset[0] > 0.0f);
    assert(moved_snapshot.offset[1] > 0.0f);
    assert(moved_snapshot.offset[2] > 0.0f);
    tooie_free_camera_end(rdram, &ctx);

    tooie::camera::request_free_camera_reset();
    tooie_free_camera_begin(rdram, &ctx);
    assert(load_float(rdram, camera, 0) == 100.0f);
    assert(load_float(rdram, camera, 4) == 200.0f);
    assert(load_float(rdram, camera, 8) == 300.0f);
    assert(load_float(rdram, camera, 0x18) == -30.0f);
    assert(load_float(rdram, camera, 0x1C) == 30.0f);
    const auto reset_snapshot = tooie::camera::free_camera_debug_snapshot();
    assert((reset_snapshot.offset == std::array<float, 3>{}));
    tooie_free_camera_end(rdram, &ctx);

    // Focus/UI suspension clears the detached offset and leaves guest camera
    // state frozen even if stale motion remains published.
    tooie::camera::set_free_camera_input_active(false);
    tooie_free_camera_begin(rdram, &ctx);
    tooie_free_camera_end(rdram, &ctx);
    assert(load_float(rdram, camera, 0) == 100.0f);
    assert(load_float(rdram, camera, 4) == 200.0f);
    assert(load_float(rdram, camera, 8) == 300.0f);
    assert(finalize_calls == 8);
    return 0;
}
