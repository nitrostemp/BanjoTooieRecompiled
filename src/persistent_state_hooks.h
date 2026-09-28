#pragma once
/* C ABI for mechanically lifted generated translation units. This is an
 * experimental execution contract, not the player runtime's save-state API. */
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
uint32_t tooie_persist_enter(uint32_t function,uint64_t* locals,uint32_t count);
void tooie_persist_store(uint32_t pc,const uint64_t* locals,uint32_t count);
void tooie_persist_leave(uint32_t function);
int tooie_persist_checkpoint(uint32_t pc);
int tooie_persist_yielded(void);
/* Refuses native dependencies without an explicit atomic/continuation contract. */
void tooie_persist_native_guard(const char* symbol);
void tooie_persist_invalidate(void);
void tooie_persistent_pause(uint8_t*,recomp_context*);
recomp_func_t* tooie_persist_resolve(recomp_func_t* function);
#ifdef __cplusplus
}
#endif
