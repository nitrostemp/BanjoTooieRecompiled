#pragma once
#include "recomp.h"

namespace tooie {
// Call after original core1 has been loaded and registered, before executing it.
void prepare_thread_gate(uint8_t* rdram, bool main=false);
void thread_create_callback(uint8_t* rdram, recomp_context* context);
// Both return only after owned workers have exited and runtime cleanup has joined
// and deleted their contexts. Cleanup preserves a caller's original exception.
void finish_thread_gate(uint8_t* rdram);
void cleanup_thread_gate(uint8_t* rdram);
bool thread_gate_reached();
bool thread_gate_enabled();
bool thread_gate_memory_safe();
}
extern "C" bool tooie_thread_hooks_active();
extern "C" void tooie_thread_hook(uint8_t*, recomp_context*, unsigned);
