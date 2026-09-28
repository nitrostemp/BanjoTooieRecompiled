#include "hud_layout.hpp"

#include "recomp.h"
#include "widescreen.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
std::atomic<tooie::hud_layout::CounterLayout> configured_layout{
    tooie::hud_layout::CounterLayout::Centered};
std::atomic<tooie::hud_layout::CounterLayout> latched_layout{
    tooie::hud_layout::CounterLayout::Centered};
std::atomic<tooie::hud_layout::Proportions> configured_proportions_value{
    tooie::hud_layout::Proportions::Original};
std::atomic<tooie::hud_layout::Proportions> latched_proportions_value{
    tooie::hud_layout::Proportions::Original};

constexpr std::uint32_t kSlotBase = 0x80123880U;
constexpr std::uint32_t kSlotStride = 0x1CU;
constexpr std::uint32_t kSlotCount = 48U;
constexpr std::uint32_t kCounterTypeBase = 0x8012B2C0U;
constexpr std::uint32_t kOriginalWidescreenFlag = 0x800799B8U;
constexpr std::uint32_t kSpriteOrthoMatrix = 0x8012D554U;
constexpr std::uint32_t kGuestRdramFirst = 0x80000400U;
constexpr std::uint32_t kGuestRdramEnd = 0x80800000U;
constexpr std::uint32_t kNativeScreenWidth = 304U;
constexpr std::uint32_t kNativeScreenHeight = 228U;
constexpr std::uint32_t kExtendedOpcode = 0x64000000U;
constexpr std::uint32_t kExtendedHook = 0xE0525464U;
constexpr std::uint32_t kExtendedEnable = 0x10000064U;
constexpr std::uint32_t kExtendedDisable = 0x20000000U;
constexpr std::uint32_t kExSetScissor = kExtendedOpcode | 5U;
constexpr std::uint32_t kExSetRectAlign = kExtendedOpcode | 6U;
constexpr std::uint32_t kExPushScissor = kExtendedOpcode | 0x17U;
constexpr std::uint32_t kExPopScissor = kExtendedOpcode | 0x18U;
constexpr std::uint32_t kExOriginRight = 0x400U;
constexpr std::uint32_t kExOriginNone = 0x800U;
constexpr std::uint16_t kHealthCounter = 0xCFU;
constexpr std::uint16_t kNotesCounter = 0xD0U;
constexpr std::uint16_t kBlueEggCounter = 0xD1U;
constexpr std::uint16_t kClockworkEggCounter = 0xD5U;
constexpr std::uint16_t kJiggiesCounter = 0xD6U;
constexpr std::uint16_t kRedFeatherCounter = 0xD7U;
constexpr std::uint16_t kGoldFeatherCounter = 0xD8U;
constexpr std::uint16_t kEmptyHoneycombCounter = 0xDAU;
constexpr std::uint16_t kFirstJinjoFamilyCounter = 0xE1U;
constexpr std::uint16_t kLastJinjoFamilyCounter = 0xE9U;

constexpr bool is_ordinary_counter(std::uint16_t counter_id) noexcept {
    return counter_id == kHealthCounter ||
        counter_id == kNotesCounter ||
        (counter_id >= kBlueEggCounter && counter_id <= kClockworkEggCounter) ||
        counter_id == kJiggiesCounter ||
        (counter_id >= kRedFeatherCounter && counter_id <= kGoldFeatherCounter) ||
        counter_id == kEmptyHoneycombCounter ||
        (counter_id >= kFirstJinjoFamilyCounter &&
            counter_id <= kLastJinjoFamilyCounter);
}

bool expanded_counter_slot(uint8_t* rdram, recomp_context* ctx,
    std::uint32_t& slot_address) noexcept {
    if (!rdram || !ctx || !tooie::widescreen::latched_enabled() ||
        tooie::hud_layout::latched_counter_layout() !=
            tooie::hud_layout::CounterLayout::Expanded ||
        tooie::hud_layout::latched_proportions() !=
            tooie::hud_layout::Proportions::Original ||
        tooie::widescreen::latched_aspect_ratio() <= 16.0 / 9.0 ||
        MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
            kOriginalWidescreenFlag))) == 0) return false;

    // scinfobar receives slot+0x18's callback-data pointer, not the slot
    // address kept by its core caller. Resolve the owner without host state.
    if (ctx->r16 == 0U) return false;
    for (std::uint32_t index = 0; index < kSlotCount; ++index) {
        const auto candidate = kSlotBase + index * kSlotStride;
        const auto signed_candidate = static_cast<gpr>(static_cast<std::int32_t>(candidate));
        if (static_cast<std::uint32_t>(MEM_W(0X18, signed_candidate)) !=
            static_cast<std::uint32_t>(ctx->r16)) continue;
        const std::uint32_t type_address = kCounterTypeBase +
            index * sizeof(std::uint16_t);
        const auto counter_id = static_cast<std::uint16_t>(MEM_H(0,
            static_cast<gpr>(static_cast<std::int32_t>(type_address))));
        if (!is_ordinary_counter(counter_id) ||
            static_cast<std::uint32_t>(tooie::hud_layout::adjust_counter_x(
                counter_id, static_cast<std::int16_t>(MEM_H(0X6, signed_candidate)))) !=
                static_cast<std::uint32_t>(ctx->r18)) continue;
        slot_address = candidate;
        return true;
    }
    return false;
}

bool append_hud_gfx(uint8_t* rdram, recomp_context* ctx,
    const std::uint32_t* words, std::uint32_t command_count) noexcept {
    // The scinfobar callback receives its Gfx context in s1. Its first word
    // is the same live display-list cursor advanced by the game draw helpers.
    const auto context = static_cast<std::uint32_t>(ctx->r17);
    if (context < kGuestRdramFirst || context > kGuestRdramEnd - 4U ||
        (context & 3U) != 0U) return false;
    const auto cursor = static_cast<std::uint32_t>(MEM_W(0,
        static_cast<gpr>(static_cast<std::int32_t>(context))));
    const std::uint32_t byte_count = command_count * 8U;
    if (cursor < kGuestRdramFirst || cursor > kGuestRdramEnd - byte_count ||
        (cursor & 7U) != 0U) return false;
    const auto signed_cursor = static_cast<gpr>(static_cast<std::int32_t>(cursor));
    for (std::uint32_t index = 0; index < command_count * 2U; ++index) {
        MEM_W(index * 4U, signed_cursor) = words[index];
    }
    MEM_W(0, static_cast<gpr>(static_cast<std::int32_t>(context))) =
        cursor + byte_count;
    return true;
}

constexpr tooie::hud_layout::CounterLayout normalize_layout(
    tooie::hud_layout::CounterLayout layout) noexcept {
    return layout == tooie::hud_layout::CounterLayout::Expanded
        ? layout : tooie::hud_layout::CounterLayout::Centered;
}

constexpr tooie::hud_layout::Proportions normalize_proportions(
    tooie::hud_layout::Proportions proportions) noexcept {
    return proportions == tooie::hud_layout::Proportions::Stretch
        ? proportions : tooie::hud_layout::Proportions::Original;
}
}

namespace tooie::hud_layout {

void configure_counter_layout(CounterLayout layout) noexcept {
    configured_layout.store(normalize_layout(layout), std::memory_order_release);
}

void configure_proportions(Proportions proportions) noexcept {
    configured_proportions_value.store(normalize_proportions(proportions),
        std::memory_order_release);
}

void latch_for_game_start() noexcept {
    latched_layout.store(configured_layout.load(std::memory_order_acquire),
        std::memory_order_release);
    latched_proportions_value.store(configured_proportions_value.load(std::memory_order_acquire),
        std::memory_order_release);
}

CounterLayout configured_counter_layout() noexcept {
    return configured_layout.load(std::memory_order_acquire);
}

CounterLayout latched_counter_layout() noexcept {
    return latched_layout.load(std::memory_order_acquire);
}

Proportions configured_proportions() noexcept {
    return configured_proportions_value.load(std::memory_order_acquire);
}

Proportions latched_proportions() noexcept {
    return latched_proportions_value.load(std::memory_order_acquire);
}

int adjust_counter_x(std::uint16_t counter_id, int original_x) noexcept {
    if (latched_counter_layout() != CounterLayout::Expanded ||
        !is_ordinary_counter(counter_id) ||
        !widescreen::latched_enabled()) {
        return original_x;
    }

    constexpr double original_native_aspect = 16.0 / 9.0;
    const double scale = widescreen::latched_aspect_ratio() / original_native_aspect;
    if (scale <= 1.0) return original_x;
    const long adjusted = std::lround(static_cast<double>(original_x) * scale);
    return static_cast<int>(std::clamp(adjusted,
        static_cast<long>(std::numeric_limits<std::int16_t>::min()),
        static_cast<long>(std::numeric_limits<std::int16_t>::max())));
}

} // namespace tooie::hud_layout

extern "C" void tooie_hud_ortho_adjust(uint8_t* rdram, recomp_context* ctx) noexcept {
    // func_800E42F0 owns the orthographic matrix shared by 2D sprites and
    // text. The game alternates between 4:3 and 16:9 horizontal extents;
    // either one stretches when the renderer fills a wider output. Give both
    // paths the same native-width extent, leaving the world projection alone.
    if (!tooie::widescreen::latched_enabled() ||
        tooie::hud_layout::latched_proportions() == tooie::hud_layout::Proportions::Stretch ||
        MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
            kOriginalWidescreenFlag))) == 0) {
        return;
    }
    constexpr double original_wide_aspect = 16.0 / 9.0;
    constexpr double original_wide_half_width = 810.0;
    const float half_width = static_cast<float>(original_wide_half_width *
        tooie::widescreen::latched_aspect_ratio() / original_wide_aspect);
    ctx->r5 = std::bit_cast<std::uint32_t>(-half_width);
    ctx->r6 = std::bit_cast<std::uint32_t>(half_width);
}

extern "C" void tooie_hud_egg_reticle_ortho_restore(
    uint8_t* rdram, recomp_context* ctx) noexcept {
    // The egg cursor's sprite renderer already applies its own inverse-aspect
    // X scale. Undo only the additional native-width ortho X scale on this
    // draw's matrix; its center/aim translation and the world matrix stay put.
    if (!rdram || !ctx || !tooie::widescreen::latched_enabled() ||
        tooie::hud_layout::latched_proportions() !=
            tooie::hud_layout::Proportions::Original ||
        MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
            kOriginalWidescreenFlag))) == 0) {
        return;
    }

    const auto matrix = static_cast<std::uint32_t>(MEM_W(0,
        static_cast<gpr>(static_cast<std::int32_t>(kSpriteOrthoMatrix))));
    constexpr std::uint32_t rdram_first = 0x80000400U;
    constexpr std::uint32_t rdram_end = 0x80800000U;
    if (matrix < rdram_first || matrix > rdram_end - 0x40U ||
        (matrix & 7U) != 0U) return;

    constexpr float original_wide_half_width = 810.0f;
    const float native_half_width = static_cast<float>(original_wide_half_width *
        tooie::widescreen::latched_aspect_ratio() / (16.0 / 9.0));
    const auto expected_m00 = static_cast<std::uint32_t>(
        (1.0f / native_half_width) * 65536.0f);
    constexpr auto original_m00 = static_cast<std::uint32_t>(
        (1.0f / 608.0f) * 65536.0f);
    const auto signed_matrix = static_cast<gpr>(static_cast<std::int32_t>(matrix));
    // guMtxF2L packs M00's integer half at +0 and fractional half at +0x20.
    // The companion M01 is zero in both words for this symmetric ortho.
    if (MEM_W(0, signed_matrix) != 0U ||
        static_cast<std::uint32_t>(MEM_W(0x20, signed_matrix)) !=
            (expected_m00 << 16U)) return;
    MEM_W(0x20, signed_matrix) = original_m00 << 16U;
}

extern "C" void tooie_hud_counter_adjust(uint8_t* rdram, recomp_context* ctx) noexcept {
    // This is the same live signed halfword read by func_80015D14 and
    // func_800E42F0. Scripted Original 4:3 cutscenes clear it even while the
    // host widescreen profile remains latched, so their HUD must stay 4:3.
    if (MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(
            kOriginalWidescreenFlag))) == 0) {
        return;
    }
    const std::uint32_t slot_address = static_cast<std::uint32_t>(ctx->r16);
    const std::uint32_t slot_offset = slot_address - kSlotBase;
    if (slot_offset >= kSlotStride * kSlotCount || slot_offset % kSlotStride != 0U) return;

    const std::uint32_t slot_index = slot_offset / kSlotStride;
    const std::uint32_t type_address = kCounterTypeBase + slot_index * sizeof(std::uint16_t);
    const auto counter_id = static_cast<std::uint16_t>(
        MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(type_address))));
    ctx->r6 = static_cast<gpr>(tooie::hud_layout::adjust_counter_x(
        counter_id, SIGNED(ctx->r6)));
}

extern "C" void tooie_hud_counter_rect_begin(
    uint8_t* rdram, recomp_context* ctx) noexcept {
    // +0x30 is unused in this function's 0x38-byte guest frame; +0x34 is
    // the game's own scratch word. This marker survives callback yields.
    MEM_W(0X30, ctx->r29) = 0U;
    std::uint32_t slot_address = 0;
    if (!expanded_counter_slot(rdram, ctx, slot_address)) return;

    // RT64's ordinary texture rectangles retain their native pixel width,
    // but the RDP clips them against the game's 304-pixel scissor before
    // widescreen placement. The expanded slot X can exceed that bound.
    // Re-express the same final rectangle in a 304-pixel, side-anchored
    // coordinate system, and give only these digits a full-width scissor.
    const double ratio = tooie::widescreen::latched_aspect_ratio() / (4.0 / 3.0);
    const long center_shift_quarters = std::lround(
        (ratio - 1.0) * static_cast<double>(kNativeScreenWidth) * 2.0);
    const auto home_x = static_cast<std::int16_t>(MEM_H(0,
        static_cast<gpr>(static_cast<std::int32_t>(slot_address))));
    const bool right_side = home_x >= static_cast<std::int16_t>(kNativeScreenWidth / 2U);
    const long offset_quarters = right_side
        ? -static_cast<long>(kNativeScreenWidth * 4U) - center_shift_quarters
        : center_shift_quarters;
    if (offset_quarters < std::numeric_limits<std::int16_t>::min() ||
        offset_quarters > std::numeric_limits<std::int16_t>::max()) return;

    const std::uint32_t origin = right_side ? kExOriginRight : 0U;
    const std::uint32_t offset = static_cast<std::uint16_t>(offset_quarters);
    const std::uint32_t commands[] = {
        kExtendedHook, kExtendedEnable,
        kExPushScissor, 0U,
        kExSetScissor, kExOriginRight << 14U,
        0U, kNativeScreenHeight * 4U,
        kExSetRectAlign, origin | (origin << 12U),
        offset << 16U, offset << 16U,
    };
    if (append_hud_gfx(rdram, ctx, commands, 6U))
        MEM_W(0X30, ctx->r29) = 1U;
}

extern "C" void tooie_hud_counter_rect_end(
    uint8_t* rdram, recomp_context* ctx) noexcept {
    if (MEM_W(0X30, ctx->r29) != 1U) return;
    MEM_W(0X30, ctx->r29) = 0U;
    const std::uint32_t commands[] = {
        kExSetRectAlign, kExOriginNone | (kExOriginNone << 12U),
        0U, 0U,
        kExPopScissor, 0U,
        kExtendedHook, kExtendedDisable,
    };
    append_hud_gfx(rdram, ctx, commands, 4U);
}
