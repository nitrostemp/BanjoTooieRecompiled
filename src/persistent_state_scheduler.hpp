#pragma once
#include "persistent_state_snapshot.hpp"
#include <chrono>
#include <exception>
#include <string>
struct UltraThreadContext;
namespace tooie::persistent_state::scheduler {
struct RestoreUnwind final : std::exception {};
// Called by the prepared runtime only. No guest queue mutation occurs here.
void worker_started(uint8_t*,uint32_t,uint64_t,uint64_t,uint64_t,UltraThreadContext*);
void worker_attached(continuation::Machine*,recomp_context*);
void worker_context_leaving() noexcept;
void worker_finished() noexcept;
void worker_retirement_complete() noexcept;
void before_scheduler_wait();
void before_scheduler_signal(UltraThreadContext*);
void after_scheduler_wait();
void before_external_wait();
void after_external_wait();
// Host-only lease: the idle/external worker parks while all other guest workers
// remain in their ordinary scheduler waits. Device freeze follows this lease.
bool freeze(std::chrono::milliseconds timeout,std::string& reason);
void release() noexcept;
void terminal_release() noexcept;
std::string refusal_reason();
bool capture_guest(void* output,uint64_t epoch) noexcept;
// All loaded structure is checked before any worker is asked to unwind.
bool validate_restore(const Snapshot&,std::string& reason);
bool restore_guest(void* input,uint64_t epoch) noexcept;
// Worker outer execution catches RestoreUnwind, reports the old native stack
// unwound, and waits for the atomic guest+device transaction to commit.
ThreadImage await_restored_image();
bool external_queue_ready() noexcept;
}
extern "C" bool tooie_persistent_external_export(std::vector<tooie::persistent_state::ExternalMessage>*) noexcept;
extern "C" bool tooie_persistent_external_import(const std::vector<tooie::persistent_state::ExternalMessage>*) noexcept;
