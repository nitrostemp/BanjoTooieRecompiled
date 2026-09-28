// Synthetic dispatch/lifetime fixtures plus the ACTUAL generated relocation
// routine and retained Step0 relocation fixture. Not a live ROM-loader test.
#include "tooie_overlays.hpp"
#include "librecomp/overlays.hpp"
#include "native_fixture.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
extern "C" void ovl_relocate(uint8_t*,recomp_context*);
extern "C" void func_80081E30(uint8_t*,recomp_context*);

namespace {
using namespace tooie::overlays;
constexpr uint32_t base_a=0x80300000,base_b=0x80310000;
constexpr uint32_t stub_a=0x80082640,stub_b=0x80082840;
std::vector<Event> events;
bool nesting=false,try_active_unload=false,handler_called=false,throw_entry=false;
unsigned native_calls=0,unload_calls=0;
gpr guest(uint32_t value) { return static_cast<gpr>(static_cast<int32_t>(value)); }
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F call,const char* message) {
    bool rejected=false;try {call();}catch(const std::exception&){rejected=true;}require(rejected,message);
}
void event(const Event& value) {events.push_back(value);}
void fixture_entry(uint8_t* rdram,recomp_context* ctx) {
    ++native_calls;
    require(ctx->r4==0x12345678 && ctx->f12.u64==0x7ff0123456789abcULL,"Arguments corrupted by dispatch");
    if(throw_entry) throw std::runtime_error("Intentional native-entry failure");
    if(try_active_unload) rejects([&]{before_free(rdram,base_a);},"Active unload accepted");
    if(nesting) {
        nesting=false;
        dispatch_syscall(rdram,ctx,stub_b);
    }
    ctx->r2=guest(0xdeadbeef);ctx->r3=guest(0x87654321);
    ctx->f0.u64=0x7ff812340000abcdULL;ctx->f2.u64=0x1122334455667788ULL;
}
void request_boundary(uint8_t*,recomp_context* ctx) {
    require(ctx->r8==guest(stub_a),"Exception EPC was not passed through t0");
    require(ctx->r4==0x12345678,"Arguments clobbered before original handler boundary");
    handler_called=true;
    throw std::runtime_error("Intentional test boundary: original loader not executed by this fixture");
}
void fixture_alternate_return(uint8_t* rdram,recomp_context* ctx) {
    ++unload_calls;
    uint32_t header=static_cast<uint32_t>(ctx->r31)-0x20;
    require(header==base_a,"Alternate cleanup wrong return-address handoff");
    if(static_cast<uint8_t>(MEM_B(15,guest(header)))&4) {
        // Execute the ACTUAL original stack/return wrapper and original
        // ovl_unload's protected branch; this path requires no synthetic heap.
        func_80081E30(rdram,ctx);
        require(ctx->f0.u64==0x7ff8123400000001ULL,"Original wrapper's documented f0 spill alias changed");
        return;
    }
    const gpr saved_caller=guest(MEM_W(0x20,guest(header)));
    const gpr v0=ctx->r2,v1=ctx->r3;const uint64_t f0=ctx->f0.u64,f2=ctx->f2.u64;
    // Explicit test callback models only the documented header flag branch;
    // it does not claim to exercise original heap/free/list machinery.
    if(!(static_cast<uint8_t>(MEM_B(15,guest(header)))&4)) before_free(rdram,header);
    ctx->r2=0;ctx->r3=0;ctx->f0.u64=0;ctx->f2.u64=0;ctx->r31=0;
    (void)f0;
    ctx->r2=v0;ctx->r3=v1;ctx->f0.u64=(uint64_t(header)<<32)|1;ctx->f2.u64=f2;ctx->r31=saved_caller;
}
void make_image(uint8_t* rdram,uint32_t header,uint32_t stub,uint16_t id,bool alternate=false) {
    for(uint32_t i=0;i<0x150;++i) MEM_B(i,guest(header))=0;
    MEM_H(0,guest(header))=8;MEM_H(2,guest(header))=4;MEM_H(4,guest(header))=4;MEM_H(6,guest(header))=1;
    MEM_H(8,guest(header))=2;MEM_H(0x2c,guest(header))=id;MEM_H(0x2e,guest(header))=(stub-0x80082540)/8;
    MEM_W(0x34,guest(header))=header+0x40;
    MEM_W(0x38,guest(header))=header+0x40;MEM_W(0x3c,guest(header))=header+0x60;
    MEM_W(0,guest(stub))=0x08000000u|((header+0x10)>>2&0x03ffffff);
    MEM_W(4,guest(stub))=alternate?0x38080000:0x20080000;
    MEM_W(8,guest(stub))=MEM_W(0,guest(stub));MEM_W(12,guest(stub))=0x20080004;
    if(alternate) {
        MEM_W(0x10,guest(header))=0x0c000000u|((header+0x40)>>2&0x03ffffff);
        MEM_W(0x18,guest(header))=0x0c02078c;
        MEM_W(0x20,guest(header))=0x80012340;
    } else {
        MEM_W(0x10,guest(header))=0x3c090000|((header+0x38)>>16);
        MEM_W(0x14,guest(header))=0x35290000|((header+0x38)&0xffff);
        MEM_W(0x18,guest(header))=0x01095020;MEM_W(0x1c,guest(header))=0x8d4b0000;
        MEM_W(0x20,guest(header))=0x01600008;MEM_W(0x24,guest(header))=0xa520fffa;
    }
}
void context_init(recomp_context& ctx) {
    ctx={};ctx.f_odd=&ctx.f0.u32h;ctx.r31=guest(0x80012340);ctx.r4=0x12345678;
    ctx.r29=guest(0x80020000);ctx.f12.u64=0x7ff0123456789abcULL;
}
void verify_return(const recomp_context& ctx,uint64_t expected_f0=0x7ff812340000abcdULL) {
    require(ctx.r2==guest(0xdeadbeef) && ctx.r3==guest(0x87654321),"Integer returns lost");
    require(ctx.f0.u64==expected_f0 && ctx.f2.u64==0x1122334455667788ULL,"Floating return bits differ from original contract");
    require(ctx.r31==guest(0x80012340),"Caller RA lost");
    require(ctx.r29==guest(0x80020000),"Stack pointer changed");
    require(ctx.f_odd==&ctx.f0.u32h,"Native FPR pointer changed");
}
void actual_relocation_probe(uint8_t* rdram) {
    constexpr uint32_t records=0x80010000;
    constexpr uint16_t encoded_key=0x5a3c;
    unsigned kinds=0;
    for(uint32_t base:{0x8038fff0u,0x803afff0u}) {
        for(std::size_t i=0;i<std::size(original_words);++i) MEM_W(i*4,guest(base))=original_words[i];
        for(std::size_t i=0;i<std::size(packed_relocs);++i) MEM_H(i*2,guest(records))=packed_relocs[i]^encoded_key;
        recomp_context ctx{};ctx.f_odd=&ctx.f0.u32h;
        uint32_t delta=base-OVERLAY_VRAM;
        ctx.r8=guest(base);ctx.r9=guest(delta);ctx.r10=guest(records);ctx.r11=std::size(packed_relocs);ctx.r21=encoded_key;
        ovl_relocate(rdram,&ctx);
        for(std::size_t i=0;i<std::size(packed_relocs);++i) {
            unsigned kind=packed_relocs[i]&3,off=packed_relocs[i]&~3;
            kinds|=1u<<kind;
            uint32_t target=reloc_targets[i]+delta,expected=original_words[off/4];
            if(kind==0) expected=target;
            else if(kind==1) expected=(expected&0xfc000000)|((target>>2)&0x03ffffff);
            else if(kind==2) expected=(expected&0xffff0000)|(((target+0x8000)>>16)&0xffff);
            else expected=(expected&0xffff0000)|(target&0xffff);
            require(static_cast<uint32_t>(MEM_W(off,guest(base)))==expected,"Actual generated relocation differs from independent expectation");
        }
    }
    require(kinds==15,"Retained fixture does not cover all four relocation kinds");
}
}
int main() {
    try {
        require(current_callsite()==0,"Fresh thread has observed callsite");
        tooie_overlay_callsite_push(0x80012300);tooie_overlay_callsite_push(0x80800100);
        require(current_callsite()==0x80800100,"Nested observed callsite lost");
        tooie_overlay_callsite_pop();require(current_callsite()==0x80012300,"Outer observed callsite not restored");
        tooie_overlay_callsite_pop();require(current_callsite()==0,"Observed callsite not cleared");
        rejects([]{tooie_overlay_callsite_pop();},"Observed callsite underflow accepted");
        for(unsigned i=0;i<1024;++i)tooie_overlay_callsite_push(0x80012300);
        rejects([]{tooie_overlay_callsite_push(0x80012300);},"Unbounded observed callsite stack");
        for(unsigned i=0;i<1024;++i)tooie_overlay_callsite_pop();
        std::vector<uint8_t> memory(8*1024*1024);auto* rdram=memory.data();
        FuncEntry functions[]={{fixture_entry,0,0x20},{fixture_entry,0x20,0x20}};
        SectionTableEntry table[]={{0x2000000,0x80800000,0x100,functions,2,nullptr,0,0},
                                  {0x2001000,0x80800000,0x100,functions,2,nullptr,0,1}};
        std::array<int,885> ids;ids.fill(-1);ids[350]=0;ids[679]=1;
        recomp::overlays::register_overlays({table,2,2},{ids.data(),ids.size()});
        recomp::overlays::init_overlays();
        configure(table,ids,{request_boundary,fixture_alternate_return,event});
        recomp_context ctx;context_init(ctx);
        MEM_W(0,guest(stub_a))=(350<<6)|12;MEM_W(4,guest(stub_a))=0x20080000;
        auto request=decode_request(rdram,stub_a);require(request.id==350 && !request.already_patched,"Original request decoding wrong");
        // Original ROM/ELF words at 800888B8: gldbDll entry2, id708.
        MEM_W(0,guest(0x800888b8))=0x0000b10c;MEM_W(4,guest(0x800888b8))=0x38080008;
        auto original_xori=decode_request(rdram,0x800888b8);
        require(original_xori.id==708&&original_xori.alternate&&original_xori.entry_offset==8&&original_xori.table_start==0x800888a8,"Original XORI request decoding wrong");
        MEM_W(0,guest(stub_a))=(1u<<25)|(350<<6)|12;MEM_W(4,guest(stub_a))=0x38080004;
        auto flagged=decode_request(rdram,stub_a);
        require(flagged.load_flag==1 && flagged.alternate && flagged.entry_offset==4 && flagged.table_start==stub_a-8,"Request flags/group decoding wrong");
        for(uint32_t malformed:{0x30080004u,0x34080004u,0x38090004u,0x38080002u,0x38088000u}){
            MEM_W(4,guest(stub_a))=malformed;
            rejects([&]{decode_request(rdram,stub_a);},"Unsupported opcode/register/offset accepted");
        }
        MEM_W(4,guest(stub_a))=0x30080004;
        tooie_overlay_callsite_push(0x800da0ec);
        try{decode_request(rdram,stub_a);throw std::logic_error("ANDI should be rejected");}
        catch(const std::runtime_error& error){
            const std::string detail=error.what();
            require(detail.find("first_word=0x0200578c")!=std::string::npos&&detail.find("delay_word=0x30080004")!=std::string::npos&&detail.find("observed_callsite=0x800da0ec")!=std::string::npos,"Decode failure lacks original words/callsite");
        }
        tooie_overlay_callsite_pop();
        MEM_W(0,guest(stub_a))=(350<<6)|12;MEM_W(4,guest(stub_a))=0x20080000;
        rejects([&]{dispatch_syscall(rdram,&ctx,stub_a);},"Original handler boundary did not run");
        require(handler_called && !resident(350),"Request boundary fabricated a residency");
        MEM_W(0,guest(stub_a))=(351<<6)|12;
        rejects([&]{dispatch_syscall(rdram,&ctx,stub_a);},"Uncompiled stable ID accepted");
        MEM_W(4,guest(stub_a))=0xdeadbeef;
        rejects([&]{decode_request(rdram,stub_a);},"Malformed delay word accepted");
        rejects([&]{read_layout(rdram,0x807ffff0);},"Truncated header accepted");
        make_image(rdram,base_a,stub_a,350);make_image(rdram,base_b,stub_b,679);
        MEM_W(0x140,guest(base_a))=1;
        rejects([&]{after_relocate(rdram,base_a,0);},"Dirty first-load BSS accepted");
        require(!resident(350) && section_addresses[0]==static_cast<int32_t>(0x80800000),"Failed publication changed mappings");
        MEM_W(0x140,guest(base_a))=0;
        after_relocate(rdram,base_a,0);after_relocate(rdram,base_b,0);
        require(resident(350)&&resident(679),"Two simultaneous overlays not published");
        require(get_function(static_cast<int32_t>(base_a+0x40))==fixture_entry,"Native mapping missing");
        context_init(ctx);nesting=true;try_active_unload=true;
        dispatch_syscall(rdram,&ctx,stub_a);verify_return(ctx);
        require(native_calls==2,"Nested dispatch did not invoke both real native callbacks");
        try_active_unload=false;
        // An XORI entry reached after another entry installed a NORMAL thunk
        // must execute t0=entry_offset, not confuse its loader flag with ANDI.
        MEM_W(12,guest(stub_a))=0x38080004;
        context_init(ctx);dispatch_syscall(rdram,&ctx,stub_a+8);verify_return(ctx);
        require(ctx.r8==4&&events.back().entry==base_a+0x60&&resident(350)&&unload_calls==0,"XORI offset/normal installed thunk contract violated");
        MEM_B(15,guest(base_a))=4;
        rejects([&]{before_free(rdram,base_a);},"Protected unload accepted");
        MEM_B(15,guest(base_a))=0;
        before_free(rdram,base_a);
        rejects([&]{dispatch_syscall(rdram,&ctx,stub_a);},"Unloaded address accepted");
        make_image(rdram,base_a,stub_a,350,true);after_relocate(rdram,base_a,0);
        require(generation(350)==2,"Reload generation not advanced");
        context_init(ctx);dispatch_syscall(rdram,&ctx,stub_a);verify_return(ctx,(uint64_t(base_a)<<32)|1);
        require(unload_calls==1 && !resident(350),"Alternate thunk did not conditionally unload");
        make_image(rdram,base_a,stub_a,350,true);after_relocate(rdram,base_a,0);
        MEM_B(15,guest(base_a))=4;
        context_init(ctx);ctx.r31=guest(0x80054320);
        // A normal-flagged second stub still jumps to the installed alternate
        // thunk, which returns via its stored caller and may refuse to unload.
        dispatch_syscall(rdram,&ctx,stub_a+8);verify_return(ctx,0x7ff8123400000001ULL);
        require(unload_calls==2 && resident(350),"Protected alternate thunk incorrectly unmapped residency");
        MEM_B(15,guest(base_a))=0;before_free(rdram,base_a);
        make_image(rdram,base_a,stub_a,350);after_relocate(rdram,base_a,0);
        context_init(ctx);throw_entry=true;
        rejects([&]{dispatch_syscall(rdram,&ctx,stub_a);},"Native failure was swallowed");
        throw_entry=false;
        // The later move also proves the exceptional entry released its frame.
        MEM_W(0x140,guest(base_a))=0x13579bdf; // Dirty BSS must survive movement.
        constexpr uint32_t moved=base_a+0x8000;
        std::memcpy(rdram+(moved&0xffffff),rdram+(base_a&0xffffff),0x150);
        MEM_W(0x34,guest(moved))=moved+0x40;MEM_W(0x38,guest(moved))=moved+0x40;MEM_W(0x3c,guest(moved))=moved+0x60;
        after_relocate(rdram,moved,0x8000);
        require(static_cast<uint32_t>(MEM_W(0x140,guest(moved)))==0x13579bdf,"Movement cleared dirty BSS");
        rejects([&]{dispatch_syscall(rdram,&ctx,stub_a);},"Stale old-header jump accepted after move");
        require(section_addresses[0]==static_cast<int32_t>(moved+0x40),"Runtime delta mapping wrong");
        require(get_function(static_cast<int32_t>(moved+0x40))==fixture_entry,"Moved mapping missing");
        before_free(rdram,moved);before_free(rdram,base_b);
        actual_relocation_probe(rdram);
        std::cout<<"PASS Tooie overlay adapter: request decoding; handler boundary; missing ID rejection; two residencies; nested arguments/returns; flags; active-frame rejection; unload/reload; delta move and dirty BSS; stale dispatch rejection; actual original relocation at 2 bases with encoded fixture key and all 4 relocation kinds\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
