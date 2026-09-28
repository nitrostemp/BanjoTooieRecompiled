#pragma once
#include "recomp.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void tooie_boot_hook(uint8_t* rdram, recomp_context* ctx, unsigned stage);
bool tooie_thread_hooks_active(void);
void tooie_thread_hook(uint8_t* rdram, recomp_context* ctx, unsigned stage);
bool tooie_main_hooks_active(void);
void tooie_main_hook(uint8_t* rdram, recomp_context* ctx, unsigned stage);
void tooie_continuous_poll(uint8_t* rdram, recomp_context* ctx, uint32_t pc);
void tooie_observe_global_settings(const recomp_context* ctx, uint32_t site);
void tooie_widescreen_apply_profile(recomp_context* ctx);
void tooie_observe_graphics(const uint8_t* rdram, const recomp_context* ctx, uint32_t site);
void tooie_observe_title(const uint8_t* rdram, const recomp_context* ctx, uint32_t site);
void tooie_observe_map_actor_list(const uint8_t* rdram, const recomp_context* ctx, uint32_t site);
void tooie_observe_menu(const uint8_t* rdram, const recomp_context* ctx, uint32_t site);
void tooie_core2_hook(uint8_t* rdram, recomp_context* ctx, unsigned stage);
int32_t tooie_uncached_word(uint8_t* rdram, uint32_t address);
void tooie_rom_read_word(uint8_t* rdram, recomp_context* ctx);
int32_t tooie_overlay_hardware_word(uint32_t address);
void tooie_overlay_after_relocate(uint8_t*,uint32_t header,int32_t delta);
void tooie_overlay_before_free(uint8_t*,uint32_t header);
void tooie_validate_overlay(uint8_t*,uint32_t header,int32_t delta);
void tooie_overlay_callsite_push(uint32_t original_jal_pc);
void tooie_overlay_callsite_pop(void);
#ifdef __cplusplus
}
namespace tooie {
void prepare_core1_diagnostic(uint8_t* rdram, bool idle=false, bool main=false);
void prepare_core1_continuous(uint8_t* rdram);
void restore_core1_boundary();
bool core1_boundary_reached();
void boot_procedure_entry(uint8_t* rdram, recomp_context* ctx);
}
#endif
