#include "ipl3_boot_state.hpp"
#include "ipl3_shape.hpp"
#include <array>
#include <stdexcept>
#include <utility>

namespace tooie::boot {
ScatterReport apply_ipl3_scatter(std::span<const uint8_t> rom, std::span<uint8_t> rdram) {
    if (rom.size()<0x1000) throw std::runtime_error("IPL3: truncated original boot image");
    const auto word=[&](size_t offset) {
        return uint32_t(rom[offset])<<24 | uint32_t(rom[offset+1])<<16 |
               uint32_t(rom[offset+2])<<8 | uint32_t(rom[offset+3]);
    };
    // Narrow equivalent of the original pre-entry DMA chain, not an IPL3
    // interpreter. Pin its producer instructions; the application separately
    // verifies the entire original ROM identity before this function is called.
    // Instruction words are generated locally from the validated user ROM.
    for(auto [offset,instruction]:detail::ipl3_shape)
        if(word(offset)!=instruction) throw std::runtime_error("IPL3: changed scatter producer instructions");

    // CPU copies ROM554..887 to physical4..337. The original RSP bootstrap
    // reads physical1E8..3D7 into IMEM1120..130F. The scatter consumes only
    // its first192 bytes, which correspond to ROM738..7F7. See mission's
    // integrity-ipl3 producer proof; no expected integrity word is supplied.
    const uint32_t cpu_source=word(0x4FC)&0xFFFF;
    const uint32_t cpu_destination=word(0x520)&0xFFFF;
    // IPL3 XORs its bootstrap data against the resident IPL2 word5500FFFC;
    // the resulting ADDI s1,zero,1E8 supplies both SP_DRAM_ADDR and SP_RD_LEN.
    const uint32_t rsp_read_source=(word(0x094)^0x5500FFFC)&0xFFFF;
    const uint32_t source=cpu_source+rsp_read_source-cpu_destination;
    const uint32_t destination=(((word(0x850)&0xFFFF)<<16) ^ (word(0x854)&0xFFFF)) & 0xFFFFFF;
    const uint32_t dma=((word(0x85C)&0xFFFF)<<16) ^ (word(0x860)&0xFFFF);
    const uint32_t bytes=((dma&0xFFF)|7)+1;
    const uint32_t blocks=((dma>>12)&0xFF)+1;
    const uint32_t stride=bytes+(dma>>20);
    const uint64_t source_end=uint64_t(source)+uint64_t(blocks)*bytes;
    const uint64_t destination_end=uint64_t(destination)+uint64_t(blocks-1)*stride+bytes;
    if(source_end>rom.size() || destination_end>rdram.size() || (rdram.size()&3))
        throw std::runtime_error("IPL3: scatter exceeds ROM or RDRAM bounds");
    for(uint32_t block=0;block<blocks;++block)
        for(uint32_t i=0;i<bytes;++i)
            rdram[(destination+block*stride+i)^3]=rom[source+block*bytes+i];
    return {source,destination,blocks,bytes,stride};
}
}
