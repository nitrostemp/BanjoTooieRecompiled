#pragma once
#include "recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
// Synchronous, const-context observations; no scheduling or guest writes.
void tooie_observe_audio_pacing(const uint8_t* rdram, const recomp_context* ctx, uint32_t site)
#ifdef __cplusplus
    noexcept
#endif
;
#ifdef __cplusplus
}
#endif
