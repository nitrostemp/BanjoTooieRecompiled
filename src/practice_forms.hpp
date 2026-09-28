#pragma once

#include <cstddef>
#include <cstdint>
#include "recomp.h"

namespace tooie::practice::forms {

struct Form {
    std::uint8_t id;
    const char* name;
};

enum class Outcome : std::uint8_t {
    None, Queued, ReloadRequested, Applied, Rejected, TimedOut,
};

enum class Failure : std::uint8_t {
    None, InvalidForm, Busy, NoActiveMap, UnsafeGameplay, TransitionActive, Multiplayer,
    InvalidEntrance, MissingPlayer, AlreadyInForm, DifferentMap,
    NativeSetupIncomplete, FormMismatch, MissingBehavior,
};

struct Status {
    Outcome outcome = Outcome::None;
    std::uint8_t form_id = 0;
    std::uint64_t request_number = 0;
    Failure failure = Failure::None;
    std::uint8_t observed_form = 0;
    std::uint32_t observed_behavior = 0;
    bool saved_record_overridden = false;
    bool basetup_overridden = false;
};

std::size_t form_count() noexcept;
const Form* form(std::size_t index) noexcept;
bool request(std::uint8_t form_id) noexcept;
Status status() noexcept;
const char* failure_label(Failure failure) noexcept;
void tick(std::uint8_t* rdram, recomp_context* ctx);
void reset() noexcept;

} // namespace tooie::practice::forms

// Request-scoped hooks in generated player restoration and basetup. The
// generator owns their call sites; neither hook changes saved records.
extern "C" void tooie_practice_form_restore_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept;
extern "C" void tooie_practice_form_basetup_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept;
extern "C" void tooie_practice_form_basetup_final_override(std::uint8_t* rdram,
    recomp_context* ctx) noexcept;
