#include "camera_analog.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "funcs.h"
#include "recomp.h"
#include "frontend_input_preference.hpp"

namespace {
float right_x = 0.0f;
float right_y = 0.0f;
float expected_target_x = 0.0f;
bool game_input_is_disabled = false;
bool throw_from_target = false;
gpr expected_component = 0;
gpr expected_pre_dispatch_direction = 0;
int pre_dispatch_calls = 0;
int target_calls = 0;
int target_result = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

gpr guest_address(std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}

void reset_stubs() {
    right_x = 0.0f;
    right_y = 0.0f;
    expected_target_x = 0.0f;
    game_input_is_disabled = false;
    pre_dispatch_calls = 0;
    target_calls = 0;
    target_result = 0;
    throw_from_target = false;
}
} // namespace

namespace tooie::input {
void get_right_analog(int, float* x, float* y) noexcept {
    *x = right_x;
    *y = right_y;
}

bool game_input_disabled() noexcept {
    return game_input_is_disabled;
}

} // namespace tooie::input

void func_80114D98(std::uint8_t*, recomp_context* ctx) {
    ++pre_dispatch_calls;
    require(ctx->r4 == expected_component && ctx->r5 == expected_pre_dispatch_direction,
            "pre-dispatch must receive the matching original C-direction argument");
    ctx->r16 = 0x1111111111111111ull;
    ctx->f0.u64 = 0x2222222222222222ull;
}

void func_801111D0(std::uint8_t*, recomp_context* ctx) {
    ++target_calls;
    require(ctx->r4 == expected_component,
            "target command must retain the active camera component");
    require(std::fabs(std::bit_cast<float>(static_cast<std::uint32_t>(ctx->r5)) -
                      expected_target_x * 45.0f) < 0.0001f,
            "target command must receive proportional horizontal input");
    ctx->r17 = 0x3333333333333333ull;
    ctx->f1.u64 = 0x4444444444444444ull;
    if (throw_from_target) throw std::runtime_error("cooperative guest unwind");
    ctx->r2 = static_cast<gpr>(static_cast<std::int32_t>(target_result));
}

int main() {
    try {
        std::vector<std::uint8_t> storage(0x130000);
        auto* rdram = storage.data();
        recomp_context ctx{};
        std::memset(&ctx, 0x5A, sizeof(ctx));
        ctx.r29 = guest_address(0x80020000u);
        expected_component = guest_address(0x80010000u);
        MEM_W(0x24, ctx.r29) = static_cast<std::int32_t>(expected_component);
        MEM_W(0x6C, expected_component) = 2;
        const auto rule = guest_address(0x801247ACu + 2u * 16u);
        MEM_H(0, rule) = 1;

        tooie::camera::set_analog_enabled(true);
        tooie::camera::set_horizontal_inverted(false);
        tooie::camera::set_vertical_inverted(false);

        reset_stubs();
        right_x = 0.25f;
        right_y = -0.75f;
        require(std::fabs(tooie_camera_first_person_axis(0.1f, 0) - 0.25f) < 0.0001f,
                "first-person yaw must use the right stick inside the original controller path");
        require(std::fabs(tooie_camera_first_person_axis(0.1f, 1) + 0.75f) < 0.0001f,
                "first-person pitch must use the right stick inside the original controller path");
        tooie::camera::set_horizontal_inverted(true);
        tooie::camera::set_vertical_inverted(true);
        require(std::fabs(tooie_camera_first_person_axis(0.1f, 0) + 0.25f) < 0.0001f,
                "first-person yaw inversion must be independent");
        require(std::fabs(tooie_camera_first_person_axis(0.1f, 1) - 0.75f) < 0.0001f,
                "first-person pitch inversion must be independent");
        tooie::camera::set_horizontal_inverted(false);
        tooie::camera::set_vertical_inverted(false);
        right_x = 0.0f;
        right_y = 0.0f;
        require(std::fabs(tooie_camera_first_person_axis(0.1f, 0) - 0.1f) < 0.0001f &&
                std::fabs(tooie_camera_first_person_axis(-0.2f, 1) + 0.2f) < 0.0001f,
                "zero right-stick axes must preserve Tooie's original left-stick values");

        reset_stubs();
        right_x = 0.5f;
        expected_target_x = 0.5f;
        expected_pre_dispatch_direction = 1;
        target_result = 0;
        const auto original_context = ctx;
        require(tooie_camera_analog_apply(rdram, &ctx) == 0,
                "an eligible collision result must remain zero after consumption");
        require(pre_dispatch_calls == 1 && target_calls == 1,
                "eligible input must use the matching original C-direction route once");
        require(std::memcmp(&ctx, &original_context, sizeof(ctx)) == 0,
                "guest scratch registers must not escape the hook");

        reset_stubs();
        right_x = 0.5f;
        expected_target_x = -0.5f;
        expected_pre_dispatch_direction = 0;
        target_result = 1;
        tooie::camera::set_horizontal_inverted(true);
        require(tooie_camera_analog_apply(rdram, &ctx) == 1,
                "inverted input must preserve the original guest result");
        require(pre_dispatch_calls == 1 && target_calls == 1,
                "inverted input must still use the matching C-direction route");
        require(std::memcmp(&ctx, &original_context, sizeof(ctx)) == 0,
                "inverted guest calls must restore the full context");
        tooie::camera::set_horizontal_inverted(false);

        reset_stubs();
        right_x = 0.5f;
        expected_target_x = 0.5f;
        expected_pre_dispatch_direction = 1;
        throw_from_target = true;
        bool unwind_seen = false;
        try {
            (void)tooie_camera_analog_apply(rdram, &ctx);
        } catch (const std::runtime_error&) {
            unwind_seen = true;
        }
        require(unwind_seen, "guest cooperative unwind must propagate");
        require(std::memcmp(&ctx, &original_context, sizeof(ctx)) == 0,
                "guest cooperative unwind must restore the full context");

        reset_stubs();
        right_x = 0.5f;
        MEM_H(0, rule) = 0;
        require(tooie_camera_analog_apply(rdram, &ctx) == -1,
                "a rejected mode must leave the original C input path active");
        require(pre_dispatch_calls == 0 && target_calls == 0,
                "a rejected mode must not dispatch camera commands");
        MEM_H(0, rule) = 1;

        reset_stubs();
        right_x = 0.5f;
        game_input_is_disabled = true;
        require(tooie_camera_analog_apply(rdram, &ctx) == -1,
                "disabled input must leave the original C input path active");
        require(pre_dispatch_calls == 0 && target_calls == 0,
                "disabled input must not dispatch camera commands");

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
