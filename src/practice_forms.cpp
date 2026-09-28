#include "practice_forms.hpp"

#include "practice_state.hpp"
#include "game_features.hpp"
#include "scene_observer.hpp"

#include "funcs.h"

#include <array>
#include <chrono>
#include <mutex>

namespace {
using tooie::practice::forms::Form;
using tooie::practice::forms::Outcome;
using tooie::practice::forms::Failure;
using tooie::practice::forms::Status;

// These are the defined TRANSFORM_* values in Tooie's transformations.h and
// all have form-specific rows in ba/data.c. Values 3-5 are not defined forms.
constexpr std::array<Form, 16> catalog{{
    {0x01, "Banjo & Kazooie"}, {0x02, "Snowball"}, {0x06, "Bee"},
    {0x07, "Washing Machine"}, {0x08, "Stony"}, {0x09, "First Person"},
    {0x0A, "Banjo"}, {0x0B, "Kazooie"}, {0x0C, "Submarine"},
    {0x0D, "Mumbo"}, {0x0E, "Golden Goliath"}, {0x0F, "Detonator"},
    {0x10, "Van"}, {0x11, "Clockwork Kazooie"}, {0x12, "Small T-Rex"},
    {0x13, "Large T-Rex"},
}};

std::mutex form_mutex;
Status latest;
std::uint64_t next_request_number = 0;
bool queued = false;
bool dispatch_in_flight = false;
std::chrono::steady_clock::time_point queued_at{};
std::chrono::steady_clock::time_point dispatched_at{};
std::uint16_t expected_map = 0;
std::int16_t expected_entrance = 0;
std::uint32_t controlled_index = 0;
std::uint64_t baseline_activation = 0;
bool restore_seen = false;
bool basetup_early_seen = false;
bool basetup_seen = false;

constexpr auto timeout = std::chrono::seconds(20);
constexpr auto form_owner = tooie::practice::TransitionOwner::Form;

bool known_form(std::uint8_t id) noexcept {
    for (const Form& candidate : catalog) {
        if (candidate.id == id) return true;
    }
    return false;
}

void finish_locked(Outcome outcome, Failure failure = Failure::None) noexcept {
    queued = false;
    latest.outcome = outcome;
    latest.failure = failure;
    latest.saved_record_overridden = restore_seen;
    latest.basetup_overridden = basetup_seen;
    tooie::practice::release_transition(form_owner);
}

bool gameplay_mode(std::uint8_t* rdram) noexcept {
    const auto mode = MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012762CU)));
    const auto slot = MEM_B(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F1U)));
    const auto game_type = MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F2U)));
    // Type 3 selects the separate multiplayer preload branch. The normal
    // gameplay mode/slot admission matches the existing Practice Travel gate.
    return (mode < 0x0FU || (mode < 0x1CU && mode != 0x11U)) &&
        slot >= 0 && game_type != 3;
}

template <typename Function>
std::uint32_t guest_value(Function function, std::uint8_t* rdram,
    recomp_context* ctx) {
    const recomp_context saved = *ctx;
    try {
        function(rdram, ctx);
        const auto result = static_cast<std::uint32_t>(ctx->r2);
        *ctx = saved;
        return result;
    } catch (...) {
        *ctx = saved;
        throw;
    }
}

std::uint32_t player_form(std::uint8_t* rdram, recomp_context* ctx,
    std::uint32_t index) {
    const recomp_context saved = *ctx;
    ctx->r4 = index;
    try {
        func_800F5410(rdram, ctx);
        const auto result = static_cast<std::uint32_t>(ctx->r2);
        *ctx = saved;
        return result;
    } catch (...) {
        *ctx = saved;
        throw;
    }
}

std::uint32_t player_behavior(std::uint8_t* rdram, recomp_context* ctx,
    std::uint32_t player) {
    const recomp_context saved = *ctx;
    // Guest pointers in the recompiled context are sign-extended MIPS values.
    ctx->r4 = static_cast<gpr>(static_cast<std::int32_t>(player));
    try {
        bs_getCurrentState(rdram, ctx);
        const auto result = static_cast<std::uint32_t>(ctx->r2);
        *ctx = saved;
        return result;
    } catch (...) {
        *ctx = saved;
        throw;
    }
}

std::uint32_t player_pointer(std::uint8_t* rdram, std::uint32_t index) noexcept {
    return static_cast<std::uint32_t>(MEM_W(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x80135490U + index * 4U))));
}

bool guest_range(std::uint32_t address, std::uint32_t bytes) noexcept {
    constexpr std::uint32_t first = 0x80000400U;
    constexpr std::uint32_t end = 0x80800000U;
    return address >= first && bytes <= end - first &&
        address <= end - bytes && (address & 3U) == 0;
}

bool player_behavior_ready(std::uint8_t* rdram, std::uint32_t player) noexcept {
    if (!guest_range(player, 0x124U)) return false;
    const auto state = static_cast<std::uint32_t>(MEM_W(0,
        static_cast<gpr>(static_cast<std::int32_t>(player + 0x120U))));
    // bs_getCurrentState reads the state pointer, then state->current at +4.
    return guest_range(state, 8U);
}

void request_reload(std::uint8_t* rdram, recomp_context* ctx,
    std::uint16_t map, std::int16_t entrance) {
    const recomp_context saved = *ctx;
    ctx->r4 = map;
    ctx->r5 = static_cast<std::uint16_t>(entrance);
    ctx->r6 = 1;
    try {
        func_800A7990(rdram, ctx);
    } catch (...) {
        *ctx = saved;
        throw;
    }
    *ctx = saved;
    MEM_H(0, static_cast<gpr>(static_cast<std::int32_t>(0x80132DCAU))) = 0;
    MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x801275C0U))) = 1U;
}

bool expected_activation_active(std::uint8_t* rdram) noexcept {
    const auto scene = tooie::scene::snapshot();
    return scene.activation_active &&
        scene.activations_completed == baseline_activation &&
        std::chrono::steady_clock::now() - dispatched_at <= timeout &&
        static_cast<std::uint16_t>(MEM_H(0,
            static_cast<gpr>(static_cast<std::int32_t>(0x80132DC2U)))) == expected_map &&
        static_cast<std::int16_t>(MEM_H(0,
            static_cast<gpr>(static_cast<std::int32_t>(0x80132DC8U)))) == expected_entrance;
}
} // namespace

namespace tooie::practice::forms {

std::size_t form_count() noexcept { return catalog.size(); }

const Form* form(std::size_t index) noexcept {
    return index < catalog.size() ? &catalog[index] : nullptr;
}

bool request(std::uint8_t form_id) noexcept {
    const auto scene = tooie::scene::snapshot();
    const bool cutscene = tooie::features::cutscene_active();
    std::lock_guard lock(form_mutex);
    if (queued || latest.outcome == Outcome::Queued ||
        latest.outcome == Outcome::ReloadRequested) return false;
    const Failure rejection = !known_form(form_id) ? Failure::InvalidForm :
        cutscene ? Failure::UnsafeGameplay :
        !scene.map_available ? Failure::NoActiveMap :
        scene.activation_active ? Failure::TransitionActive : Failure::None;
    if (rejection != Failure::None ||
        !tooie::practice::try_reserve_transition(form_owner)) {
        latest = {Outcome::Rejected, form_id, ++next_request_number,
            rejection != Failure::None ? rejection : Failure::Busy};
        return false;
    }
    queued = true;
    queued_at = std::chrono::steady_clock::now();
    latest = {Outcome::Queued, form_id, ++next_request_number};
    return true;
}

Status status() noexcept {
    std::lock_guard lock(form_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (queued && latest.outcome == Outcome::Queued && now - queued_at > timeout) {
        finish_locked(Outcome::TimedOut);
    } else if (latest.outcome == Outcome::ReloadRequested &&
        !dispatch_in_flight && now - dispatched_at > timeout) {
        finish_locked(Outcome::TimedOut);
    }
    return latest;
}

const char* failure_label(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "None";
    case Failure::InvalidForm: return "Not a defined transformation";
    case Failure::Busy: return "Another Practice transition is pending";
    case Failure::NoActiveMap: return "No active game area";
    case Failure::UnsafeGameplay: return "Available only during normal gameplay, outside cutscenes";
    case Failure::TransitionActive: return "A game transition is active";
    case Failure::Multiplayer: return "Multiplayer mode is not supported";
    case Failure::InvalidEntrance: return "Current entrance is not a valid transition index";
    case Failure::MissingPlayer: return "Controlled player is unavailable";
    case Failure::AlreadyInForm: return "Already in the requested form";
    case Failure::DifferentMap: return "Reload did not finish at the expected area entrance";
    case Failure::NativeSetupIncomplete: return "Native player setup did not complete";
    case Failure::FormMismatch: return "Observed player form differs from the request";
    case Failure::MissingBehavior: return "Native behavior state was not established";
    }
    return "Unknown form failure";
}

void tick(std::uint8_t* rdram, recomp_context* ctx) {
    if (!rdram || !ctx) return;
    const auto scene = tooie::scene::snapshot();
    std::uint8_t target = 0;
    std::uint64_t request_number = 0;
    {
        std::lock_guard lock(form_mutex);
        if (latest.outcome == Outcome::ReloadRequested) {
            if (!dispatch_in_flight &&
                std::chrono::steady_clock::now() - dispatched_at > timeout) {
                finish_locked(Outcome::TimedOut);
            } else if (scene.activations_completed > baseline_activation &&
                !scene.activation_active) {
                // Completion only counts if native basetup ran on the selected
                // player and the resulting player really has the requested form.
                const bool expected_arrival = scene.map_available &&
                    scene.map_id == expected_map &&
                    static_cast<std::int16_t>(MEM_H(0,
                        static_cast<gpr>(static_cast<std::int32_t>(0x80132DC8U)))) ==
                        expected_entrance;
                if (scene.activations_completed != baseline_activation + 1 ||
                    !expected_arrival) {
                    finish_locked(Outcome::Rejected, Failure::DifferentMap);
                    return;
                }
                if (!basetup_seen) {
                    finish_locked(Outcome::Rejected, Failure::NativeSetupIncomplete);
                    return;
                }
                const auto player = player_pointer(rdram, controlled_index);
                if (player == 0) {
                    finish_locked(Outcome::Rejected, Failure::MissingPlayer);
                    return;
                }
                if (!player_behavior_ready(rdram, player)) {
                    finish_locked(Outcome::Rejected, Failure::NativeSetupIncomplete);
                    return;
                }
                try {
                    latest.observed_form = static_cast<std::uint8_t>(
                        player_form(rdram, ctx, controlled_index));
                    latest.observed_behavior = player_behavior(rdram, ctx, player);
                } catch (...) {
                    finish_locked(Outcome::Rejected, Failure::NativeSetupIncomplete);
                    throw;
                }
                if (latest.observed_form != latest.form_id)
                    finish_locked(Outcome::Rejected, Failure::FormMismatch);
                else if (latest.observed_behavior == 0)
                    finish_locked(Outcome::Rejected, Failure::MissingBehavior);
                else finish_locked(Outcome::Applied);
            } else if (scene.activation_active &&
                static_cast<std::uint16_t>(MEM_H(0,
                    static_cast<gpr>(static_cast<std::int32_t>(0x80132DC2U)))) !=
                    expected_map) {
                finish_locked(Outcome::Rejected, Failure::DifferentMap);
            }
        }
        if (queued) {
            if (std::chrono::steady_clock::now() - queued_at > timeout) {
                finish_locked(Outcome::TimedOut);
            } else {
                target = latest.form_id;
                request_number = latest.request_number;
                queued = false;
            }
        }
    }
    if (target == 0) return;
    const auto game_type = MEM_BU(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F2U)));
    const auto transition = MEM_BU(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x801275C0U)));
    if (!gameplay_mode(rdram) || tooie::features::cutscene_active() ||
        !scene.map_available || scene.activation_active || transition != 0U) {
        std::lock_guard lock(form_mutex);
        if (latest.request_number == request_number &&
            latest.outcome == Outcome::Queued)
            finish_locked(Outcome::Rejected,
                game_type == 3 ? Failure::Multiplayer :
                transition != 0U || scene.activation_active ? Failure::TransitionActive :
                !scene.map_available ? Failure::NoActiveMap : Failure::UnsafeGameplay);
        return;
    }

    try {
        const auto map = static_cast<std::uint16_t>(guest_value(
            func_800EA05C, rdram, ctx));
        const auto entrance = static_cast<std::int16_t>(guest_value(
            func_800EA090, rdram, ctx));
        const auto index = guest_value(func_800F54E4, rdram, ctx);
        if (map != scene.map_id || entrance < 0 || entrance > 255 || index >= 2 ||
            player_pointer(rdram, index) == 0 ||
            player_form(rdram, ctx, index) == target) {
            std::lock_guard lock(form_mutex);
            if (latest.request_number == request_number &&
                latest.outcome == Outcome::Queued)
                finish_locked(Outcome::Rejected,
                    map != scene.map_id ? Failure::DifferentMap :
                    entrance < 0 || entrance > 255 ? Failure::InvalidEntrance :
                    index >= 2 || player_pointer(rdram, index) == 0
                        ? Failure::MissingPlayer : Failure::AlreadyInForm);
            return;
        }
        {
            std::lock_guard lock(form_mutex);
            if (latest.request_number != request_number ||
                latest.outcome != Outcome::Queued) return;
            expected_map = map;
            expected_entrance = entrance;
            controlled_index = index;
            baseline_activation = scene.activations_completed;
            restore_seen = false;
            basetup_early_seen = false;
            basetup_seen = false;
            latest.outcome = Outcome::ReloadRequested;
            dispatched_at = std::chrono::steady_clock::now();
            dispatch_in_flight = true;
        }
        request_reload(rdram, ctx, map, entrance);
        {
            std::lock_guard lock(form_mutex);
            if (latest.request_number != request_number ||
                latest.outcome != Outcome::ReloadRequested) return;
            dispatch_in_flight = false;
            // The core transition call may itself have taken time. Start the
            // arrival deadline only after it has returned to the guest hook.
            dispatched_at = std::chrono::steady_clock::now();
        }
    } catch (...) {
        std::lock_guard lock(form_mutex);
        if (latest.request_number == request_number &&
            (latest.outcome == Outcome::Queued ||
                latest.outcome == Outcome::ReloadRequested)) {
            dispatch_in_flight = false;
            finish_locked(Outcome::Rejected, Failure::NativeSetupIncomplete);
        }
        throw;
    }
}

void reset() noexcept {
    std::lock_guard lock(form_mutex);
    latest = {};
    queued = false;
    dispatch_in_flight = false;
    queued_at = {};
    dispatched_at = {};
    expected_map = 0;
    expected_entrance = 0;
    controlled_index = 0;
    baseline_activation = 0;
    restore_seen = false;
    basetup_early_seen = false;
    basetup_seen = false;
    tooie::practice::release_transition(form_owner);
}

} // namespace tooie::practice::forms

extern "C" void tooie_practice_form_restore_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    if (!rdram || !ctx) return;
    std::lock_guard lock(form_mutex);
    if (latest.outcome != Outcome::ReloadRequested ||
        !expected_activation_active(rdram) ||
        restore_seen ||
        static_cast<std::uint32_t>(ctx->r4) != controlled_index) return;
    ctx->r5 = latest.form_id;
    restore_seen = true;
}

extern "C" void tooie_practice_form_basetup_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    if (!rdram || !ctx) return;
    std::lock_guard lock(form_mutex);
    if (latest.outcome != Outcome::ReloadRequested ||
        !expected_activation_active(rdram) ||
        basetup_early_seen ||
        static_cast<std::uint32_t>(ctx->r16) !=
            player_pointer(rdram, controlled_index)) return;
    // Replace only the selector returned by the native basetup policy. The
    // following native path performs form/model reload and behavior reset.
    ctx->r2 = latest.form_id;
    basetup_early_seen = true;
}

extern "C" void tooie_practice_form_basetup_final_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    if (!rdram || !ctx) return;
    std::lock_guard lock(form_mutex);
    if (latest.outcome != Outcome::ReloadRequested ||
        !expected_activation_active(rdram) ||
        !basetup_early_seen || basetup_seen ||
        static_cast<std::uint32_t>(ctx->r16) !=
            player_pointer(rdram, controlled_index)) return;
    // First-person basetup can rewrite its local selector from 0x8012704C
    // after the early hook. Repair only that local argument at the common
    // apply site, without changing the global or saved player record.
    MEM_W(0X3C, ctx->r29) = latest.form_id;
    basetup_seen = true;
}
