#pragma once
#include "recomp.h"
#include <cstdint>
namespace tooie {
uint32_t canonical_rdram(uint32_t address,uint32_t bytes,uint32_t alignment);
}
extern "C" int32_t tooie_uncached_word(uint8_t*,uint32_t address);
extern "C" void tooie_rom_read_word(uint8_t*,recomp_context*);
extern "C" void tooie_pfs_init_absent(uint8_t*,recomp_context*);
extern "C" int32_t tooie_overlay_hardware_word(uint32_t address);
