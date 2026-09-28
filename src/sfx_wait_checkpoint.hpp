#pragma once
#include <cstdint>
#include "recomp.h"
namespace tooie::sfx_checkpoint {
struct State { bool active{},first_outer{}; };
// Caller owns State per guest native worker and supplies the unchanged context
// at an existing generated poll. This function never writes guest state.
inline bool should_service(bool enabled,uint32_t pc,const recomp_context& ctx,State& state) noexcept {
    if(!enabled)return false;
    if(pc==0x800c2ab8) {state.active=true;state.first_outer=true;return false;}
    if(pc==0x800fb968) {state.active=false;return false;}
    if(pc!=0x800c2af4 || !state.active)return false;
    if(state.first_outer) {state.first_outer=false;return false;}
    // Taken original backedge: s2 finished59 slots, s3 counts1..59 unresolved,
    // and its branch-likely delay slot already restored s1 to the slot base.
    return uint32_t(ctx.r18)==60 && uint32_t(ctx.r19)>0 && uint32_t(ctx.r19)<60 &&
           uint32_t(ctx.r17)==0x80128c10 && uint32_t(ctx.r20)==3 &&
           uint32_t(ctx.r21)==60 && uint32_t(ctx.r22)==0x80128c10;
}
}
