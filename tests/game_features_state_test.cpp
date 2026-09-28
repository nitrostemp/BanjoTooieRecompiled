#include "game_features.hpp"
#include "camera_interpolation.hpp"
#include "scene_observer.hpp"

#include "recomp.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

extern "C" void tooie_cheats_tick(uint8_t*, recomp_context*);
extern "C" void tooie_cheats_invalidate();
extern "C" void tooie_cutscene_state_command(uint8_t*, recomp_context*, std::uint32_t);

namespace {
std::array<bool, 12> typeable{};
std::array<bool, 12> available{};
std::array<bool, 12> active{};
unsigned set_calls = 0;
unsigned effect_calls = 0;
unsigned counter_refresh_calls = 0;
std::uint32_t last_counter = 0, last_counter_total = 0;
unsigned health_refresh_calls = 0;
std::uint32_t health_upgrade = 0, current_health = 5;
std::uint32_t selected_egg_type = 0;
std::array<std::uint32_t, 0x60> inventory{};
std::array<std::uint32_t, 0x60> inventory_capacity{};
std::array<bool, 0x50> learned_moves{};
std::unordered_set<std::uint32_t> persistent_flags;
bool force_broad_jiggy_gate = false;
bool suppress_silo_writes = false;
bool suppress_gi_door_write = false;
bool suppress_train_station_write = false;
bool throw_next_guest_call = false;
bool throw_next_move_set = false;
unsigned overlay_callsite_depth = 0;
std::uint16_t current_map = 0;
bool scene_activation_active = false;
struct GuestUnwind {};

std::size_t index(recomp_context* ctx) {
    assert(ctx->r4 >= 1 && ctx->r4 <= 12);
    return static_cast<std::size_t>(ctx->r4 - 1);
}

void return_and_clobber(recomp_context* ctx, bool value) {
    *ctx = {};
    ctx->r2 = value ? 1 : 0;
}

void set_guest_byte(uint8_t* rdram, std::uint32_t address, std::int8_t value) {
    MEM_B(0, static_cast<gpr>(static_cast<std::int32_t>(address))) = value;
}
}

namespace tooie::scene {
Snapshot snapshot() noexcept {
    Snapshot result{};
    result.map_available = true;
    result.map_id = current_map;
    result.activation_active = scene_activation_active;
    return result;
}
}

extern "C" void tooie_overlay_callsite_push(std::uint32_t pc) {
    assert(pc == 0x80094B24U); ++overlay_callsite_depth;
}
extern "C" void tooie_overlay_callsite_pop() {
    assert(overlay_callsite_depth != 0); --overlay_callsite_depth;
}

extern "C" void func_800D3DD0(uint8_t*, recomp_context* ctx) {
    if (throw_next_guest_call) {
        throw_next_guest_call = false;
        *ctx = {};
        throw GuestUnwind{};
    }
    const bool value = typeable[index(ctx)];
    return_and_clobber(ctx, value);
}
extern "C" void func_800D3E14(uint8_t*, recomp_context* ctx) {
    const bool value = available[index(ctx)];
    return_and_clobber(ctx, value);
}
extern "C" void func_800D3E40(uint8_t*, recomp_context* ctx) {
    const bool value = active[index(ctx)];
    return_and_clobber(ctx, value);
}
extern "C" void func_800D3F58(uint8_t*, recomp_context* ctx) {
    const auto cheat = index(ctx);
    const bool enabled = ctx->r5 != 0;
    ++set_calls;
    active[cheat] = enabled;
    return_and_clobber(ctx, false);
}
extern "C" void func_800D3FD4(uint8_t*, recomp_context* ctx) {
    ++effect_calls;
    return_and_clobber(ctx, false);
}
extern "C" void set_widescreen(uint8_t*, recomp_context* ctx) {
    return_and_clobber(ctx, false);
}
extern "C" void func_800C6E38(uint8_t*, recomp_context* ctx) {
    const auto move = static_cast<std::size_t>(ctx->r4);
    return_and_clobber(ctx, move < learned_moves.size() && learned_moves[move]);
}
extern "C" void func_800C7074(uint8_t*, recomp_context* ctx) {
    if (throw_next_move_set) {
        throw_next_move_set = false;
        *ctx = {};
        throw GuestUnwind{};
    }
    const auto move = static_cast<std::size_t>(ctx->r4);
    if (move < learned_moves.size()) learned_moves[move] = ctx->r5 != 0;
    return_and_clobber(ctx, false);
}
extern "C" void func_800C7010(uint8_t*, recomp_context* ctx) {
    for (const auto move : tooie::features::kMoves)
        learned_moves[static_cast<std::size_t>(move)] = true;
    return_and_clobber(ctx, false);
}
extern "C" void func_800DA298(uint8_t*, recomp_context* ctx) {
    const bool set = persistent_flags.contains(static_cast<std::uint32_t>(ctx->r4));
    return_and_clobber(ctx, set);
}
extern "C" void func_800DA544(uint8_t*, recomp_context* ctx) {
    const auto flag = static_cast<std::uint32_t>(ctx->r4);
    if (!(suppress_silo_writes && flag >= 0x32DU && flag <= 0x333U) &&
        !(suppress_gi_door_write && flag == 0x1ABU) &&
        !(suppress_train_station_write && flag == 0x403U))
        persistent_flags.insert(flag);
    constexpr std::array<std::uint32_t, 12> typeable_flags{
        0x06C, 0x06D, 0x06E, 0x06F, 0x070, 0x076, 0, 0, 0, 0, 0, 0};
    constexpr std::array<std::uint32_t, 12> available_flags{
        0x071, 0x072, 0x073, 0x074, 0x075, 0x077,
        0x078, 0x079, 0x07A, 0x042, 0x04B, 0x446};
    for (std::size_t i = 0; i < 12; ++i) {
        if (flag == typeable_flags[i]) typeable[i] = true;
        if (flag == available_flags[i]) available[i] = true;
    }
    return_and_clobber(ctx, false);
}
extern "C" void func_800DA3B8(uint8_t*, recomp_context* ctx) {
    const auto flag = static_cast<std::uint32_t>(ctx->r4);
    const bool value = ctx->r5 != 0;
    if (value) persistent_flags.insert(flag); else persistent_flags.erase(flag);
    constexpr std::array<std::uint32_t, 12> typeable_flags{
        0x06C, 0x06D, 0x06E, 0x06F, 0x070, 0x076, 0, 0, 0, 0, 0, 0};
    constexpr std::array<std::uint32_t, 12> available_flags{
        0x071, 0x072, 0x073, 0x074, 0x075, 0x077,
        0x078, 0x079, 0x07A, 0x042, 0x04B, 0x446};
    for (std::size_t i = 0; i < 12; ++i) {
        if (flag == typeable_flags[i]) typeable[i] = value;
        if (flag == available_flags[i]) available[i] = value;
    }
    return_and_clobber(ctx, false);
}
extern "C" void func_800D0894(uint8_t*, recomp_context* ctx) {
    const auto type = static_cast<std::uint32_t>(ctx->r4);
    const auto result = type == 0 ? 45U : type == 1 ? 90U : type == 2 ? 25U : type == 6 ? 153U : 0U;
    *ctx = {};
    ctx->r2 = result;
}
extern "C" void func_800D0A80(uint8_t*, recomp_context* ctx) {
    const auto record = static_cast<std::uint32_t>(ctx->r4);
    const auto type = static_cast<std::uint32_t>(ctx->r5);
    // Match the original D_8011AF04 bases used by D0A80: Jiggies occupy
    // 0x250..0x2A9 and Note records occupy 0x44F..0x4E7.
    const auto base = type == 0 ? 0x1F4U : type == 1 ? 0x250U : type == 2 ? 0x222U : 0x44FU;
    const auto result = base + record - 1U;
    *ctx = {};
    ctx->r2 = result;
}
extern "C" void func_800D035C(uint8_t*, recomp_context* ctx) {
    const auto type = static_cast<std::uint32_t>(ctx->r4);
    std::uint32_t collected = 0;
    const auto maximum = type == 0 ? 45U : type == 1 ? 90U : type == 2 ? 25U : type == 6 ? 153U : 0U;
    const auto base = type == 0 ? 0x1F4U : type == 1 ? 0x250U : type == 2 ? 0x222U : 0x44FU;
    for (std::uint32_t record = 1; record <= maximum; ++record)
        collected += persistent_flags.contains(base + record - 1U) ? 1U : 0U;
    const auto result = type == 6 && collected == 153 ? 900U : collected;
    *ctx = {};
    ctx->r2 = result;
}
extern "C" void func_800D129C(uint8_t*, recomp_context* ctx) {
    const auto color = static_cast<std::uint32_t>(ctx->r4);
    *ctx = {}; ctx->r2 = color + 1U;
}
extern "C" void func_800D1338(uint8_t*, recomp_context* ctx) {
    const auto color = static_cast<std::uint32_t>(ctx->r4);
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i <= color; ++i) total += i + 1U;
    std::uint32_t collected = 0;
    const auto start = total - color - 1U;
    for (std::uint32_t i = 0; i <= color; ++i)
        collected += persistent_flags.contains(0x1F4U + start + i) ? 1U : 0U;
    *ctx = {}; ctx->r2 = collected;
}
extern "C" void func_800D0C78(uint8_t*, recomp_context* ctx) {
    persistent_flags.insert(0x250U + static_cast<std::uint32_t>(ctx->r4) - 1U);
    return_and_clobber(ctx, false);
}
extern "C" void func_800D0B68(uint8_t*, recomp_context* ctx) {
    const auto record = static_cast<std::uint32_t>(ctx->r4);
    // The real helper also reports true during cutscenes/replay. Progression
    // reward reconciliation must therefore use the exact collected flag.
    const bool collected = force_broad_jiggy_gate ||
        persistent_flags.contains(0x250U + record - 1U);
    return_and_clobber(ctx, collected);
}
extern "C" void func_800DA564(uint8_t*, recomp_context* ctx) {
    assert(ctx->r4 == 0x4EA && ctx->r5 == 3);
    *ctx = {}; ctx->r2 = health_upgrade;
}
extern "C" void func_800DA7A8(uint8_t*, recomp_context* ctx) {
    assert(ctx->r4 == 0x4EA && ctx->r6 == 3);
    health_upgrade = static_cast<std::uint32_t>(ctx->r5);
    return_and_clobber(ctx, false);
}
extern "C" void func_800D517C(uint8_t*, recomp_context* ctx) {
    ++health_refresh_calls;
    current_health = 5U + health_upgrade;
    return_and_clobber(ctx, false);
}
extern "C" void func_8008FD48(uint8_t*, recomp_context* ctx) { *ctx = {}; ctx->r2 = 1; }
extern "C" void func_800D4E7C(uint8_t*, recomp_context* ctx) { *ctx = {}; ctx->r2 = current_health; }
extern "C" void func_800D4EB8(uint8_t*, recomp_context* ctx) { *ctx = {}; ctx->r2 = 5U + health_upgrade; }
extern "C" void func_800D4E18(uint8_t*, recomp_context* ctx) {
    current_health = std::min(5U + health_upgrade, current_health + static_cast<std::uint32_t>(ctx->r5));
    return_and_clobber(ctx, false);
}
extern "C" void func_800D4FB8(uint8_t*, recomp_context* ctx) {
    current_health = std::min(5U + health_upgrade, static_cast<std::uint32_t>(ctx->r5));
    return_and_clobber(ctx, false);
}
extern "C" void func_800D1A04(uint8_t*, recomp_context* ctx) {
    const auto item = static_cast<std::size_t>(ctx->r4); *ctx = {}; ctx->r2 = inventory[item];
}
extern "C" void func_800D1A6C(uint8_t*, recomp_context* ctx) {
    const auto item = static_cast<std::size_t>(ctx->r4); *ctx = {}; ctx->r2 = inventory_capacity[item];
}
extern "C" void func_800D1864(uint8_t*, recomp_context* ctx) {
    inventory[static_cast<std::size_t>(ctx->r4)] = static_cast<std::uint32_t>(ctx->r5);
    return_and_clobber(ctx, false);
}
extern "C" void func_80094510(uint8_t*, recomp_context* ctx) {
    *ctx = {}; ctx->r2 = selected_egg_type;
}
extern "C" void _gcegg_entrypoint_5(uint8_t*, recomp_context* ctx) {
    // Exact item bytes from asm/data/overlays/gc/egg.data.s (+9 in each
    // 14-byte record). Type 6 is special and type 7 wraps to Blue Eggs.
    constexpr std::array<std::uint32_t, 8> items{
        0x40, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x40};
    const auto type = static_cast<std::size_t>(ctx->r4);
    *ctx = {}; ctx->r2 = type < items.size() ? items[type] : 0;
}
extern "C" void func_800D24E8(uint8_t*, recomp_context* ctx) {
    ++counter_refresh_calls;
    last_counter = static_cast<std::uint32_t>(ctx->r4);
    last_counter_total = static_cast<std::uint32_t>(ctx->r5);
    assert(ctx->r6 == 0);
    return_and_clobber(ctx, false);
}

int main() {
    std::vector<uint8_t> rdram(8 * 1024 * 1024);
    selected_egg_type = 3;
    inventory[0x42] = 17;
    inventory_capacity[0x42] = 100;
    set_guest_byte(rdram.data(), 0x8012762C, 0);
    set_guest_byte(rdram.data(), 0x8012B3F1, 0);

    tooie::features::latch_for_game_start();
    typeable[0] = true;
    recomp_context ctx{};
    ctx.r4 = 0x00123456;
    ctx.r1 = 0x11111111;
    ctx.r31 = 0x31313131;
    ctx.f12.u64 = 0x1212121212121212ULL;
    ctx.hi = 0xAAAAAAAAAAAAAAAAULL;
    ctx.lo = 0xBBBBBBBBBBBBBBBBULL;
    const recomp_context expected = ctx;

    throw_next_guest_call = true;
    bool unwind_observed = false;
    try {
        tooie_cheats_tick(rdram.data(), &ctx);
    } catch (const GuestUnwind&) {
        unwind_observed = true;
    }
    assert(unwind_observed);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    tooie::features::request_cheat_refresh();

    tooie_cheats_tick(rdram.data(), &ctx);
    auto readout = tooie::features::cheat_readout();
    assert(readout.current && readout.observed && readout.cheats[0].typeable);
    assert(!readout.cheats[0].available);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);

    auto progression = tooie::features::progression_readout();
    assert(progression.current && progression.observed);
    assert(progression.notes == 0 && progression.jiggies == 0);
    // A current file and readable snapshots alone never authorize mutation.
    assert(!tooie::features::cheats_access_enabled());
    assert(!tooie::features::request_unlock_move(tooie::features::Move::GripGrab));
    assert(!tooie::features::request_open_gi_front_door());
    assert(!tooie::features::request_unlock_and_enable_cheat(tooie::features::Cheat::Feathers));
    tooie::features::set_cheats_access_enabled(true);
    assert(tooie::features::cheats_access_enabled());
    assert(tooie::features::request_unlock_move(tooie::features::Move::GripGrab));
    throw_next_move_set = true;
    unwind_observed = false;
    try {
        tooie_cheats_tick(rdram.data(), &ctx);
    } catch (const GuestUnwind&) {
        unwind_observed = true;
    }
    assert(unwind_observed);
    assert(!tooie::features::progression_readout().request_pending);
    assert(tooie::features::progression_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Failed);
    assert(!learned_moves[static_cast<std::size_t>(tooie::features::Move::GripGrab)]);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    assert(tooie::features::request_unlock_move(tooie::features::Move::GripGrab));
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.moves[0] && progression.applied_requests == 1);
    assert(!tooie::features::request_unlock_move(tooie::features::Move::GripGrab));
    assert(tooie::features::request_unlock_world(tooie::features::World::MayahemTemple));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().worlds[0]);
    assert(tooie::features::request_collect_all_notes());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().notes == 900);
    assert(persistent_flags.size() == 1U + 153U);
    assert(persistent_flags.contains(0x44FU) && persistent_flags.contains(0x4E7U));
    assert(!persistent_flags.contains(0U));
    assert(counter_refresh_calls == 1 && last_counter == 0xD0 && last_counter_total == 900);
    assert(tooie::features::request_collect_all_jiggies());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().jiggies == 90);
    assert(persistent_flags.size() == 1U + 153U + 90U);
    assert(persistent_flags.contains(0x250U) && persistent_flags.contains(0x2A9U));
    // 0x2AA begins the separate spawned-Jiggy range; collection completion
    // must touch only the original collected flags.
    assert(!persistent_flags.contains(0x2AAU));
    assert(counter_refresh_calls == 2 && last_counter == 0xD6 && last_counter_total == 90);

    // Two pieces were collected and one was already spent at Honey B. Bulk
    // completion must add only the 23 missing pieces to the remaining item.
    persistent_flags.insert(0x222U);
    persistent_flags.insert(0x223U);
    inventory[0x49] = 1U;
    assert(tooie::features::request_collect_all_honeycombs());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.honeycombs == 25);
    assert(persistent_flags.contains(0x222U) && persistent_flags.contains(0x23AU));
    assert(inventory[0x49] == 24U);
    assert(!tooie::features::request_collect_all_honeycombs());
    assert(inventory[0x49] == 24U);

    for (std::uint32_t flag = 0x2A0U; flag <= 0x2A8U; ++flag) persistent_flags.erase(flag);
    assert(tooie::features::request_collect_all_jinjos());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.jinjos == 45 && progression.jinjo_families == 9);
    assert(progression.jinjo_family_rewards == 9);
    assert(persistent_flags.contains(0x1F4U) && persistent_flags.contains(0x220U));
    assert(persistent_flags.contains(0x2A0U) && persistent_flags.contains(0x2A8U));
    // A broad D0B68 cutscene/replay result must not hide a missing persistent
    // family reward once all 45 Jinjos already read as complete.
    persistent_flags.erase(0x2A4U);
    force_broad_jiggy_gate = true;
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.jinjos == 45 && progression.jinjo_families == 9);
    assert(progression.jinjo_family_rewards == 8);
    assert(tooie::features::request_collect_all_jinjos());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(persistent_flags.contains(0x2A4U));
    assert(tooie::features::progression_readout().jinjo_family_rewards == 9);
    assert(!tooie::features::request_collect_all_jinjos());
    force_broad_jiggy_gate = false;

    assert(tooie::features::request_upgrade_max_health());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.health_capacity == 10 && progression.health_upgrade_level == 5 &&
        current_health == 5);
    assert(health_refresh_calls == 1);
    assert(!tooie::features::request_upgrade_max_health());
    assert(tooie::features::request_refill_health());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().current_health == 10);

    assert(tooie::features::request_unlock_all_hub_connections());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.hub_connections_open == 5 && progression.hub_connection_total == 5);
    assert(persistent_flags.contains(0x391U) && persistent_flags.contains(0x3E9U) &&
        persistent_flags.contains(0x3EAU) && persistent_flags.contains(0x3F0U) &&
        persistent_flags.contains(0x436U));

    // The factory door's original actor reads 0x1AB on init and writes it on
    // opening. A failed save-flag write must not produce an Applied receipt.
    assert(!progression.gi_front_door_open);
    suppress_gi_door_write = true;
    assert(tooie::features::request_open_gi_front_door());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(!progression.gi_front_door_open);
    assert(progression.receipt.outcome == tooie::features::RequestOutcome::Failed);
    suppress_gi_door_write = false;
    assert(tooie::features::request_open_gi_front_door());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.gi_front_door_open && persistent_flags.contains(0x1ABU));
    assert(progression.receipt.outcome == tooie::features::RequestOutcome::Applied);
    assert(!tooie::features::request_open_gi_front_door());
    assert(!persistent_flags.contains(0x095U) && !persistent_flags.contains(0x086U) &&
        !persistent_flags.contains(0x163U) && !persistent_flags.contains(0x4EDU));

    // Station unlocks must touch only the six platform flags, not Chuffy's
    // repair, boss, cooling, location, or adjacent story flags.
    using TrainStation = tooie::features::TrainStation;
    constexpr std::array<std::uint32_t, 6> station_flags{0x096U, 0x164U, 0x163U, 0x1D0U, 0x1CFU, 0x403U};
    auto expected_flags = persistent_flags;
    assert(!tooie::features::request_unlock_train_station(static_cast<TrainStation>(99)));
    tooie::features::set_cheats_access_enabled(false);
    assert(!tooie::features::request_unlock_all_train_stations());
    tooie::features::set_cheats_access_enabled(true);
    assert(tooie::features::request_unlock_train_station(TrainStation::CliffTop));
    tooie::features::set_cheats_access_enabled(false);
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(persistent_flags == expected_flags); // Revoked requests never write.
    tooie::features::set_cheats_access_enabled(true);
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    suppress_train_station_write = true;
    assert(tooie::features::request_unlock_train_station(TrainStation::CliffTop));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().receipt.outcome == tooie::features::RequestOutcome::Failed);
    assert(persistent_flags == expected_flags);
    suppress_train_station_write = false;
    for (std::size_t i = 0; i < tooie::features::kTrainStations.size(); ++i) {
        const auto station = tooie::features::kTrainStations[i];
        assert(tooie::features::request_unlock_train_station(station));
        tooie_cheats_tick(rdram.data(), &ctx);
        expected_flags.insert(station_flags[i]);
        assert(persistent_flags == expected_flags);
        const auto readout = tooie::features::progression_readout();
        assert(readout.train_stations_unlocked[i]);
        assert(readout.receipt.outcome == tooie::features::RequestOutcome::Applied);
        assert(!tooie::features::request_unlock_train_station(station));
    }
    assert(!tooie::features::request_unlock_all_train_stations());
    // A partly unlocked file is completed without changing unrelated state.
    for (std::size_t i = 0; i < station_flags.size() - 1; ++i) persistent_flags.erase(station_flags[i]);
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::request_unlock_all_train_stations());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(persistent_flags == expected_flags);
    progression = tooie::features::progression_readout();
    assert(std::all_of(progression.train_stations_unlocked.begin(),
        progression.train_stations_unlocked.end(), [](bool unlocked) { return unlocked; }));
    assert(progression.receipt.outcome == tooie::features::RequestOutcome::Applied);

    // Silo actors use their marker index plus 0x32C, producing the seven
    // source-named travel flags 0x32D..0x333. The adjacent 0x334 flag is the
    // first-time silo tutorial and is not part of travel discovery.
    assert(progression.silos_unlocked == 0 && progression.silo_total == 7);
    suppress_silo_writes = true;
    assert(tooie::features::request_unlock_all_silos());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Failed);
    assert(tooie::features::progression_readout().silos_unlocked == 0);
    suppress_silo_writes = false;
    assert(tooie::features::request_unlock_all_silos());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.silos_unlocked == progression.silo_total);
    for (std::uint32_t flag = 0x32DU; flag <= 0x333U; ++flag)
        assert(persistent_flags.contains(flag));
    assert(!persistent_flags.contains(0x334U));
    assert(!tooie::features::request_unlock_all_silos());

    // chwarppad derives each persistent activation/link flag from its world
    // group and local pad index. Activate one complete five-pad world first.
    assert(progression.warp_pad_totals[0] == 5 && progression.warp_pads_active[0] == 0);
    assert(tooie::features::request_activate_world_warp_pads(
        tooie::features::World::MayahemTemple));
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.warp_pads_active[0] == 5);
    for (std::uint32_t flag = 0x3ACU; flag <= 0x3B0U; ++flag)
        assert(persistent_flags.contains(flag));
    assert(!tooie::features::request_activate_world_warp_pads(
        tooie::features::World::MayahemTemple));
    assert(!tooie::features::request_activate_world_warp_pads(
        static_cast<tooie::features::World>(99)));

    // The source table has only two real pads in CCL and CK. Global activation
    // must leave their three reserved/unused slots untouched.
    persistent_flags.insert(0x3CFU);
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(progression.warp_pad_totals[7] == 2 && progression.warp_pads_active[7] == 1);
    assert(tooie::features::request_activate_all_warp_pads());
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    for (std::size_t i = 0; i < tooie::features::kWorlds.size(); ++i)
        assert(progression.warp_pads_active[i] == progression.warp_pad_totals[i]);
    assert(!persistent_flags.contains(0x3D1U) && !persistent_flags.contains(0x3D2U) &&
        !persistent_flags.contains(0x3D3U) && !persistent_flags.contains(0x3D6U) &&
        !persistent_flags.contains(0x3D7U) && !persistent_flags.contains(0x3D8U));
    assert(!tooie::features::request_activate_all_warp_pads());

    assert(progression.selected_egg_item == 0x42 && progression.selected_egg_ammo == 17 &&
        progression.selected_egg_capacity == 100);
    assert(tooie::features::request_refill_selected_eggs());
    selected_egg_type = 99;
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Rejected);
    assert(inventory[0x42] == 17);
    selected_egg_type = 3;
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::request_refill_selected_eggs());
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(inventory[0x42] == 100);
    assert(overlay_callsite_depth == 0);
    selected_egg_type = 99;
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().selected_egg_capacity == 0);
    assert(!tooie::features::request_refill_selected_eggs());
    selected_egg_type = 3;

    current_map = 0x141U;
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(!tooie::features::progression_readout().boss_edits_available[0]);
    assert(!tooie::features::request_boss_defeated(tooie::features::Boss::Klungo1, true));
    current_map = 0;
    persistent_flags.insert(0x318U);
    persistent_flags.insert(0x0C1U);
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::request_boss_defeated(tooie::features::Boss::Klungo1, false));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(!persistent_flags.contains(0x318U) && !persistent_flags.contains(0x0C1U));

    const auto boss_applied_before = tooie::features::progression_readout().applied_requests;
    assert(tooie::features::request_boss_defeated(tooie::features::Boss::Targitzan, true));
    current_map = 0x17AU;
    tooie_cheats_tick(rdram.data(), &ctx);
    progression = tooie::features::progression_readout();
    assert(!progression.bosses[3] && !progression.boss_edits_available[3]);
    assert(progression.applied_requests == boss_applied_before);
    assert(progression.receipt.outcome == tooie::features::RequestOutcome::Rejected);
    assert(!tooie::features::request_boss_defeated(tooie::features::Boss::Targitzan, true));
    current_map = 0;
    assert(tooie::features::request_boss_defeated(tooie::features::Boss::Targitzan, true));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().bosses[3]);
    persistent_flags.insert(0x088U);
    assert(tooie::features::request_boss_defeated(tooie::features::Boss::Targitzan, false));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(!tooie::features::progression_readout().bosses[3]);
    assert(!persistent_flags.contains(0x088U) && persistent_flags.contains(0x250U));
    // Mingy Jongo's ordinary completion is coupled to its collected Jiggy;
    // the backend must reject a fabricated or destructive reversal.
    assert(!tooie::features::request_boss_defeated(tooie::features::Boss::MingyJongo, false));
    persistent_flags.insert(0x043U);
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::progression_readout().bosses[12]);
    assert(!tooie::features::request_boss_defeated(tooie::features::Boss::Hag1, true));
    tooie_cutscene_state_command(rdram.data(), &ctx, 2);
    assert(!tooie::features::request_boss_defeated(tooie::features::Boss::OldKingCoal, true));
    tooie_cutscene_state_command(rdram.data(), &ctx, 1);

    assert(tooie::features::request_unlock_and_enable_cheat(tooie::features::Cheat::Homing));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(available[11] && active[11]);
    active[11] = false;
    set_calls = effect_calls = 0;
    assert(tooie::features::request_invincibility_enabled(true));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(active[8] && tooie::features::progression_readout().invincible);
    assert(tooie::features::cheat_readout().cheats[8].active);
    assert(tooie::features::request_unlock_and_enable_cheat(tooie::features::Cheat::HoneyKing));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::request_cheat_enabled(tooie::features::Cheat::HoneyKing, false));
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(!active[8] && !tooie::features::progression_readout().invincible);
    active[8] = false;
    tooie::features::request_progression_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    set_calls = effect_calls = 0;
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);

    assert(!tooie::features::request_cheat_enabled(tooie::features::Cheat::Feathers, true));
    assert(set_calls == 0 && effect_calls == 0);

    available[0] = true;
    tooie::features::request_cheat_refresh();
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(tooie::features::request_cheat_enabled(tooie::features::Cheat::Feathers, true));
    assert(tooie::features::cheat_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Queued);
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(active[0] && set_calls == 1 && effect_calls == 1);
    assert(tooie::features::cheat_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Applied);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);

    assert(tooie::features::request_cheat_enabled(tooie::features::Cheat::Feathers, false));
    assert(tooie::features::request_unlock_all_moves());
    tooie::features::set_cheats_access_enabled(false);
    assert(!tooie::features::cheat_readout().request_pending);
    assert(!tooie::features::progression_readout().request_pending);
    assert(tooie::features::cheat_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Canceled);
    assert(tooie::features::progression_readout().receipt.outcome ==
        tooie::features::RequestOutcome::Canceled);
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(active[0] && set_calls == 1 && effect_calls == 1);
    assert(!learned_moves[static_cast<std::size_t>(tooie::features::Move::BreegullBlaster)]);
    assert(!tooie::features::request_refill_health());
    tooie::features::set_cheats_access_enabled(true);
    assert(tooie::features::request_cheat_enabled(tooie::features::Cheat::Feathers, false));
    assert(tooie::features::request_unlock_all_moves());
    const auto generation = tooie::features::cheat_readout().generation;
    const auto progression_generation = tooie::features::progression_readout().generation;
    tooie::camera_interpolation::configure_cutscene_motion(
        tooie::camera_interpolation::CutsceneMotion::Original);
    tooie_cutscene_state_command(rdram.data(), &ctx, 2);
    assert(tooie::features::cutscene_active());
    tooie_cheats_invalidate();
    readout = tooie::features::cheat_readout();
    assert(!readout.current && !readout.observed && !readout.request_pending);
    assert(readout.receipt.outcome == tooie::features::RequestOutcome::Canceled);
    assert(readout.generation == generation + 1);
    assert(active[0] && set_calls == 1 && effect_calls == 1);
    progression = tooie::features::progression_readout();
    assert(!progression.current && !progression.observed && !progression.request_pending);
    assert(progression.receipt.outcome == tooie::features::RequestOutcome::Canceled);
    assert(progression.generation == progression_generation + 1);
    assert(!learned_moves[static_cast<std::size_t>(tooie::features::Move::BreegullBlaster)]);

    // File/progression invalidation does not prove that glcut ended. Preserve
    // the actual active state and capture OriginalMotion on the exact task.
    assert(tooie::camera_interpolation::bind_task(0x00500000U));
    assert(tooie::camera_interpolation::consume_task_interpolation(0x00500000U) ==
        tooie::camera_interpolation::TaskInterpolation::OriginalMotion);
    tooie_cutscene_state_command(rdram.data(), &ctx, 1);
    assert(!tooie::features::cutscene_active());

    set_guest_byte(rdram.data(), 0x8012B3F1, -1);
    tooie_cheats_tick(rdram.data(), &ctx);
    assert(!tooie::features::cheat_readout().current);
    assert(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    return 0;
}
