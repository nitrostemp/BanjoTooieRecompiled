#pragma once
#include "recomp.h"
extern "C" void tooie_core1_osInitialize(uint8_t*, recomp_context*);
namespace tooie { uint32_t core1_fcsr_shadow(); }
