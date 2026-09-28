#pragma once
#include "recomp.h"
extern "C" {
void boot_osInitialize(uint8_t*, recomp_context*);
void boot___osDisableInt(uint8_t*, recomp_context*);
void boot___osRestoreInt(uint8_t*, recomp_context*);
void boot___osPiRawStartDma(uint8_t*, recomp_context*);
void boot_osPiGetStatus(uint8_t*, recomp_context*);
}
namespace tooie { uint32_t boot_fcsr_shadow(); }
