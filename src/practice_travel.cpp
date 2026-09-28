#include "practice_travel.hpp"
#include "practice_state.hpp"

#include "game_features.hpp"
#include "scene_observer.hpp"

#include "funcs.h"
#include "recomp.h"

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

namespace {
using tooie::practice::travel::Destination;
using tooie::practice::travel::Outcome;
using tooie::practice::travel::Status;

// Decoded from D_808010B0_chwarppad in the source ROM using
// func_80800E10_chwarppad. Entry 0 in each world group is a sentinel.
// The final seven pairs are decoded from D_808012EC_chsilo / D_808012E8_chsilo
// using func_80800000_chsilo and its _gcgoto_entrypoint_1 call. No entrance is
// invented from a map name or guessed from an adjacent map.
constexpr std::array<Destination, 46> destinations{{
    {"Mayahem Temple", "Main area", 0x0B8, 30, 1},
    {"Mayahem Temple", "Main area", 0x0B8, 31, 2},
    {"Mayahem Temple", "Prison Compound", 0x0B9, 32, 3},
    {"Mayahem Temple", "Jade Snake Grove", 0x0C4, 33, 4},
    {"Mayahem Temple", "Kickball Stadium", 0x0BB, 34, 5},
    {"Glitter Gulch Mine", "Main area", 0x0C7, 30, 1},
    {"Glitter Gulch Mine", "Main area", 0x0C7, 31, 2},
    {"Glitter Gulch Mine", "Wumba's", 0x0E9, 32, 3},
    {"Glitter Gulch Mine", "Main area", 0x0C7, 33, 4},
    {"Glitter Gulch Mine", "Main area", 0x0C7, 34, 5},
    {"Witchyworld", "Main area", 0x0D6, 30, 1},
    {"Witchyworld", "Main area", 0x0D6, 31, 2},
    {"Witchyworld", "Main area", 0x0D6, 32, 3},
    {"Witchyworld", "Main area", 0x0D6, 33, 4},
    {"Witchyworld", "Inferno", 0x0E7, 34, 5},
    {"Jolly Roger's Lagoon", "Main area", 0x1A7, 30, 1},
    {"Jolly Roger's Lagoon", "Atlantis", 0x1A8, 31, 2},
    {"Jolly Roger's Lagoon", "Atlantis", 0x1A8, 32, 3},
    {"Jolly Roger's Lagoon", "Sea Bottom", 0x1A9, 33, 4},
    {"Jolly Roger's Lagoon", "Sea Bottom", 0x1A9, 34, 5},
    {"Terrydactyland", "Main area", 0x112, 30, 1},
    {"Terrydactyland", "Stomping Plains", 0x11A, 31, 2},
    {"Terrydactyland", "Main area", 0x112, 32, 3},
    {"Terrydactyland", "Main area", 0x112, 33, 4},
    {"Terrydactyland", "Main area", 0x112, 34, 5},
    {"Grunty Industries", "Inside", 0x101, 30, 1},
    {"Grunty Industries", "Floor 2", 0x106, 31, 2},
    {"Grunty Industries", "Floor 3", 0x108, 32, 3},
    {"Grunty Industries", "Floor 4", 0x10B, 33, 4},
    {"Grunty Industries", "Outside", 0x100, 34, 5},
    {"Hailfire Peaks", "Lava Side", 0x127, 30, 1},
    {"Hailfire Peaks", "Lava Side", 0x127, 31, 2},
    {"Hailfire Peaks", "Ice Side", 0x128, 32, 3},
    {"Hailfire Peaks", "Ice Side", 0x128, 33, 4},
    {"Hailfire Peaks", "Icicle Grotto", 0x132, 34, 5},
    {"Cloud Cuckooland", "Main area", 0x136, 30, 1},
    {"Cloud Cuckooland", "Central Cavern", 0x13A, 31, 2},
    {"Cauldron Keep", "Main area", 0x15D, 30, 1},
    {"Cauldron Keep", "Main area", 0x15D, 31, 2},
    {"Isle o' Hags", "Jinjo Village", 0x142, 4, 1, true},
    {"Isle o' Hags", "Wooded Hollow", 0x14F, 6, 2, true},
    {"Isle o' Hags", "Plateau", 0x152, 5, 3, true},
    {"Isle o' Hags", "Pine Grove", 0x154, 5, 4, true},
    {"Isle o' Hags", "Cliff Top", 0x155, 7, 5, true},
    {"Isle o' Hags", "Wasteland", 0x15A, 4, 6, true},
    {"Isle o' Hags", "Quagmire", 0x15C, 4, 7, true},
}};

std::mutex travel_mutex;
Status latest;
std::optional<std::size_t> queued;
std::chrono::steady_clock::time_point dispatched_at{};
std::chrono::steady_clock::time_point queued_at{};
std::uint64_t activation_count_at_dispatch = 0;

bool gameplay_mode(std::uint8_t* rdram) noexcept {
    // Same original mode/slot admission used by game_features' live actions.
    const auto mode = MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012762CU)));
    const auto slot = MEM_B(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F1U)));
    return (mode < 0x0FU || (mode < 0x1CU && mode != 0x11U)) && slot >= 0;
}

template <typename Function>
void guest_call(Function function, std::uint8_t* rdram, recomp_context* ctx,
    std::uint32_t map_id, std::uint32_t entrance_id) {
    const recomp_context saved = *ctx;
    ctx->r4 = static_cast<gpr>(map_id);
    ctx->r5 = static_cast<gpr>(entrance_id);
    ctx->r6 = 1; // Same transition selector as gcgoto_entrypoint_1.
    try {
        function(rdram, ctx);
    } catch (...) {
        *ctx = saved;
        throw;
    }
    *ctx = saved;
}
} // namespace

namespace tooie::practice::travel {

std::size_t destination_count() noexcept { return destinations.size(); }

const Destination* destination(std::size_t index) noexcept {
    return index < destinations.size() ? &destinations[index] : nullptr;
}

bool request(std::size_t index) noexcept {
    if (index >= destinations.size() || tooie::features::cutscene_active()) return false;
    const auto scene = tooie::scene::snapshot();
    if (!scene.map_available || scene.activation_active) return false;
    std::lock_guard lock(travel_mutex);
    if (queued || latest.outcome == Outcome::Queued ||
        latest.outcome == Outcome::TransitionRequested) return false;
    if (!tooie::practice::try_reserve_transition(
        tooie::practice::TransitionOwner::Warp)) return false;
    queued = index;
    latest = {Outcome::Queued, index, latest.request_number + 1};
    queued_at = std::chrono::steady_clock::now();
    return true;
}

Status status() noexcept {
    std::lock_guard lock(travel_mutex);
    if (queued && std::chrono::steady_clock::now() - queued_at >
        std::chrono::seconds(20)) {
        queued.reset();
        latest.outcome = Outcome::TimedOut;
        tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
    }
    return latest;
}

void tick(std::uint8_t* rdram, recomp_context* ctx) {
    if (!rdram || !ctx) return;
    const auto scene = tooie::scene::snapshot();
    std::optional<std::size_t> to_dispatch;
    {
        std::lock_guard lock(travel_mutex);
        if (latest.outcome == Outcome::TransitionRequested) {
            if (scene.map_available && !scene.activation_active &&
                scene.activations_completed > activation_count_at_dispatch &&
                scene.map_id == destinations[latest.destination_index].map_id) {
                latest.outcome = Outcome::Arrived;
                tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
            } else if (std::chrono::steady_clock::now() - dispatched_at >
                std::chrono::seconds(20)) {
                latest.outcome = Outcome::TimedOut;
                tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
            }
        }
        if (queued) {
            if (std::chrono::steady_clock::now() - queued_at >
                std::chrono::seconds(20)) {
                queued.reset();
                latest.outcome = Outcome::TimedOut;
                tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
            } else {
                to_dispatch = *queued;
                queued.reset();
                // Keep latest as Queued while the guest call is in flight.
                // request() rejects this state, even after queued is cleared.
            }
        }
    }
    if (!to_dispatch) return;
    const auto index = *to_dispatch;
    if (!gameplay_mode(rdram) || tooie::features::cutscene_active() ||
        !scene.map_available || scene.activation_active ||
        MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x801275C0U))) != 0U) {
        std::lock_guard lock(travel_mutex);
        latest.outcome = Outcome::Rejected;
        tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
        return;
    }
    try {
        // The original warp pad calls gcgoto_entrypoint_1. Its map rewrite only
        // applies to map 0xFB (none of these catalog destinations). Reproduce
        // the remaining core transition and two state writes in the same order;
        // calling the gc overlay entrypoint from an arbitrary loaded world is
        // unsafe because that overlay is not necessarily resident.
        guest_call(func_800A7990, rdram, ctx,
            destinations[index].map_id, destinations[index].entrance_id);
        MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(0x80132DCAU))) = 0;
        MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x801275C0U))) = 1U;
    } catch (...) {
        std::lock_guard lock(travel_mutex);
        latest.outcome = Outcome::Rejected;
        tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
        throw;
    }
    std::lock_guard lock(travel_mutex);
    latest.outcome = Outcome::TransitionRequested;
    dispatched_at = std::chrono::steady_clock::now();
    activation_count_at_dispatch = scene.activations_completed;
}

void reset() noexcept {
    std::lock_guard lock(travel_mutex);
    latest = {};
    queued.reset();
    dispatched_at = {};
    queued_at = {};
    activation_count_at_dispatch = 0;
    tooie::practice::release_transition(tooie::practice::TransitionOwner::Warp);
}

} // namespace tooie::practice::travel
