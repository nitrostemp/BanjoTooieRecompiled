#include "game_features.hpp"

#include "camera_interpolation.hpp"
#include "boot_hooks.h"
#include "funcs.h"
#include "scene_observer.hpp"
#include "widescreen.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>

namespace {
using tooie::features::Cheat;
using tooie::features::CheatReadout;
using tooie::features::CheatStatus;
using tooie::features::Boss;
using tooie::features::CutsceneAspect;
using tooie::features::Move;
using tooie::features::ProgressionReadout;
using tooie::features::ProgressionAction;
using tooie::features::RequestOutcome;
using tooie::features::World;
struct CheatRequest { Cheat cheat; bool enabled; bool unlock; };
struct ProgressionRequest { ProgressionAction action; std::uint32_t value = 0; };

std::mutex cheat_mutex;
// A permission change waits for any guest-thread application already in flight.
// Recursive because original guest calls can enter a file lifecycle hook.
std::recursive_mutex cheat_execution_mutex;
std::atomic_bool cheat_access_permission{false};
std::atomic_uint64_t request_sequence{0};
CheatReadout cheat_state;
std::optional<CheatRequest> pending_cheat;
bool refresh_cheats = true;
std::uint32_t cheat_ticks_since_scan = 0;
std::uint64_t cheat_generation = 0;
std::int32_t cheat_active_slot = -1;
std::mutex progression_mutex;
ProgressionReadout progression_state;
std::optional<ProgressionRequest> pending_progression;
bool refresh_progression = true;
std::uint32_t progression_ticks_since_scan = 0;
std::uint64_t progression_generation = 0;
std::atomic<CutsceneAspect> configured_aspect{CutsceneAspect::Widescreen};
std::atomic<CutsceneAspect> latched_aspect{CutsceneAspect::Widescreen};
std::atomic_bool pillarbox_available{false};
std::atomic_bool in_cutscene{false};

bool valid_cheat(Cheat cheat) noexcept {
    const auto value = static_cast<std::uint32_t>(cheat);
    return value >= 1 && value <= 12;
}

std::optional<std::size_t> move_index(Move move) noexcept {
    for (std::size_t i = 0; i < tooie::features::kMoves.size(); ++i)
        if (tooie::features::kMoves[i] == move) return i;
    return std::nullopt;
}

std::optional<std::size_t> world_index(World world) noexcept {
    const auto value = static_cast<std::uint32_t>(world);
    if (value < tooie::features::kWorlds.size()) return static_cast<std::size_t>(value);
    return std::nullopt;
}

std::optional<std::size_t> boss_index(Boss boss) noexcept {
    const auto value = static_cast<std::uint32_t>(boss);
    if (value < tooie::features::kBosses.size()) return static_cast<std::size_t>(value);
    return std::nullopt;
}

// Verified original encounter-completion bits. Zero denotes a boss whose
// ordinary completion cannot be edited independently of collected rewards or
// endgame story state, so requests for it are rejected.
constexpr std::array<std::uint32_t, tooie::features::kBosses.size()> boss_flags{
    0x318U, 0x319U, 0x31AU, 0x36FU, 0x086U, 0x09CU, 0x377U,
    0x0E5U, 0x18CU, 0x1D5U, 0x1D6U, 0U, 0x043U,
};
// Retry-flow bits are not defeat state. Clear the verified ones only when
// resetting an encounter, so actor initialization takes its fresh-fight path.
constexpr std::array<std::uint32_t, tooie::features::kBosses.size()> boss_retry_flags{
    0x0C1U, 0x1BAU, 0x0B7U, 0x088U, 0x179U, 0x17FU, 0x1AFU,
    0U, 0x174U, 0U, 0U, 0U, 0U,
};

// Source-verified physical traversal barriers: Hailfire Peaks entrance bridge,
// the two Pine Grove connecting-door states, Pine Grove's Talon Torpedo
// boulder, and the Cauldron Keep entrance drawbridge. This is an intentionally
// bounded set; anonymous gate/boulder flags and world-unlock bits are not
// guessed or changed.
constexpr std::array<std::uint32_t, 5> hub_connection_flags{
    0x391U, 0x3E9U, 0x3EAU, 0x3F0U, 0x436U,
};

// The factory main-door actor reads this original save flag at initialization
// and writes the same flag after its opening animation. An already-live actor
// needs an area reload to observe an external change.
constexpr std::uint32_t gi_front_door_flag = 0x1ABU;

// Original station switch/door flags: chghostdoor, chdinotraindoorswitch,
// chfactoryroofbits, chlavatraindoorswitch, chicestationbits, chhagstraindoorswit.
// Do not change Chuffy raised, Coal defeated, engine cooled, or parked-location bits.
constexpr std::array<std::uint32_t, tooie::features::kTrainStations.size()> train_station_flags{
    0x096U, 0x164U, 0x163U, 0x1D0U, 0x1CFU, 0x403U,
};

// chsilo reads and writes the actor marker index plus 0x32C. The seven
// ordinary silo markers are indices 1..7, hence the original save flags
// 0x32D..0x333. 0x334 is the separate first-time tutorial flag.
constexpr std::array<std::uint32_t, 7> silo_flags{
    0x32DU, 0x32EU, 0x32FU, 0x330U, 0x331U, 0x332U, 0x333U,
};

// chwarppad uses five persistent flag slots per world starting at 0x3AC.
// These are both the pad's activation bit and the destination-link bit read by
// the original travel menu. CCL and CK have two real entries; never set their
// three reserved slots merely because the storage stride is five.
constexpr std::array<std::uint32_t, tooie::features::kWorlds.size()>
    warp_pad_first_flags{
        0x3ACU, 0x3B1U, 0x3B6U, 0x3BBU, 0x3C0U,
        0x3C5U, 0x3CAU, 0x3CFU, 0x3D4U,
    };

// Named original boss maps whose encounter actor can be live. Klungo's actor
// selects its encounter from the source table at D_808038A0_chklungo, whose
// three entries are handled separately because one Boss row maps to each fight.
constexpr std::array<std::uint16_t, 3> klungo_arena_maps{
    0x141U, // Spiral Mountain Digger Tunnel
    0x15BU, // Isle o' Hags Another Digger Tunnel
    0x15EU, // Cauldron Keep Gatehouse
};
constexpr std::array<std::uint16_t, tooie::features::kBosses.size()> boss_arena_maps{
    0U, 0U, 0U,
    0x17AU, // Targitzan's Boss Temple
    0x0D1U, // Inside Chuffy's Boiler
    0x0F9U, // Witchyworld Big Top
    0x0FCU, // Lord Woo Fak Fak
    0x112U, // Terrydactyland (Terry encounter)
    0x10DU, // Grunty Industries Quality Control (Weldar)
    0x12CU, // Chilly Willy
    0x12BU, // Chilli Billi
    0U, 0U,
};

bool boss_edit_environment_safe(std::size_t index) noexcept {
    if (in_cutscene.load(std::memory_order_acquire)) return false;
    const auto scene = tooie::scene::snapshot();
    if (scene.activation_active) return false;
    if (index <= static_cast<std::size_t>(Boss::Klungo3) && scene.map_available &&
        std::find(klungo_arena_maps.begin(), klungo_arena_maps.end(), scene.map_id) !=
            klungo_arena_maps.end()) return false;
    const auto arena = boss_arena_maps[index];
    return arena == 0 || !scene.map_available || scene.map_id != arena;
}

constexpr std::array<std::uint32_t, 12> cheat_typeable_flags{
    0x06CU, 0x06DU, 0x06EU, 0x06FU, 0x070U, 0x076U, 0U, 0U, 0U, 0U, 0U, 0U,
};
constexpr std::array<std::uint32_t, 12> cheat_available_flags{
    0x071U, 0x072U, 0x073U, 0x074U, 0x075U, 0x077U,
    0x078U, 0x079U, 0x07AU, 0x042U, 0x04BU, 0x446U,
};

std::int32_t active_save_slot(uint8_t* rdram) noexcept {
    // Match the original save-manager admission already used by the project's
    // pause-save path: title/demo and unsupported transition modes are not a
    // current player file even if a PlayerState input tick exists.
    const auto mode = MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012762C)));
    const bool supported_mode = mode < 0x0FU || (mode < 0x1CU && mode != 0x11U);
    const auto slot = MEM_B(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F1)));
    return supported_mode ? slot : -1;
}

template <typename Function>
std::uint32_t call_guest_u32(Function function, uint8_t* rdram, recomp_context* ctx,
                             std::uint32_t a0, std::uint32_t a1 = 0, std::uint32_t a2 = 0) {
    const recomp_context saved = *ctx;
    ctx->r4 = static_cast<gpr>(static_cast<std::int32_t>(a0));
    ctx->r5 = static_cast<gpr>(static_cast<std::int32_t>(a1));
    ctx->r6 = static_cast<gpr>(static_cast<std::int32_t>(a2));
    try {
        function(rdram, ctx);
    } catch (...) {
        // Generated guest calls may cooperatively terminate the current guest
        // thread by exception. Do not let their scratch registers escape when
        // that control transfer unwinds through this native helper.
        *ctx = saved;
        throw;
    }
    const auto result = static_cast<std::uint32_t>(ctx->r2);
    *ctx = saved;
    return result;
}

std::uint32_t egg_item_for_type(std::uint32_t egg_type, uint8_t* rdram,
                                recomp_context* ctx) {
    const recomp_context saved = *ctx;
    ctx->r4 = static_cast<gpr>(static_cast<std::int32_t>(egg_type));
    // This is the original core callsite used by func_80094B14. The overlay
    // dispatcher needs it to resolve gcegg's entrypoint table unambiguously.
    tooie_overlay_callsite_push(0x80094B24U);
    try {
        _gcegg_entrypoint_5(rdram, ctx);
    } catch (...) {
        tooie_overlay_callsite_pop();
        *ctx = saved;
        throw;
    }
    const auto result = static_cast<std::uint32_t>(ctx->r2);
    tooie_overlay_callsite_pop();
    *ctx = saved;
    return result;
}

CheatStatus read_cheat(Cheat cheat, uint8_t* rdram, recomp_context* ctx) {
    const auto index = static_cast<std::uint32_t>(cheat);
    return {call_guest_u32(func_800D3DD0, rdram, ctx, index) != 0,
            call_guest_u32(func_800D3E14, rdram, ctx, index) != 0,
            call_guest_u32(func_800D3E40, rdram, ctx, index) != 0};
}

void scan_cheats(uint8_t* rdram, recomp_context* ctx) {
    std::array<CheatStatus, 12> snapshot{};
    for (std::uint32_t index = 1; index <= snapshot.size(); ++index)
        snapshot[index - 1] = read_cheat(static_cast<Cheat>(index), rdram, ctx);
    std::lock_guard lock(cheat_mutex);
    cheat_state.cheats = snapshot;
    cheat_state.current = true;
    cheat_state.observed = true;
    cheat_ticks_since_scan = 0;
}

void invalidate_cheats_locked() {
    const auto receipt = cheat_state.receipt;
    pending_cheat.reset();
    cheat_state = {};
    if (receipt.outcome == RequestOutcome::Queued) {
        cheat_state.receipt = receipt;
        cheat_state.receipt.outcome = RequestOutcome::Canceled;
    }
    cheat_state.generation = ++cheat_generation;
    refresh_cheats = true;
    cheat_ticks_since_scan = 0;
    cheat_active_slot = -1;
}

void invalidate_progression_locked() {
    const auto receipt = progression_state.receipt;
    pending_progression.reset();
    progression_state = {};
    if (receipt.outcome == RequestOutcome::Queued) {
        progression_state.receipt = receipt;
        progression_state.receipt.outcome = RequestOutcome::Canceled;
    }
    progression_state.generation = ++progression_generation;
    refresh_progression = true;
    progression_ticks_since_scan = 0;
}

std::uint32_t selected_egg_item(std::uint32_t player, uint8_t* rdram, recomp_context* ctx) {
    if (player == 0) return 0;
    const auto egg_type = call_guest_u32(func_80094510, rdram, ctx, player);
    // gcegg's table also contains special/non-player selector entries. The
    // ordinary selector returns types 0..5 (0/1 are both Blue Eggs); reject
    // anything else before indexing the overlay table.
    if (egg_type > 5U) return 0;
    const auto item = egg_item_for_type(egg_type, rdram, ctx);
    // Original gcegg data maps the five player ammo inventories to 0x40..0x44
    // (Blue, Fire, Ice, Grenade, Clockwork Kazooie).
    return item >= 0x40U && item <= 0x44U ? item : 0U;
}

void scan_progression(std::uint32_t player, uint8_t* rdram, recomp_context* ctx) {
    ProgressionReadout snapshot;
    for (std::size_t i = 0; i < tooie::features::kMoves.size(); ++i) {
        snapshot.moves[i] = call_guest_u32(func_800C6E38, rdram, ctx,
            static_cast<std::uint32_t>(tooie::features::kMoves[i])) != 0;
    }
    for (std::size_t i = 0; i < tooie::features::kWorlds.size(); ++i) {
        // FLAG_392_PROGRESS_OPENED_WORLD_MT through FLAG_39A_* are the nine
        // contiguous original world-open flags checked by Jiggywiggy doors.
        snapshot.worlds[i] = call_guest_u32(func_800DA298, rdram, ctx,
            0x392U + static_cast<std::uint32_t>(i)) != 0;
    }
    for (std::size_t i = 0; i < boss_flags.size(); ++i) {
        if (boss_flags[i] != 0)
            snapshot.bosses[i] = call_guest_u32(func_800DA298, rdram, ctx, boss_flags[i]) != 0;
        snapshot.boss_edits_available[i] =
            i <= static_cast<std::size_t>(Boss::ChilliBilli) && boss_edit_environment_safe(i);
    }
    // Mingy Jongo's ordinary actor removal is keyed to the collected Jiggy
    // record (0x47), rather than an independent defeated bit.
    snapshot.bosses[static_cast<std::size_t>(Boss::MingyJongo)] =
        call_guest_u32(func_800DA298, rdram, ctx, 0x296U) != 0;
    snapshot.jiggies = call_guest_u32(func_800D035C, rdram, ctx, 1U);
    snapshot.notes = call_guest_u32(func_800D035C, rdram, ctx, 6U);
    snapshot.honeycombs = call_guest_u32(func_800D035C, rdram, ctx, 2U);
    snapshot.jinjos = call_guest_u32(func_800D035C, rdram, ctx, 0U);
    for (std::uint32_t color = 0; color < snapshot.jinjo_family_total; ++color) {
        if (call_guest_u32(func_800D1338, rdram, ctx, color) ==
            call_guest_u32(func_800D129C, rdram, ctx, color)) ++snapshot.jinjo_families;
        const auto reward_flag = call_guest_u32(func_800D0A80, rdram, ctx, 0x51U + color, 1U);
        snapshot.jinjo_family_rewards +=
            call_guest_u32(func_800DA298, rdram, ctx, reward_flag) != 0;
    }
    const auto transformation = call_guest_u32(func_8008FD48, rdram, ctx, 0U);
    snapshot.current_health = call_guest_u32(func_800D4E7C, rdram, ctx, transformation);
    snapshot.health_capacity = call_guest_u32(func_800D4EB8, rdram, ctx, transformation);
    snapshot.health_upgrade_level = call_guest_u32(func_800DA564, rdram, ctx, 0x4EAU, 3U);
    snapshot.max_health_capacity = 10;
    snapshot.hub_connection_total = static_cast<std::uint32_t>(hub_connection_flags.size());
    for (const auto flag : hub_connection_flags)
        snapshot.hub_connections_open += call_guest_u32(func_800DA298, rdram, ctx, flag) != 0;
    snapshot.gi_front_door_open =
        call_guest_u32(func_800DA298, rdram, ctx, gi_front_door_flag) != 0;
    for (std::size_t i = 0; i < train_station_flags.size(); ++i)
        snapshot.train_stations_unlocked[i] =
            call_guest_u32(func_800DA298, rdram, ctx, train_station_flags[i]) != 0;
    snapshot.silo_total = static_cast<std::uint32_t>(silo_flags.size());
    for (const auto flag : silo_flags)
        snapshot.silos_unlocked += call_guest_u32(func_800DA298, rdram, ctx, flag) != 0;
    snapshot.warp_pad_totals = tooie::features::kWarpPadTotals;
    for (std::size_t world = 0; world < warp_pad_first_flags.size(); ++world) {
        for (std::uint32_t pad = 0; pad < snapshot.warp_pad_totals[world]; ++pad) {
            snapshot.warp_pads_active[world] += call_guest_u32(
                func_800DA298, rdram, ctx, warp_pad_first_flags[world] + pad) != 0;
        }
    }
    snapshot.selected_egg_item = selected_egg_item(player, rdram, ctx);
    if (snapshot.selected_egg_item != 0) {
        snapshot.selected_egg_ammo = call_guest_u32(
            func_800D1A04, rdram, ctx, snapshot.selected_egg_item);
        snapshot.selected_egg_capacity = call_guest_u32(
            func_800D1A6C, rdram, ctx, snapshot.selected_egg_item);
    }
    // HoneyKing is the original game's damage-path invincibility mode.
    snapshot.invincible = call_guest_u32(func_800D3E40, rdram, ctx, 9U) != 0;
    snapshot.current = true;
    snapshot.observed = true;

    std::lock_guard lock(progression_mutex);
    snapshot.generation = progression_state.generation;
    snapshot.applied_requests = progression_state.applied_requests;
    snapshot.request_pending = pending_progression.has_value();
    snapshot.receipt = progression_state.receipt;
    progression_state = snapshot;
    progression_ticks_since_scan = 0;
}

bool progression_target_met(const ProgressionRequest& request,
                            const ProgressionReadout& observed) noexcept {
    switch (request.action) {
        case ProgressionAction::Move:
            if (const auto index = move_index(static_cast<Move>(request.value)))
                return observed.moves[*index];
            return false;
        case ProgressionAction::AllMoves:
            return std::all_of(observed.moves.begin(), observed.moves.end(),
                [](bool learned) { return learned; });
        case ProgressionAction::World:
            return request.value < observed.worlds.size() && observed.worlds[request.value];
        case ProgressionAction::AllWorlds:
            return std::all_of(observed.worlds.begin(), observed.worlds.end(),
                [](bool open) { return open; });
        case ProgressionAction::AllNotes: return observed.notes >= observed.note_total;
        case ProgressionAction::AllJiggies: return observed.jiggies >= observed.jiggy_total;
        case ProgressionAction::AllHoneycombs:
            return observed.honeycombs >= observed.honeycomb_total;
        case ProgressionAction::MaxHealth:
            return observed.health_upgrade_level >= observed.health_upgrade_total;
        case ProgressionAction::RefillHealth:
            return observed.health_capacity != 0 &&
                observed.current_health >= observed.health_capacity;
        case ProgressionAction::AllJinjos:
            return observed.jinjos >= observed.jinjo_total &&
                observed.jinjo_families >= observed.jinjo_family_total &&
                observed.jinjo_family_rewards >= observed.jinjo_family_reward_total;
        case ProgressionAction::HubConnections:
            return observed.hub_connection_total != 0 &&
                observed.hub_connections_open >= observed.hub_connection_total;
        case ProgressionAction::GIFrontDoor: return observed.gi_front_door_open;
        case ProgressionAction::TrainStation:
            return request.value < observed.train_stations_unlocked.size() &&
                observed.train_stations_unlocked[request.value];
        case ProgressionAction::AllTrainStations:
            return std::all_of(observed.train_stations_unlocked.begin(),
                observed.train_stations_unlocked.end(), [](bool unlocked) { return unlocked; });
        case ProgressionAction::AllSilos:
            return observed.silo_total != 0 && observed.silos_unlocked >= observed.silo_total;
        case ProgressionAction::WorldWarpPads:
            return request.value < observed.warp_pads_active.size() &&
                observed.warp_pad_totals[request.value] != 0 &&
                observed.warp_pads_active[request.value] >=
                    observed.warp_pad_totals[request.value];
        case ProgressionAction::AllWarpPads:
            for (std::size_t i = 0; i < observed.warp_pads_active.size(); ++i)
                if (observed.warp_pad_totals[i] == 0 ||
                    observed.warp_pads_active[i] < observed.warp_pad_totals[i]) return false;
            return true;
        case ProgressionAction::BossState: {
            const auto index = request.value & 0xFFU;
            return index < observed.bosses.size() &&
                observed.bosses[index] == ((request.value & 0x100U) != 0);
        }
        case ProgressionAction::RefillSelectedEggs:
            return observed.selected_egg_capacity != 0 &&
                observed.selected_egg_ammo >= observed.selected_egg_capacity;
        case ProgressionAction::Invincibility:
            return observed.invincible == (request.value != 0);
    }
    return false;
}

void set_persistent_flag(std::uint32_t flag, uint8_t* rdram, recomp_context* ctx) {
    if (call_guest_u32(func_800DA298, rdram, ctx, flag) == 0)
        (void)call_guest_u32(func_800DA544, rdram, ctx, flag);
}

void complete_collectible_type(std::uint32_t type, uint8_t* rdram, recomp_context* ctx) {
    // The original collectible table is one-based. D0894 supplies the last
    // record and D0A80 maps each record to its precise persistent collected
    // flag. Setting those flags completes the collection without flooding the
    // HUD/message queue with one event per collectible.
    const auto last = call_guest_u32(func_800D0894, rdram, ctx, type);
    for (std::uint32_t record = 1; record <= last; ++record) {
        const auto flag = call_guest_u32(func_800D0A80, rdram, ctx, record, type);
        set_persistent_flag(flag, rdram, ctx);
    }
    // D0BD4 refreshes the original cached counter through D24E8 after setting
    // a collected flag. Do that once for the final bulk total, without its
    // per-Jiggy actor notification (B7) or dozens of intermediate HUD updates.
    if (type == 1U || type == 6U) {
        const auto total = call_guest_u32(func_800D035C, rdram, ctx, type);
        (void)call_guest_u32(func_800D24E8, rdram, ctx,
            type == 6U ? 0xD0U : 0xD6U, total, 0U);
    }
}

void refill_health(uint8_t* rdram, recomp_context* ctx) {
    const auto transformation = call_guest_u32(func_8008FD48, rdram, ctx, 0U);
    const auto current = call_guest_u32(func_800D4E7C, rdram, ctx, transformation);
    const auto capacity = call_guest_u32(func_800D4EB8, rdram, ctx, transformation);
    if (current < capacity)
        (void)call_guest_u32(func_800D4E18, rdram, ctx, transformation, capacity - current);
}

RequestOutcome apply_progression(const ProgressionRequest& request, std::uint32_t player,
                                 uint8_t* rdram, recomp_context* ctx) {
    switch (request.action) {
        case ProgressionAction::Move:
            (void)call_guest_u32(func_800C7074, rdram, ctx, request.value, 1U);
            break;
        case ProgressionAction::AllMoves:
            // This is the original game's own "give all abilities" routine.
            (void)call_guest_u32(func_800C7010, rdram, ctx, 0U);
            break;
        case ProgressionAction::World:
            set_persistent_flag(0x392U + request.value, rdram, ctx);
            break;
        case ProgressionAction::AllWorlds:
            for (std::uint32_t i = 0; i < tooie::features::kWorlds.size(); ++i)
                set_persistent_flag(0x392U + i, rdram, ctx);
            break;
        case ProgressionAction::AllNotes:
            complete_collectible_type(6U, rdram, ctx);
            break;
        case ProgressionAction::AllJiggies:
            complete_collectible_type(1U, rdram, ctx);
            break;
        case ProgressionAction::AllHoneycombs:
        {
            // Empty Honeycomb collection has two original state changes: the
            // persistent collectible flag and one spendable Honey B item
            // (0x49). Add only the newly completed records so upgrades already
            // purchased with earlier pieces remain spent, then let D1864 clamp
            // and refresh the ordinary inventory counter once.
            const auto before = call_guest_u32(func_800D035C, rdram, ctx, 2U);
            complete_collectible_type(2U, rdram, ctx);
            const auto after = call_guest_u32(func_800D035C, rdram, ctx, 2U);
            if (after > before) {
                constexpr std::uint32_t honeycomb_item = 0x49U;
                const auto current = call_guest_u32(
                    func_800D1A04, rdram, ctx, honeycomb_item);
                (void)call_guest_u32(func_800D1864, rdram, ctx,
                    honeycomb_item, current + (after - before), 1U);
            }
            break;
        }
        case ProgressionAction::MaxHealth:
        {
            std::array<std::uint32_t, 20> current_health{};
            for (std::uint32_t transformation = 0; transformation < current_health.size(); ++transformation)
                current_health[transformation] = call_guest_u32(
                    func_800D4E7C, rdram, ctx, transformation);
            (void)call_guest_u32(func_800DA7A8, rdram, ctx, 0x4EAU, 5U, 3U);
            // The original cache rebuild initializes both current and maximum
            // health. Restore every transformation's current-health byte so
            // the permanent capacity upgrade remains separate from refill.
            (void)call_guest_u32(func_800D517C, rdram, ctx, 0U);
            for (std::uint32_t transformation = 0; transformation < current_health.size(); ++transformation)
                (void)call_guest_u32(func_800D4FB8, rdram, ctx,
                    transformation, current_health[transformation]);
            break;
        }
        case ProgressionAction::RefillHealth:
            refill_health(rdram, ctx);
            break;
        case ProgressionAction::AllJinjos:
            complete_collectible_type(0U, rdram, ctx);
            // Match gcgamefix: a completed family grants its original Jiggy
            // record. Check the exact persistent collected flag: D0B68 also
            // reports true during cutscenes and replay, which could otherwise
            // skip a reward permanently. D0C78 preserves the game's normal
            // collected/spawn bookkeeping and message path.
            for (std::uint32_t color = 0; color < 9; ++color) {
                const auto record = 0x51U + color;
                const auto collected_flag = call_guest_u32(func_800D0A80, rdram, ctx, record, 1U);
                if (call_guest_u32(func_800DA298, rdram, ctx, collected_flag) == 0)
                    (void)call_guest_u32(func_800D0C78, rdram, ctx, record, 1U, 1U);
            }
            break;
        case ProgressionAction::HubConnections:
            for (const auto flag : hub_connection_flags) set_persistent_flag(flag, rdram, ctx);
            break;
        case ProgressionAction::GIFrontDoor:
            set_persistent_flag(gi_front_door_flag, rdram, ctx);
            break;
        case ProgressionAction::TrainStation:
            if (request.value < train_station_flags.size())
                set_persistent_flag(train_station_flags[request.value], rdram, ctx);
            break;
        case ProgressionAction::AllTrainStations:
            for (const auto flag : train_station_flags) set_persistent_flag(flag, rdram, ctx);
            break;
        case ProgressionAction::AllSilos:
            for (const auto flag : silo_flags) set_persistent_flag(flag, rdram, ctx);
            break;
        case ProgressionAction::WorldWarpPads: {
            const auto world = static_cast<std::size_t>(request.value);
            for (std::uint32_t pad = 0; pad < tooie::features::kWarpPadTotals[world]; ++pad)
                set_persistent_flag(warp_pad_first_flags[world] + pad, rdram, ctx);
            break;
        }
        case ProgressionAction::AllWarpPads:
            for (std::size_t world = 0; world < warp_pad_first_flags.size(); ++world) {
                for (std::uint32_t pad = 0; pad < tooie::features::kWarpPadTotals[world]; ++pad)
                    set_persistent_flag(warp_pad_first_flags[world] + pad, rdram, ctx);
            }
            break;
        case ProgressionAction::BossState: {
            const auto index = request.value & 0xFFU;
            // Recheck on the guest thread: the player can enter an arena or a
            // scene transition after the frontend queues the request.
            if (!boss_edit_environment_safe(index)) return RequestOutcome::Rejected;
            const bool defeated = (request.value & 0x100U) != 0;
            (void)call_guest_u32(func_800DA3B8, rdram, ctx, boss_flags[index], defeated ? 1U : 0U);
            if (!defeated && boss_retry_flags[index] != 0)
                (void)call_guest_u32(func_800DA3B8, rdram, ctx, boss_retry_flags[index], 0U);
            break;
        }
        case ProgressionAction::RefillSelectedEggs:
            if (const auto item = selected_egg_item(player, rdram, ctx); item != 0) {
                const auto capacity = call_guest_u32(func_800D1A6C, rdram, ctx, item);
                if (capacity == 0) return RequestOutcome::Rejected;
                (void)call_guest_u32(func_800D1864, rdram, ctx, item, capacity, 1U);
            } else {
                return RequestOutcome::Rejected;
            }
            break;
        case ProgressionAction::Invincibility:
            (void)call_guest_u32(func_800D3F58, rdram, ctx, 9U, request.value != 0 ? 1U : 0U);
            (void)call_guest_u32(func_800D3FD4, rdram, ctx, 1U);
            return read_cheat(Cheat::HoneyKing, rdram, ctx).active == (request.value != 0)
                ? RequestOutcome::Applied : RequestOutcome::Failed;
    }
    return RequestOutcome::Applied;
}

bool request_progression(ProgressionRequest request) noexcept {
    std::scoped_lock lock(cheat_mutex, progression_mutex);
    if (!cheat_access_permission.load(std::memory_order_acquire) ||
        !progression_state.current || !progression_state.observed ||
        progression_state.request_pending || pending_progression) return false;
    if (request.action == ProgressionAction::Invincibility && pending_cheat &&
        pending_cheat->cheat == Cheat::HoneyKing) return false;
    pending_progression = request;
    progression_state.request_pending = true;
    progression_state.receipt = {++request_sequence,
        static_cast<std::uint32_t>(request.action), request.value,
        RequestOutcome::Queued};
    return true;
}

} // namespace

namespace tooie::features {
void set_cheats_access_enabled(bool enabled) noexcept {
    std::lock_guard execution_lock(cheat_execution_mutex);
    std::scoped_lock state_lock(cheat_mutex, progression_mutex);
    if (!enabled) {
        if (pending_cheat) {
            pending_cheat.reset();
            cheat_state.receipt.outcome = RequestOutcome::Canceled;
        }
        if (pending_progression) {
            pending_progression.reset();
            progression_state.receipt.outcome = RequestOutcome::Canceled;
        }
        cheat_state.request_pending = false;
        progression_state.request_pending = false;
        cheat_state.generation = ++cheat_generation;
        progression_state.generation = ++progression_generation;
    }
    cheat_access_permission.store(enabled, std::memory_order_release);
}
bool cheats_access_enabled() noexcept {
    return cheat_access_permission.load(std::memory_order_acquire);
}
BossSupport boss_support(Boss boss) noexcept {
    const auto index = boss_index(boss);
    if (!index) return {};
    if (*index <= static_cast<std::size_t>(Boss::ChilliBilli) &&
        !boss_edit_environment_safe(*index)) {
        return {false, false,
            "Leave this boss's arena and wait for any cutscene or loading screen to finish.",
            "Leave this boss's arena and wait for any cutscene or loading screen to finish."};
    }
    if (*index <= static_cast<std::size_t>(Boss::Klungo3)) {
        return {true, true,
            "Mark this Klungo encounter as beaten. Other story progress stays unchanged. Leave and re-enter the area afterward.",
            "Make this Klungo encounter available again. Leave and re-enter the area before replaying it."};
    }
    if (*index <= static_cast<std::size_t>(Boss::ChilliBilli)) {
        return {true, true,
            "Mark this boss as beaten. This does not grant rewards or complete other story objectives. Leave and re-enter the area afterward.",
            "Make this encounter available again. Rewards and other story progress stay unchanged. Leave and re-enter the area before replaying it."};
    }
    if (boss == Boss::MingyJongo) {
        return {false, false,
            "Changing this boss would also change its Jiggy. This tool cannot do that yet.",
            "Replaying this boss without removing its Jiggy is not available yet."};
    }
    return {false, false,
        "Changing the final boss also changes the ending. This tool cannot do that yet.",
        "Replaying the final boss without changing the ending is not available yet."};
}
bool request_cheat_enabled(Cheat cheat, bool enabled) noexcept {
    if (!valid_cheat(cheat)) return false;
    std::scoped_lock lock(cheat_mutex, progression_mutex);
    // Do not accept launcher/title requests or overwrite an in-flight guest
    // request. The first valid player update publishes original game state.
    if (!cheat_access_permission.load(std::memory_order_acquire) ||
        !cheat_state.current || !cheat_state.observed ||
        cheat_state.request_pending || pending_cheat) return false;
    if (cheat == Cheat::HoneyKing && pending_progression &&
        pending_progression->action == ProgressionAction::Invincibility) return false;
    if (!cheat_state.cheats[static_cast<std::size_t>(cheat) - 1].available) {
        ++cheat_state.rejected_locked_requests;
        return false;
    }
    pending_cheat = CheatRequest{cheat, enabled, false};
    cheat_state.request_pending = true;
    cheat_state.receipt = {++request_sequence, static_cast<std::uint32_t>(cheat),
        enabled ? 1U : 0U, RequestOutcome::Queued};
    return true;
}
bool request_unlock_and_enable_cheat(Cheat cheat) noexcept {
    if (!valid_cheat(cheat)) return false;
    std::scoped_lock lock(cheat_mutex, progression_mutex);
    if (!cheat_access_permission.load(std::memory_order_acquire) ||
        !cheat_state.current || !cheat_state.observed ||
        cheat_state.request_pending || pending_cheat) return false;
    if (cheat == Cheat::HoneyKing && pending_progression &&
        pending_progression->action == ProgressionAction::Invincibility) return false;
    pending_cheat = CheatRequest{cheat, true, true};
    cheat_state.request_pending = true;
    cheat_state.receipt = {++request_sequence, static_cast<std::uint32_t>(cheat),
        1U, RequestOutcome::Queued};
    return true;
}
void request_cheat_refresh() noexcept { std::lock_guard lock(cheat_mutex); refresh_cheats = true; }
CheatReadout cheat_readout() noexcept { std::lock_guard lock(cheat_mutex); return cheat_state; }
bool request_unlock_move(Move move) noexcept {
    const auto index = move_index(move);
    if (!index) return false;
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.moves[*index]) return false;
    }
    return request_progression({ProgressionAction::Move, static_cast<std::uint32_t>(move)});
}
bool request_unlock_all_moves() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            std::all_of(progression_state.moves.begin(), progression_state.moves.end(),
                [](bool learned) { return learned; })) return false;
    }
    return request_progression({ProgressionAction::AllMoves});
}
bool request_unlock_world(World world) noexcept {
    const auto index = world_index(world);
    if (!index) return false;
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.worlds[*index]) return false;
    }
    return request_progression({ProgressionAction::World, static_cast<std::uint32_t>(*index)});
}
bool request_unlock_all_worlds() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            std::all_of(progression_state.worlds.begin(), progression_state.worlds.end(),
                [](bool open) { return open; })) return false;
    }
    return request_progression({ProgressionAction::AllWorlds});
}
bool request_collect_all_notes() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            progression_state.notes >= progression_state.note_total) return false;
    }
    return request_progression({ProgressionAction::AllNotes});
}
bool request_collect_all_jiggies() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            progression_state.jiggies >= progression_state.jiggy_total) return false;
    }
    return request_progression({ProgressionAction::AllJiggies});
}
bool request_collect_all_honeycombs() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            progression_state.honeycombs >= progression_state.honeycomb_total) return false;
    }
    return request_progression({ProgressionAction::AllHoneycombs});
}
bool request_upgrade_max_health() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.health_upgrade_level >=
            progression_state.health_upgrade_total) return false;
    }
    return request_progression({ProgressionAction::MaxHealth});
}
bool request_refill_health() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            progression_state.current_health >= progression_state.health_capacity) return false;
    }
    return request_progression({ProgressionAction::RefillHealth});
}
bool request_collect_all_jinjos() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.jinjos >= progression_state.jinjo_total &&
            progression_state.jinjo_families >= progression_state.jinjo_family_total &&
            progression_state.jinjo_family_rewards >=
                progression_state.jinjo_family_reward_total) return false;
    }
    return request_progression({ProgressionAction::AllJinjos});
}
bool request_unlock_all_hub_connections() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.hub_connection_total != 0 &&
            progression_state.hub_connections_open >= progression_state.hub_connection_total) return false;
    }
    return request_progression({ProgressionAction::HubConnections});
}
bool request_open_gi_front_door() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.gi_front_door_open) return false;
    }
    return request_progression({ProgressionAction::GIFrontDoor});
}
bool request_unlock_train_station(TrainStation station) noexcept {
    const auto index = static_cast<std::uint32_t>(station);
    if (index >= kTrainStations.size()) return false;
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.train_stations_unlocked[index]) return false;
    }
    return request_progression({ProgressionAction::TrainStation, index});
}
bool request_unlock_all_train_stations() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            std::all_of(progression_state.train_stations_unlocked.begin(),
                progression_state.train_stations_unlocked.end(), [](bool unlocked) { return unlocked; })) return false;
    }
    return request_progression({ProgressionAction::AllTrainStations});
}
bool request_unlock_all_silos() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.silo_total != 0 &&
            progression_state.silos_unlocked >= progression_state.silo_total) return false;
    }
    return request_progression({ProgressionAction::AllSilos});
}
bool request_activate_world_warp_pads(World world) noexcept {
    const auto index = world_index(world);
    if (!index) return false;
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed &&
            progression_state.warp_pads_active[*index] >=
                progression_state.warp_pad_totals[*index]) return false;
    }
    return request_progression({ProgressionAction::WorldWarpPads,
        static_cast<std::uint32_t>(*index)});
}
bool request_activate_all_warp_pads() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed) {
            bool complete = true;
            for (std::size_t i = 0; i < progression_state.warp_pads_active.size(); ++i)
                complete = complete && progression_state.warp_pads_active[i] >=
                    progression_state.warp_pad_totals[i];
            if (complete) return false;
        }
    }
    return request_progression({ProgressionAction::AllWarpPads});
}
bool request_boss_defeated(Boss boss, bool defeated) noexcept {
    const auto index = boss_index(boss);
    if (!index || *index > static_cast<std::size_t>(Boss::ChilliBilli)) return false;
    // Never alter encounter bookkeeping while original cutscene actors and
    // scripts may still be consuming it. Area reload remains required because
    // a live boss actor is not retroactively created/deleted by a flag write.
    if (!boss_edit_environment_safe(*index)) return false;
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.bosses[*index] == defeated) return false;
    }
    return request_progression({ProgressionAction::BossState,
        static_cast<std::uint32_t>(*index) | (defeated ? 0x100U : 0U)});
}
bool request_refill_selected_eggs() noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && (progression_state.selected_egg_capacity == 0 ||
            progression_state.selected_egg_ammo >= progression_state.selected_egg_capacity)) return false;
    }
    return request_progression({ProgressionAction::RefillSelectedEggs});
}
bool request_invincibility_enabled(bool enabled) noexcept {
    {
        std::lock_guard lock(progression_mutex);
        if (progression_state.observed && progression_state.invincible == enabled) return false;
    }
    return request_progression({ProgressionAction::Invincibility, enabled ? 1U : 0U});
}
void request_progression_refresh() noexcept {
    std::lock_guard lock(progression_mutex); refresh_progression = true;
}
ProgressionReadout progression_readout() noexcept {
    std::lock_guard lock(progression_mutex); return progression_state;
}
void configure_cutscene_aspect(CutsceneAspect aspect) noexcept {
    configured_aspect.store(aspect == CutsceneAspect::Widescreen
        ? CutsceneAspect::Widescreen : CutsceneAspect::Original, std::memory_order_release);
}
void latch_for_game_start() noexcept {
    std::lock_guard execution_lock(cheat_execution_mutex);
    latched_aspect.store(configured_aspect.load(std::memory_order_acquire), std::memory_order_release);
    in_cutscene.store(false, std::memory_order_release);
    tooie::camera_interpolation::set_cutscene_active(false);
    std::lock_guard lock(cheat_mutex);
    invalidate_cheats_locked();
    std::lock_guard progression_lock(progression_mutex);
    invalidate_progression_locked();
}
CutsceneAspect configured_cutscene_aspect() noexcept { return configured_aspect.load(std::memory_order_acquire); }
CutsceneAspect latched_cutscene_aspect() noexcept { return latched_aspect.load(std::memory_order_acquire); }
void set_cutscene_pillarbox_available(bool available) noexcept {
    pillarbox_available.store(available, std::memory_order_release);
}
bool cutscene_pillarbox_available() noexcept { return pillarbox_available.load(std::memory_order_acquire); }
bool cutscene_active() noexcept { return in_cutscene.load(std::memory_order_acquire); }
bool cutscene_requires_pillarbox() noexcept {
    return cutscene_pillarbox_available() && cutscene_active() &&
        latched_cutscene_aspect() == CutsceneAspect::Original &&
        tooie::widescreen::latched_enabled();
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_requests_idle() noexcept {
    std::lock_guard execution_lock(cheat_execution_mutex);
    std::scoped_lock state_lock(cheat_mutex, progression_mutex);
    return !pending_cheat && !pending_progression &&
        !cheat_state.request_pending && !progression_state.request_pending;
}

void persistent_restore_epoch(bool active) noexcept {
    std::lock_guard execution_lock(cheat_execution_mutex);
    std::scoped_lock state_lock(cheat_mutex, progression_mutex);
    invalidate_cheats_locked();
    invalidate_progression_locked();
    in_cutscene.store(active, std::memory_order_release);
    tooie::camera_interpolation::set_cutscene_active(active);
}
#endif
} // namespace tooie::features

extern "C" void tooie_cheats_tick(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard execution_lock(cheat_execution_mutex);
    // The hook runs at bainput_update entry, where a0 is the live PlayerState.
    const auto player = static_cast<std::uint32_t>(ctx->r4);
    const auto slot = active_save_slot(rdram);
    bool invalidate_progression = false;
    {
        std::lock_guard lock(cheat_mutex);
        if (slot < 0) {
            if (cheat_state.current || cheat_state.observed ||
                cheat_state.request_pending || pending_cheat)
                invalidate_cheats_locked();
            invalidate_progression = true;
        } else {
            if (cheat_active_slot >= 0 && cheat_active_slot != slot) {
                invalidate_cheats_locked();
                invalidate_progression = true;
            }
            cheat_active_slot = slot;
        }
    }
    if (invalidate_progression) {
        std::lock_guard lock(progression_mutex);
        if (progression_state.current || progression_state.observed ||
            progression_state.request_pending || pending_progression)
            invalidate_progression_locked();
    }
    if (slot < 0) return;
    std::optional<CheatRequest> request;
    bool refresh = false;
    {
        std::lock_guard lock(cheat_mutex);
        request.swap(pending_cheat);
        refresh = refresh_cheats; refresh_cheats = false;
        if (!refresh && ++cheat_ticks_since_scan >= 60) refresh = true;
    }
    try {
        if (request) {
            const auto index = static_cast<std::uint32_t>(request->cheat);
            if (request->unlock) {
                const auto table_index = static_cast<std::size_t>(index - 1U);
                if (cheat_typeable_flags[table_index] != 0)
                    set_persistent_flag(cheat_typeable_flags[table_index], rdram, ctx);
                set_persistent_flag(cheat_available_flags[table_index], rdram, ctx);
            }
            const auto before = read_cheat(request->cheat, rdram, ctx);
            // The original menu renders typeable-only entries with its locked
            // option state (entrypoint 23). Only the available flag reaches the
            // on/off controls (entrypoints 21/22), so match that authorization.
            if (!before.available) {
                std::lock_guard lock(cheat_mutex);
                ++cheat_state.rejected_locked_requests;
                cheat_state.receipt.outcome = RequestOutcome::Rejected;
            } else if (before.active != request->enabled) {
                (void)call_guest_u32(func_800D3F58, rdram, ctx, index, request->enabled ? 1U : 0U);
                (void)call_guest_u32(func_800D3FD4, rdram, ctx, 1U);
                const auto after = read_cheat(request->cheat, rdram, ctx);
                std::lock_guard lock(cheat_mutex);
                if (after.active == request->enabled) {
                    ++cheat_state.applied_requests;
                    cheat_state.receipt.outcome = RequestOutcome::Applied;
                } else {
                    cheat_state.receipt.outcome = RequestOutcome::Failed;
                }
            } else {
                std::lock_guard lock(cheat_mutex);
                cheat_state.receipt.outcome = RequestOutcome::Unchanged;
            }
            refresh = true;
            if (request->cheat == Cheat::HoneyKing) {
                std::lock_guard lock(progression_mutex);
                refresh_progression = true;
            }
        }
        if (refresh) scan_cheats(rdram, ctx);
    } catch (...) {
        // The request has already left the queue. A cooperative guest unwind
        // must not leave the frontend permanently reporting it as pending.
        std::lock_guard lock(cheat_mutex);
        if (request) cheat_state.receipt.outcome = RequestOutcome::Failed;
        cheat_state.request_pending = pending_cheat.has_value();
        refresh_cheats = true;
        throw;
    }
    {
        std::lock_guard lock(cheat_mutex);
        cheat_state.request_pending = pending_cheat.has_value();
    }

    std::optional<ProgressionRequest> progression_request;
    bool progression_refresh = false;
    {
        std::lock_guard lock(progression_mutex);
        progression_request.swap(pending_progression);
        progression_refresh = refresh_progression;
        refresh_progression = false;
        if (!progression_refresh && ++progression_ticks_since_scan >= 60)
            progression_refresh = true;
    }
    try {
        if (progression_request) {
            const auto outcome = apply_progression(*progression_request, player, rdram, ctx);
            {
                std::lock_guard lock(progression_mutex);
                if (outcome == RequestOutcome::Applied) ++progression_state.applied_requests;
                progression_state.receipt.outcome = outcome;
            }
            progression_refresh = true;
            if (progression_request->action == ProgressionAction::Invincibility) {
                // The Player control and Honey King cheat are aliases for the
                // same original active bit. Publish both readouts this tick.
                scan_cheats(rdram, ctx);
            }
        }
        if (progression_refresh) scan_progression(player, rdram, ctx);
        if (progression_request) {
            std::lock_guard lock(progression_mutex);
            if (progression_state.receipt.outcome == RequestOutcome::Applied &&
                !progression_target_met(*progression_request, progression_state)) {
                progression_state.receipt.outcome = RequestOutcome::Failed;
                --progression_state.applied_requests;
            }
        }
    } catch (...) {
        // Bulk operations may have partially completed before an original
        // guest routine unwinds, so do not retry them implicitly. Clear the
        // consumed request and force an observed-state refresh next tick.
        std::lock_guard lock(progression_mutex);
        if (progression_request) progression_state.receipt.outcome = RequestOutcome::Failed;
        progression_state.request_pending = pending_progression.has_value();
        refresh_progression = true;
        throw;
    }
    {
        std::lock_guard lock(progression_mutex);
        progression_state.request_pending = pending_progression.has_value();
    }
}

extern "C" void tooie_cheats_invalidate() {
    std::lock_guard execution_lock(cheat_execution_mutex);
    // A file/title lifecycle boundary starts a new renderer history even when
    // the allocator later reuses the same guest camera address.
    tooie::camera_interpolation::observe_active_camera(0);
    // Save/progression invalidation can run while glcut remains active. Mirror
    // the authoritative state; only original cutscene commands own entry/exit.
    tooie::camera_interpolation::set_cutscene_active(
        in_cutscene.load(std::memory_order_acquire));
    tooie::camera_interpolation::request_skip();
    {
        std::lock_guard lock(cheat_mutex);
        invalidate_cheats_locked();
    }
    {
        std::lock_guard lock(progression_mutex);
        invalidate_progression_locked();
    }
}

extern "C" void tooie_cutscene_state_command(uint8_t* rdram, recomp_context* ctx, std::uint32_t command) {
    switch (command) {
        case 2: {
            in_cutscene.store(true, std::memory_order_release);
            tooie::camera_interpolation::set_cutscene_active(true);
            tooie::camera_interpolation::request_skip();
            // Use the game's own aspect switch so every original reader of
            // `widescreen_enabled` (perspective, ortho and HUD compensation)
            // observes one coherent 4:3 cutscene state. Presentation bars are
            // applied separately by the RT64 owner thread.
            if (tooie::features::cutscene_requires_pillarbox())
                (void)call_guest_u32(set_widescreen, rdram, ctx, 0U);
            break;
        }
        case 1: case 4: case 5: {
            const bool was_active = in_cutscene.exchange(false, std::memory_order_acq_rel);
            tooie::camera_interpolation::set_cutscene_active(false);
            if (was_active) tooie::camera_interpolation::request_skip();
            if (was_active && tooie::widescreen::latched_enabled())
                (void)call_guest_u32(set_widescreen, rdram, ctx, 1U);
            break;
        }
        default: break;
    }
}
