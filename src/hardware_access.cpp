#include "hardware_access.hpp"
#include "game.hpp"
#include "librecomp/addresses.hpp"
#include <stdexcept>
namespace tooie {
uint32_t canonical_rdram(uint32_t address,uint32_t bytes,uint32_t alignment) {
    uint32_t segment=address&0xE0000000u,physical=address&0x1FFFFFFFu;
    if((segment!=0x80000000u&&segment!=0xA0000000u)||!alignment||(alignment&(alignment-1))||
       (address&(alignment-1))||uint64_t(physical)+bytes>0x800000ull)
        throw std::runtime_error("Access is not aligned guest RDRAM: "+hex32(address));
    return 0x80000000u|physical;
}
}
extern "C" int32_t tooie_uncached_word(uint8_t*rdram,uint32_t address) {
    return MEM_W(0,(gpr)(int32_t)tooie::canonical_rdram(address,4,4));
}
extern "C" void tooie_rom_read_word(uint8_t*rdram,recomp_context*ctx) {
    // Original 0x8001E210 reads one cart word under the PI access lock. Native
    // ROM is immutable and transfers synchronous; no busy hardware register or
    // guest interrupt-mask manipulation is needed around this non-yielding read.
    // 0x8001E220/224 use LUI B000 then OR with a0 (not addition).
    // Callers pass both offsets and already mapped cartridge addresses.
    // Reproduce that operation before validating the cartridge ROM interval;
    // this is not a general physical-address/RDRAM mask.
    const uint32_t argument=uint32_t(ctx->r4);
    const uint32_t cart_address=0xB0000000u|argument;
    const uint32_t offset=cart_address-0xB0000000u;
    if((cart_address&3)||uint64_t(offset)+4>recomp::get_rom().size())
        throw std::runtime_error("Original-ROM PIO address out of bounds/alignment: "+tooie::hex32(argument));
    uint32_t destination=tooie::canonical_rdram(ctx->r5,4,4);
    recomp::do_rom_pio(rdram,(gpr)(int32_t)destination,recomp::rom_base+offset);
    ctx->r2=0;
    tooie::trace("rom_pio_read","G2","pass",0x8001E210,ctx,
        {{"argument",tooie::hex32(argument)},{"cart_address",tooie::hex32(cart_address)},
         {"original_rom_offset",tooie::hex32(offset)},{"destination",tooie::hex32(destination)},
         {"value",tooie::hex32(MEM_W(0,(gpr)(int32_t)destination))},{"bytes",4}});
}
extern "C" void tooie_pfs_init_absent(uint8_t*,recomp_context*ctx) {
    // Both mission callback sets explicitly advertise Pak::None on all ports.
    // Original osPfsInit returns __osPfsGetStatus failure before touching OSPfs;
    // CONT_CARD_ON==0 or absent controller yields PFS_ERR_NOPACK (1).
    if(uint32_t(ctx->r6)>=4)throw std::runtime_error("Controller-pak port out of range");
    ctx->r2=1;
    tooie::trace("controller_pak_absent","G6","observed",0,ctx,
        {{"port",uint32_t(ctx->r6)},{"result","PFS_ERR_NOPACK"},{"ospfs_untouched",true}});
}
extern "C" int32_t tooie_overlay_hardware_word(uint32_t address) {
    // Called only from the original overlay relocation function's three exact
    // PI/cart accesses. Native ROM DMA/PIO is synchronous, so PI is idle here.
    if(address==0xA4600010u)return 0;
    if(address<0xB0000044u || address>0xB0000040u+884u*4u || (address&3))
        throw std::runtime_error("Unsupported overlay hardware address: "+tooie::hex32(address));
    auto rom=recomp::get_rom();
    uint32_t offset=address-0xB0000000u;
    if(uint64_t(offset)+4>rom.size())throw std::runtime_error("Original ROM unavailable for overlay key");
    uint32_t value=(uint32_t(rom[offset])<<24)|(uint32_t(rom[offset+1])<<16)|(uint32_t(rom[offset+2])<<8)|rom[offset+3];
    tooie::trace("overlay_original_rom_key","G4","observed",0x80082148,nullptr,
        {{"overlay_id",(offset-0x40)/4},{"offset",tooie::hex32(offset)},{"word",tooie::hex32(value)},
         {"key_low16",value&0xffff},{"source","original compressed ROM"}});
    return (int32_t)value;
}
