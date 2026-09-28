#pragma once
#include "recomp.h"
#include "context_observer.hpp"
#include <exception>

namespace tooie {
// The caller has already started the cleaner; install before any worker creation.
void prepare_main_thread_gate(uint8_t* rdram);
context_observer::Token main_gate_context_token(int id);
void main_thread_create_callback(uint8_t* rdram, recomp_context* ctx);
// Called on the actual idle worker with no caller gate lock held.
void run_original_idle(uint8_t* rdram, recomp_context* ctx, std::exception_ptr initial_error = {});
void finish_main_thread_gate(uint8_t* rdram);
void cleanup_main_thread_gate(uint8_t* rdram);
bool main_gate_memory_safe();
}
extern "C" bool tooie_main_hooks_active();
extern "C" void tooie_main_hook(uint8_t*, recomp_context*, unsigned);
