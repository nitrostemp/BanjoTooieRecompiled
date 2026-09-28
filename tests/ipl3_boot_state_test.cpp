#include "ipl3_boot_state.hpp"
#include "ipl3_shape.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        require(argc==2,"Original ROM path required");
        std::ifstream input(argv[1],std::ios::binary);
        require(bool(input),"ROM unavailable");
        std::vector<uint8_t> rom(std::istreambuf_iterator<char>(input),{});
        require(rom.size()==0x2000000,"Wrong ROM size");
        std::vector<uint8_t> memory(0x800000,0xA6), expected=memory;
        // Independent byte-level replay of the documented CPU->RSP->RDRAM
        // transfer chain. Preserve all unaddressed bytes, not just fingerprints.
        std::vector<uint8_t> boot_ram(0x400,0), imem(0x1000,0);
        for (size_t source=0x554;source<0x888;++source)
            boot_ram[source-0x550]=rom[source];
        for (size_t i=0;i<0x1F0;++i) imem[0x120+i]=boot_ram[0x1E8+i];
        for (size_t block=0;block<24;++block)
            for (size_t byte=0;byte<8;++byte)
                expected[(0x2FB1F0+block*0xFF0+byte)^3]=imem[0x120+block*8+byte];
        const auto report=tooie::boot::apply_ipl3_scatter(rom,memory);
        require(memory==expected,"Complete IPL3 scatter state differs from original transfer chain");
        require(report.rom_source==0x738 && report.rdram_destination==0x2FB1F0 &&
                report.blocks==24 && report.block_bytes==8 && report.rdram_stride==0xFF0,
                "Scatter report descriptor mismatch");
        auto word=[&](size_t address) {
            uint32_t value=0;for(unsigned i=0;i<4;++i)value=(value<<8)|memory[(address+i)^3];return value;
        };
        require(word(0x2FB1F4)==tooie::boot::detail::ipl3_fingerprints[0] &&
                word(0x2FE1C0)==tooie::boot::detail::ipl3_fingerprints[1],"Original fingerprint result mismatch");
        auto changed=rom;changed[0x780]^=0x55;
        auto mutated=std::vector<uint8_t>(0x800000,0xA6);
        tooie::boot::apply_ipl3_scatter(changed,mutated);
        size_t changes=0;for(size_t i=0;i<memory.size();++i)changes+=memory[i]!=mutated[i];
        require(changes==1,"Scatter did not derive payload from supplied ROM");
        const auto before=memory;
        size_t rejected=0;
        for(size_t offset:{size_t(0x4FC),size_t(0x500),size_t(0x538),size_t(0x848),size_t(0x860),size_t(0x864)}) {
            auto bad=rom;bad[offset+3]^=1;
            try {tooie::boot::apply_ipl3_scatter(bad,memory);}catch(const std::exception&){++rejected;}
            require(memory==before,"Rejected instruction mutation modified RDRAM");
        }
        require(rejected==6,"Changed producer instructions were accepted");
        for(auto sizes:{std::pair<size_t,size_t>{0x800,0x800000},{rom.size(),0x300000}}) {
            bool failed=false;
            try {tooie::boot::apply_ipl3_scatter(std::span(rom).first(sizes.first),std::span(memory).first(sizes.second));}
            catch(const std::exception&){failed=true;}
            require(failed && memory==before,"Short input was accepted or partially written");
        }
        std::cout<<"PASS complete192-byte IPL3 scatter, whole8MiB guard, live-ROM mutation, six instruction rejections, two bounds rejections\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
