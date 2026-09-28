#pragma once

#include "recomp.h"

#include <cstdint>

namespace tooie::save_progress {

enum class Status : std::uint8_t {
    Idle,
    PendingPauseMenu,
    SubmittingToGame,
    SubmittedToGame,
    ExpiredOutsidePause,
    RejectedUnavailable,
    Persisted,
    PersistenceFailed,
};

// Creates a short-lived request. The generated pause-menu hook is the only
// consumer: a request never calls an EEPROM primitive or executes from the
// frontend/event thread.
void request() noexcept;
Status status() noexcept;
void reset() noexcept;

} // namespace tooie::save_progress

// Called only by the generated original pause-menu update function.
extern "C" void tooie_save_progress_pause_tick(uint8_t* rdram, recomp_context* ctx,
    std::uint32_t pause_state);
