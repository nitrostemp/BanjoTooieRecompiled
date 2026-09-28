#pragma once
#include "persistent_state_continuation.hpp"
namespace tooie::persistent_state::hle {
std::span<const continuation::Function> functions();
}
extern "C" {
void tooie_persistent_recv(uint8_t*,recomp_context*);
void tooie_persistent_send(uint8_t*,recomp_context*);
void tooie_persistent_jam(uint8_t*,recomp_context*);
void tooie_persistent_start_thread(uint8_t*,recomp_context*);
void tooie_persistent_stop_thread(uint8_t*,recomp_context*);
void tooie_persistent_set_priority(uint8_t*,recomp_context*);
}
