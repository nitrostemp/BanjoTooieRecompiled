#include "hud_layout.hpp"
#include "recomp.h"
#include "widescreen.hpp"

#include <cassert>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <vector>

extern "C" void tooie_hud_counter_adjust(uint8_t*, recomp_context*) noexcept;
extern "C" void tooie_hud_ortho_adjust(uint8_t*, recomp_context*) noexcept;

namespace {
constexpr std::uint32_t kSlotBase = 0x80123880U;
constexpr std::uint32_t kCounterTypeBase = 0x8012B2C0U;
constexpr std::uint32_t kOriginalWidescreenFlag = 0x800799B8U;
}

int main() {
    std::vector<std::uint8_t> storage(8U * 1024U * 1024U);
    auto* rdram = storage.data();
    recomp_context ctx{};
    ctx.r16 = static_cast<gpr>(static_cast<std::int32_t>(kSlotBase));

    tooie::widescreen::configure_profile(true);
    tooie::widescreen::configure_native_aspect(
        tooie::widescreen::NativeAspect::Ratio21x9);
    tooie::widescreen::latch_for_game_start();
    tooie::hud_layout::configure_counter_layout(
        tooie::hud_layout::CounterLayout::Expanded);
    tooie::hud_layout::latch_for_game_start();
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
        kOriginalWidescreenFlag))) = 1;

    // Source-identified ordinary HUD counters move from the centered 16:9
    // coordinate system to the selected native width.
    constexpr std::array<std::uint16_t, 20> ordinary_counter_ids{
        0xCF,                         // Health
        0xD0,                         // Notes
        0xD1, 0xD2, 0xD3, 0xD4, 0xD5, // Egg ammo
        0xD6,                         // Jiggies
        0xD7, 0xD8,                   // Feather ammo
        0xDA,                         // Empty Honeycombs
        0xE1, 0xE2, 0xE3, 0xE4, 0xE5,
        0xE6, 0xE7, 0xE8, 0xE9,       // Jinjo families
    };
    for (const auto counter_id : ordinary_counter_ids) {
        assert(tooie::hud_layout::adjust_counter_x(counter_id, 160) == 210);
    }

    // The hook changes only a2/r6.
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(kCounterTypeBase))) = 0xD0;
    ctx.r6 = static_cast<gpr>(static_cast<std::int32_t>(-160));
    ctx.r7 = 37;
    tooie_hud_counter_adjust(rdram, &ctx);
    assert(static_cast<std::int32_t>(ctx.r6) == -210);
    assert(ctx.r7 == 37);

    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(kCounterTypeBase))) = 0xD6;
    ctx.r6 = 160;
    tooie_hud_counter_adjust(rdram, &ctx);
    assert(static_cast<std::int32_t>(ctx.r6) == 210);

    // The source-identified TNT Mine minigame counter stays in its authored
    // layout rather than being mistaken for an ordinary HUD counter.
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(kCounterTypeBase))) = 0x102;
    ctx.r6 = 160;
    tooie_hud_counter_adjust(rdram, &ctx);
    assert(static_cast<std::int32_t>(ctx.r6) == 160);

    // Original 4:3 and scripted Original 4:3 cutscenes clear the same live
    // flag used by the game's projection owner, so expanded placement pauses.
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
        kOriginalWidescreenFlag))) = 0;
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(kCounterTypeBase))) = 0xD0;
    ctx.r6 = -160;
    tooie_hud_counter_adjust(rdram, &ctx);
    assert(static_cast<std::int32_t>(ctx.r6) == -160);

    // Centered is the default-compatible mode even on an ultrawide profile.
    tooie::hud_layout::configure_counter_layout(
        tooie::hud_layout::CounterLayout::Centered);
    tooie::hud_layout::latch_for_game_start();
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
        kOriginalWidescreenFlag))) = 1;
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(kCounterTypeBase))) = 0xD0;
    ctx.r6 = -160;
    tooie_hud_counter_adjust(rdram, &ctx);
    assert(static_cast<std::int32_t>(ctx.r6) == -160);

    // Every sprite/text orthographic draw uses one native-width projection,
    // including the game's alternate 4:3 and anamorphic 16:9 branches.
    tooie::hud_layout::configure_proportions(
        tooie::hud_layout::Proportions::Original);
    tooie::hud_layout::latch_for_game_start();
    auto set_ortho = [&](float left, float right) {
        ctx.r5 = std::bit_cast<std::uint32_t>(left);
        ctx.r6 = std::bit_cast<std::uint32_t>(right);
    };
    auto left = [&] { return std::bit_cast<float>(static_cast<std::uint32_t>(ctx.r5)); };
    auto right = [&] { return std::bit_cast<float>(static_cast<std::uint32_t>(ctx.r6)); };
    set_ortho(-608.0f, 608.0f);
    tooie_hud_ortho_adjust(rdram, &ctx);
    assert(std::fabs(left() + 1063.125f) < 0.01f);
    assert(std::fabs(right() - 1063.125f) < 0.01f);
    set_ortho(-810.0f, 810.0f);
    tooie_hud_ortho_adjust(rdram, &ctx);
    assert(std::fabs(right() - 1063.125f) < 0.01f);
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
        kOriginalWidescreenFlag))) = 0;
    set_ortho(-608.0f, 608.0f);
    tooie_hud_ortho_adjust(rdram, &ctx);
    assert(left() == -608.0f && right() == 608.0f);
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
        kOriginalWidescreenFlag))) = 1;
    tooie::hud_layout::configure_proportions(
        tooie::hud_layout::Proportions::Stretch);
    tooie::hud_layout::latch_for_game_start();
    tooie_hud_ortho_adjust(rdram, &ctx);
    assert(left() == -608.0f && right() == 608.0f);
    return 0;
}
