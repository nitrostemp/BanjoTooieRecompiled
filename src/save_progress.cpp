#include "save_progress.hpp"

#include "funcs.h"
#include "recomp.h"
#include "save_persistence.hpp"

#include <atomic>
#include <chrono>

namespace {

using Clock = std::chrono::steady_clock;
using Status = tooie::save_progress::Status;

constexpr auto request_lifetime = std::chrono::seconds{2};
std::atomic<Status> current_status{Status::Idle};
std::atomic<std::int64_t> deadline_ns{0};
std::atomic_uint64_t pending_generation{0};

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

bool save_manager_allows_request(uint8_t* rdram) noexcept {
    const auto mode = MEM_BU(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012762C)));
    const bool supported_mode = mode < 0x0FU || (mode < 0x1CU && mode != 0x11U);
    const auto active_slot = MEM_B(0, static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F1)));
    return supported_mode && active_slot != -1;
}

bool is_normal_pause_page(uint8_t* rdram, std::uint32_t pause_state) noexcept {
    const auto state = static_cast<gpr>(static_cast<std::int32_t>(pause_state));
    // PauseState: PageIndex=0, ExitType=2, confirmation state=0xE. These are
    // the original main-options-page fields, not frontend-invented state.
    return MEM_BU(0, state) == 2U && MEM_BU(2, state) == 0U && MEM_BU(0xEU, state) == 0U;
}

} // namespace

namespace tooie::save_progress {

void request() noexcept {
    const auto previous = current_status.load(std::memory_order_acquire);
    if (previous == Status::SubmittingToGame || previous == Status::SubmittedToGame) return;
    deadline_ns.store(now_ns() + std::chrono::duration_cast<std::chrono::nanoseconds>(request_lifetime).count(),
        std::memory_order_release);
    current_status.store(Status::PendingPauseMenu, std::memory_order_release);
}

Status status() noexcept {
    auto current = current_status.load(std::memory_order_acquire);
    if (current == Status::SubmittedToGame) {
        const auto generation = pending_generation.load(std::memory_order_acquire);
        const auto next = save_persistence::complete(generation) ? Status::Persisted
            : save_persistence::unsuccessful(generation) ? Status::PersistenceFailed : current;
        current_status.compare_exchange_strong(current, next, std::memory_order_acq_rel);
        current = current_status.load(std::memory_order_acquire);
    }
    if (current == Status::PendingPauseMenu) {
        const auto deadline = deadline_ns.load(std::memory_order_acquire);
        if (deadline == 0 || now_ns() > deadline) {
            Status pending = Status::PendingPauseMenu;
            current_status.compare_exchange_strong(pending, Status::ExpiredOutsidePause, std::memory_order_acq_rel);
            current = current_status.load(std::memory_order_acquire);
        }
    }
    return current;
}

void reset() noexcept {
    deadline_ns.store(0, std::memory_order_release);
    current_status.store(Status::Idle, std::memory_order_release);
    pending_generation.store(0, std::memory_order_release);
}

} // namespace tooie::save_progress

extern "C" void tooie_save_progress_pause_tick(uint8_t* rdram, recomp_context* ctx,
    std::uint32_t pause_state) {
    if (current_status.load(std::memory_order_acquire) != Status::PendingPauseMenu) return;
    const auto deadline = deadline_ns.load(std::memory_order_acquire);
    if (deadline == 0 || now_ns() > deadline) {
        current_status.store(Status::ExpiredOutsidePause, std::memory_order_release);
        return;
    }
    if (!is_normal_pause_page(rdram, pause_state)) return;
    if (!save_manager_allows_request(rdram)) {
        current_status.store(Status::RejectedUnavailable, std::memory_order_release);
        return;
    }

    // Reproduce the original confirmed Save Progress sequence from
    // gcnewpause_entrypoint_2: func_800FC6B0(0xE), then func_800D389C().
    // This hook is not an original MIPS callsite, so restore its caller's
    // complete guest CPU context afterwards. The two calls retain their
    // intended global and EEPROM side effects; only temporary registers and
    // FPU state are discarded.
    Status expected = Status::PendingPauseMenu;
    if (!current_status.compare_exchange_strong(expected, Status::SubmittingToGame, std::memory_order_acq_rel)) return;
    const recomp_context caller_context = *ctx;
    const auto before_write = tooie::save_persistence::generation();
    try {
        ctx->r4 = ADD32(0, 0xEU);
        func_800FC6B0(rdram, ctx);
        func_800D389C(rdram, ctx);
    } catch (...) {
        *ctx = caller_context;
        throw;
    }
    *ctx = caller_context;
    const auto after_write = tooie::save_persistence::generation();
    pending_generation.store(after_write, std::memory_order_release);
    // Do not erase a new F5 request that arrived on the SDL owner while this
    // synchronous guest call was running.
    expected = Status::SubmittingToGame;
    current_status.compare_exchange_strong(expected,
        after_write > before_write ? Status::SubmittedToGame : Status::RejectedUnavailable,
        std::memory_order_acq_rel);
}
