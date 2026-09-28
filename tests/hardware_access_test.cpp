#include "hardware_access.hpp"
#include "game.hpp"
#include <stdexcept>
#include <iostream>
#include <vector>
extern "C" void func_80800000_sujiggy(uint8_t*,recomp_context*);
void require(bool x,const char* text){if(!x)throw std::runtime_error(text);}
int main(int argc,char**argv){
    std::vector<uint8_t> memory(8*1024*1024);auto*rdram=memory.data();
    require(tooie::canonical_rdram(0xA02FB1F4,4,4)==0x802FB1F4,"KSEG1 alias mismatch");
    MEM_W(0,(gpr)(int32_t)0x802FB1F4)=int32_t(0x98765432);
    require(uint32_t(tooie_uncached_word(rdram,0xA02FB1F4))==0x98765432,"Uncached alias failed to read actual RDRAM");
    for(uint32_t address:{0xA4600010u,0xB0000040u,0xA0800000u,0xA02FB1F5u,0x00000000u,0xFFFFFFFCu}){
        bool rejected=false;try{tooie::canonical_rdram(address,4,4);}catch(const std::exception&){rejected=true;}
        require(rejected,"MMIO/unaligned/out-of-range address accepted as RDRAM");
    }
    require(argc==2,"ROM argument required");tooie::validate_and_install_rom(argv[1]);
    auto rom=recomp::get_rom();recomp_context ctx{};
    for(uint32_t alias:{0u,0xB0000000u,0x10000000u,0x20000000u,0x30000000u,0x80000000u,0x90000000u,0xA0000000u})
    for(uint32_t offset:{0u,0x40u,0xCB8u,0x511Cu,0x1fffffcu}){
        ctx.r4=alias|offset;ctx.r5=(gpr)(int32_t)0x80001000;ctx.r16=0x1234;ctx.r29=(gpr)(int32_t)0x80002000;ctx.r31=0x5678;ctx.status_reg=0xff01;
        tooie_rom_read_word(rdram,&ctx);
        uint32_t wanted=(uint32_t(rom[offset])<<24)|(uint32_t(rom[offset+1])<<16)|(uint32_t(rom[offset+2])<<8)|rom[offset+3];
        require(uint32_t(MEM_W(0,(gpr)(int32_t)0x80001000))==wanted,"PIO differs from original ROM");
        require(ctx.r2==0&&ctx.r16==0x1234&&ctx.r29==(gpr)(int32_t)0x80002000&&ctx.r31==0x5678&&ctx.status_reg==0xff01,"PIO changed callee state or interrupt status");
    }
    ctx.r4=rom.size();ctx.r5=(gpr)(int32_t)0x80001000;
    bool rejected=false;try{tooie_rom_read_word(rdram,&ctx);}catch(const std::exception&){rejected=true;}
    require(rejected,"Out-of-range ROM PIO accepted");
    // Execute the original generated cartridge-address caller, including both
    // halfword branches and the optional second read. Expected values remain
    // host comparisons; no expected bytes are installed into guest memory.
    for(uint32_t token:{0x00000CB8u,0x00000CBAu,0x0CB80CBAu,0x0CBA0CB8u}){
        ctx={};ctx.r4=token;ctx.r16=0x123456;ctx.r29=(gpr)(int32_t)0x80004000;ctx.r31=0x1234;
        auto half=[&](uint32_t address){uint32_t off=address&~3u;
            uint32_t word=(uint32_t(rom[off])<<24)|(uint32_t(rom[off+1])<<16)|(uint32_t(rom[off+2])<<8)|rom[off+3];
            return (address&2)?word>>16:word&0xffffu;};
        uint32_t expected=half(token&0xffffu)+0xa0;
        if(token>>16)expected+=half(token>>16);
        func_80800000_sujiggy(rdram,&ctx);
        require(uint32_t(ctx.r2)==expected,"Original cartridge caller result differs");
        require(ctx.r16==0x123456&&ctx.r29==(gpr)(int32_t)0x80004000&&ctx.r31==0x1234,"Original cartridge caller callee state changed");
    }
    for(uint32_t address:{0xB2000000u,0x02000000u,0xB0000CB9u,0xA4600010u,0xBFC00000u,0x40000000u,0xC0000000u,0xFFFFFFFFu}){
        ctx.r4=address;ctx.r5=(gpr)(int32_t)0x80001000;auto prior=memory;
        bool bad=false;try{tooie_rom_read_word(rdram,&ctx);}catch(const std::exception&){bad=true;}
        require(bad,"Invalid cartridge address accepted");require(memory==prior,"Rejected cartridge read changed RDRAM");
    }
    auto before=memory;
    for(unsigned port=0;port<4;++port){ctx.r6=port;ctx.r5=(gpr)(int32_t)0x80001000;tooie_pfs_init_absent(rdram,&ctx);require(ctx.r2==1,"Absent pak reported success");}
    require(memory==before,"Absent pak path modified guest OSPfs or memory");
    require(tooie_overlay_hardware_word(0xA4600010u)==0,"Native synchronous PI should be idle");
    for(uint32_t id:{1u,350u,679u,884u}) {
        uint32_t offset=0x40+id*4;
        uint32_t value=(uint32_t(rom[offset])<<24)|(uint32_t(rom[offset+1])<<16)|(uint32_t(rom[offset+2])<<8)|rom[offset+3];
        require(uint32_t(tooie_overlay_hardware_word(0xB0000000u+offset))==value,"Overlay key differs from original ROM");
    }
    for(uint32_t a:{0xB0000040u,0xB0000045u,0xB0000E14u,0xA4600014u}) {
        bool rejected=false;try{tooie_overlay_hardware_word(a);}catch(const std::exception&){rejected=true;}
        require(rejected,"Unexpected overlay hardware access accepted");
    }
    std::cout<<"PASS scoped RDRAM aliases and original-ROM PIO\n";
}
