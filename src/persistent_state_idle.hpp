#pragma once
#include "persistent_state_continuation.hpp"
namespace tooie::persistent_state::idle {
std::span<const continuation::Function> functions();
}
extern "C" void tooie_persistent_pause(uint8_t*,recomp_context*);
