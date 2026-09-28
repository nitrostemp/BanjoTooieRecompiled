#include "game.hpp"
#include "continuous_host.hpp"
#include "si_adapter.hpp"
#include "hardware_access.hpp"
#include "tooie_overlays.hpp"
#include "overlay_live_fixture.hpp"
#include "vi_timing.hpp"
#include <cstdlib>
#include <iostream>
namespace tooie {
[[noreturn]] void unsupported(const char* symbol,recomp_context* ctx,uint32_t pc) {
    trace("unsupported_binding","none","failure",pc,ctx,{{"symbol",symbol}});
    if (continuous_enabled()) throw std::runtime_error(std::string("Unsupported reached binding: ")+symbol);
    fault_packet();std::cerr<<"S1-A unsupported binding: "<<symbol<<std::endl;std::_Exit(3);
}
}
// Explicit failing imports for a bounded diagnostic, NOT implementations or successful stubs.
// Boot-only bridges live in boot_bridge.cpp; remaining services are outside this gate.
#define FAIL_BINDING(name) extern "C" void name(uint8_t*,recomp_context*ctx){tooie::unsupported(#name,ctx);}
extern "C" void __osSiGetAccess_recomp(uint8_t*r,recomp_context*c){if(!tooie::continuous_enabled())tooie::unsupported("__osSiGetAccess_recomp",c);tooie_si_get_access(r,c);}
extern "C" void __osSiRawStartDma_recomp(uint8_t*r,recomp_context*c){if(!tooie::continuous_enabled())tooie::unsupported("__osSiRawStartDma_recomp",c);tooie_si_raw_start_dma(r,c);}
extern "C" void __osSiRelAccess_recomp(uint8_t*r,recomp_context*c){if(!tooie::continuous_enabled())tooie::unsupported("__osSiRelAccess_recomp",c);tooie_si_rel_access(r,c);}
extern "C" void osPfsInit_recomp(uint8_t*r,recomp_context*c){if(!tooie::continuous_enabled())tooie::unsupported("osPfsInit_recomp",c);tooie_pfs_init_absent(r,c);}
extern "C" void osViGetCurrentLine_recomp(uint8_t*,recomp_context*c){
    if(!tooie::continuous_enabled())tooie::unsupported("osViGetCurrentLine_recomp",c);
    c->r2=static_cast<gpr>(tooie::vi::current_line());
}
extern "C" void recomp_syscall_handler(uint8_t*rdram,recomp_context*ctx,int32_t instruction_vram) {
    if(!tooie::continuous_enabled())tooie::unsupported("recomp_syscall_handler",ctx,(uint32_t)instruction_vram);
    if(tooie::maybe_run_live_overlay_fixture(rdram,ctx,(uint32_t)instruction_vram))return;
    tooie::overlays::dispatch_syscall(rdram,ctx,(uint32_t)instruction_vram);
}
