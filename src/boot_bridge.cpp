#include "boot_bridge.hpp"
#include "game.hpp"
#include "librecomp/addresses.hpp"
#include <cfenv>
#include <stdexcept>

namespace {
uint32_t fcsr;
bool pi_busy;
gpr guest(uint32_t a) { return (gpr)(int32_t)a; }
}
namespace tooie { uint32_t boot_fcsr_shadow() { return fcsr; } }
extern "C" void boot_osInitialize(uint8_t* rdram, recomp_context* ctx) {
    // Observable effects of the actual linked FINALROM/J routine. See BOOT_CONTRACT.md.
    MEM_W(0,guest(0x80006F50))=1;
    ctx->status_reg |= 0x20000000u;
    fcsr=0x01000800;
    if(std::fesetround(FE_TONEAREST))throw std::runtime_error("Boot rounding initialization failed");
    for(uint32_t vector:{0x80000000u,0x80000080u,0x80000100u,0x80000180u})
        for(unsigned i=0;i<16;i+=4)MEM_W(i,guest(vector))=MEM_W(i,guest(0x80001C90));
    uint64_t clock=((uint64_t)(uint32_t)MEM_W(0,guest(0x80004250))<<32)|(uint32_t)MEM_W(4,guest(0x80004250));
    clock=clock*3u/4u;
    MEM_W(0,guest(0x80004250))=clock>>32;MEM_W(4,guest(0x80004250))=clock;
    if(MEM_W(0,guest(0x8000030C))==0)
        for(unsigned i=0;i<64;i++)MEM_B(i,guest(0x8000031C))=0;
    uint32_t tv=MEM_W(0,guest(0x80000300));
    uint32_t vi=tv==0?0x02F5B2D2u:tv==2?0x02E6025Cu:0x02E6D354u;
    MEM_W(0,guest(0x80004258))=vi;
    MEM_B(0,guest(0x80006F64))=7;MEM_B(0,guest(0x80006FDC))=7;
    tooie::capture_guest_state(rdram,ctx);
    tooie::trace("boot_initialize","E","pass",0x800016F0,ctx,
        {{"clock_rate",clock},{"vi_clock",tooie::hex32(vi)},{"status",tooie::hex32(ctx->status_reg)},
         {"fcsr_shadow",tooie::hex32(fcsr)},{"hardware_effects_deferred",true},{"game_threads_started",false}});
}
extern "C" void boot___osDisableInt(uint8_t*,recomp_context* ctx) {
    ctx->r2=ctx->status_reg&1u;ctx->status_reg&=~1u;
}
extern "C" void boot___osRestoreInt(uint8_t*,recomp_context* ctx) {
    ctx->status_reg|=(uint32_t)ctx->r4;
}
extern "C" void boot___osPiRawStartDma(uint8_t* rdram,recomp_context* ctx) {
    uint32_t direction=ctx->r4,offset=ctx->r5,destination=ctx->r6,length=ctx->r7;
    uint32_t segment=destination&0xE0000000u,physical_ram=destination&0x1FFFFFFFu;
    auto rom=recomp::get_rom();
    tooie::trace("boot_dma_request","G","entered",0x80001A40,ctx,
        {{"rom_offset",tooie::hex32(offset)},{"physical_address",tooie::hex32(recomp::rom_base+offset)},
         {"guest_destination",tooie::hex32(destination)},{"bytes",length},{"direction",direction},
         {"mode","synchronous pre-thread diagnostic"}});
    const char* error=nullptr;
    if(direction!=0)error="Only boot OS_READ is supported";
    else if(!length || (length&1) || length>0x1000000)error="Invalid boot DMA length";
    else if(offset&1)error="Unaligned boot ROM offset";
    else if(segment!=0x80000000 && segment!=0xA0000000)error="DMA destination requires KSEG0/KSEG1";
    else if(destination&7)error="Unaligned boot RDRAM destination";
    else if((uint64_t)offset+length>rom.size())error="Boot DMA exceeds original ROM";
    else if((uint64_t)physical_ram+length>0x800000)error="Boot DMA exceeds guest 8 MiB RDRAM";
    else if(pi_busy)error="Reentrant boot DMA is unsupported";
    if(error) {
        tooie::trace("boot_dma_rejected","G","rejected",0x80001A40,ctx,{{"reason",error},{"memory_untouched",true}});
        throw std::runtime_error(error);
    }
    pi_busy=true;
    struct Complete {~Complete(){pi_busy=false;}} complete;
    gpr canonical=guest(0x80000000u|physical_ram);
    recomp::do_rom_read(rdram,canonical,recomp::rom_base+offset,length);
    for(uint32_t i=0;i<length;i++)if((uint8_t)MEM_B(i,canonical)!=rom[offset+i])
        throw std::runtime_error("Runtime boot DMA copy mismatch at "+tooie::hex32(i));
    pi_busy=false;ctx->r2=0;
    tooie::capture_guest_state(rdram,ctx);
    tooie::trace("boot_dma_complete","G","pass",0x80001A40,ctx,
        {{"rom_offset",tooie::hex32(offset)},{"physical_address",tooie::hex32(recomp::rom_base+offset)},
         {"guest_destination",tooie::hex32(destination)},{"physical_ram",tooie::hex32(physical_ram)},
         {"bytes",length},{"direction",direction},{"copy_validated",true},{"status",0},
         {"runtime_copy","recomp::do_rom_read"},{"mode","synchronous pre-thread diagnostic"}});
}
extern "C" void boot_osPiGetStatus(uint8_t*,recomp_context* ctx) { ctx->r2=pi_busy?1:0; }
