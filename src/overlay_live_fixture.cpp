#include "overlay_live_fixture.hpp"
#include "tooie_overlays.hpp"
#include "hardware_access.hpp"
#include "runtime_lifecycle.hpp"
#include "main_metadata.hpp"
#include "game.hpp"
#include "ultramodern/ultramodern.hpp"
#include <algorithm>
#include <array>
#include <exception>
#include <vector>
#include <stdexcept>
extern "C" {
void heap_alloc_sided(uint8_t*,recomp_context*);
void heap_free(uint8_t*,recomp_context*);
void find_free_block(uint8_t*,recomp_context*);
void defragment_overlays(uint8_t*,recomp_context*);
void func_8001B840(uint8_t*,recomp_context*); // Original defrag byte-budget reset.
void ovl_unload(uint8_t*,recomp_context*);
void _chweldarbossfireball_entrypoint_0(uint8_t*,recomp_context*);
void _chbaddieDll_entrypoint_0(uint8_t*,recomp_context*);
void _gcstatusDll_entrypoint_0(uint8_t*,recomp_context*);
}
namespace {
bool enabled=false,started=false;
constexpr uint32_t stub350=0x80086628,stub181=0x80085920,stub679=0x80088310;
constexpr uint32_t loaded_count=0x80117c60,loaded_list=0x80126738,heap_used=0x8003f750;
gpr guest(uint32_t x){return static_cast<gpr>(static_cast<int32_t>(x));}
void check(bool ok,const std::string& detail){if(!ok)throw std::runtime_error("Live overlay fixture: "+detail);}
uint32_t word(uint8_t* rdram,uint32_t a){check(a>=0x80000400&&uint64_t(a)+4<=0x80800000&&a%4==0,"invalid word address");return MEM_W(0,guest(a));}
uint16_t half(uint8_t* rdram,uint32_t a){check(a>=0x80000400&&uint64_t(a)+2<=0x80800000&&a%2==0,"invalid halfword address");return MEM_HU(0,guest(a));}
std::vector<uint8_t> bytes(uint8_t* rdram,uint32_t a,uint32_t n){check(a>=0x80000400&&uint64_t(a)+n<=0x80800000,"invalid byte range");std::vector<uint8_t> out(n);for(uint32_t i=0;i<n;++i)out[i]=MEM_BU(i,guest(a));return out;}
void restore(uint8_t* rdram,uint32_t a,const std::vector<uint8_t>& data){for(uint32_t i=0;i<data.size();++i)MEM_B(i,guest(a))=data[i];}
void event(const char* name,tooie::Json details=tooie::Json::object()){
    details["isolated_fixture"]=true;details["gameplay_continuation"]=false;
    tooie::trace(name,"G4-live-fixture","pass",0,nullptr,std::move(details));
}
struct SavedGuest {
    uint8_t* ram;recomp_context* context;recomp_context saved;
    std::vector<uint8_t> stack;
    SavedGuest(uint8_t* r,recomp_context* c):ram(r),context(c),saved(*c),stack(bytes(r,tooie::main_meta::main_stack_start,tooie::main_meta::main_stack_size)){}
    ~SavedGuest(){restore(ram,tooie::main_meta::main_stack_start,stack);*context=saved;}
};
struct ContextImage {
    std::array<uint64_t,66> registers;
    uint32_t* f_odd;
    uint32_t status;
    uint8_t float_mode;
    bool operator==(const ContextImage&) const=default;
};
ContextImage meaningful_context(const recomp_context& c){
    // Named values deliberately exclude struct/union padding. FPR bit patterns
    // are compared as integers, so NaN payloads are preserved exactly.
    return {{c.r0,c.r1,c.r2,c.r3,c.r4,c.r5,c.r6,c.r7,
        c.r8,c.r9,c.r10,c.r11,c.r12,c.r13,c.r14,c.r15,
        c.r16,c.r17,c.r18,c.r19,c.r20,c.r21,c.r22,c.r23,
        c.r24,c.r25,c.r26,c.r27,c.r28,c.r29,c.r30,c.r31,
        c.f0.u64,c.f1.u64,c.f2.u64,c.f3.u64,c.f4.u64,c.f5.u64,c.f6.u64,c.f7.u64,
        c.f8.u64,c.f9.u64,c.f10.u64,c.f11.u64,c.f12.u64,c.f13.u64,c.f14.u64,c.f15.u64,
        c.f16.u64,c.f17.u64,c.f18.u64,c.f19.u64,c.f20.u64,c.f21.u64,c.f22.u64,c.f23.u64,
        c.f24.u64,c.f25.u64,c.f26.u64,c.f27.u64,c.f28.u64,c.f29.u64,c.f30.u64,c.f31.u64,
        c.hi,c.lo},c.f_odd,c.status_reg,c.mips3_float_mode};
}
uint32_t call(uint8_t* ram,recomp_context* ctx,recomp_func_t* fn,uint32_t a0=0,uint32_t a1=0){
    uint32_t sp=ctx->r29;ctx->r4=guest(a0);ctx->r5=guest(a1);fn(ram,ctx);
    check(static_cast<uint32_t>(ctx->r29)==sp,"original call did not restore SP");return static_cast<uint32_t>(ctx->r2);
}
uint32_t allocate(uint8_t* ram,recomp_context* ctx,uint32_t n){
    uint32_t p=call(ram,ctx,heap_alloc_sided,n,0);
    check(p>=0x80000410&&uint64_t(p)+n<=0x80800000&&p%16==0,"original allocator returned invalid fixture block");return p;
}
uint32_t header(uint8_t* ram,uint32_t stub,uint32_t id){
    uint32_t instruction=word(ram,stub);check((instruction&0xfc000000)==0x08000000,"original stub is not patched J");
    uint32_t result=((instruction&0x03ffffff)<<2)|(stub&0xf0000000);result-=0x10;
    check(half(ram,result+0x2c)==id&&tooie::overlays::resident(id),"stub/residency ID mismatch");return result;
}
uint32_t rom_word(std::span<const uint8_t> rom,uint32_t p){check(uint64_t(p)+4<=rom.size(),"ROM metadata outside original image");return uint32_t(rom[p])<<24|uint32_t(rom[p+1])<<16|uint32_t(rom[p+2])<<8|rom[p+3];}
uint32_t original_allocation_size(uint8_t* ram,uint32_t id){
    auto rom=recomp::get_rom();uint32_t table=word(ram,0x80126730);
    uint32_t first=table+rom_word(rom,table+(id-1)*4),last=table+rom_word(rom,table+id*4);
    check(first<last&&last<=rom.size(),"original overlay extent invalid");
    uint32_t bss=(rom_word(rom,first+4)&0xffff)*16;
    if(rom[first+15]&0x80)return 16+((rom_word(rom,first+16)>>16)*16)+bss;
    return last-first+bss;
}
std::vector<uint16_t> records(uint8_t* ram,const tooie::overlays::Layout& layout){
    uint16_t key=static_cast<uint16_t>(tooie_overlay_hardware_word(0xb0000040+layout.id*4));
    std::vector<uint16_t> out;for(uint32_t i=0;i<layout.packed_count;++i)out.push_back(half(ram,layout.packed_relocs+i*2)^key);return out;
}
uint32_t beword(const std::vector<uint8_t>& v,uint32_t p){check(uint64_t(p)+4<=v.size(),"snapshot relocation outside image");return uint32_t(v[p])<<24|uint32_t(v[p+1])<<16|uint32_t(v[p+2])<<8|v[p+3];}
void put(std::vector<uint8_t>& v,uint32_t p,uint32_t w){check(uint64_t(p)+4<=v.size(),"expected relocation outside image");for(uint32_t i=0;i<4;++i)v[p+i]=static_cast<uint8_t>(w>>(24-i*8));}
std::vector<uint8_t> moved_expectation(const std::vector<uint8_t>& old,const std::vector<uint16_t>& relocs,int32_t delta){
    auto expected=old;uint32_t hi=0;bool have_hi=false;
    for(size_t i=0;i<relocs.size();++i){
        uint32_t kind=relocs[i]&3,off=relocs[i]&~3,original=beword(old,off),value=original;
        if(kind==0)value+=delta;
        else if(kind==1)value=(original&0xfc000000)|(((original&0x03ffffff)+static_cast<uint32_t>(delta/4))&0x03ffffff);
        else if(kind==2){
            check(i+1<relocs.size()&&(relocs[i+1]&3)==3,"HI16 lacks following LO16");
            hi=(original&0xffff)<<16;have_hi=true;
            int32_t low=static_cast<int16_t>(beword(old,relocs[i+1]&~3)&0xffff);
            uint32_t target=hi+low+delta;value=(original&0xffff0000)|(((target+0x8000)>>16)&0xffff);
        }else{
            check(have_hi,"LO16 lacks HI16");uint32_t target=hi+static_cast<int16_t>(original&0xffff)+delta;
            value=(original&0xffff0000)|(target&0xffff);
        }
        put(expected,off,value);
    }
    return expected;
}
void unload(uint8_t* ram,recomp_context* ctx,uint32_t stub,uint32_t id){
    uint32_t p=header(ram,stub,id),before=word(ram,loaded_count);
    check(call(ram,ctx,ovl_unload,p,1)==1,"original unload refused ordinary image");
    check(!tooie::overlays::resident(id)&&word(ram,loaded_count)+1==before,"unload did not remove residency/list entry");
    auto request=tooie::overlays::decode_request(ram,stub);
    check(!request.already_patched&&request.id==id,"original unload did not restore syscall word");
    event("overlay_live_fixture_unload",{{"id",id},{"header",tooie::hex32(p)}});
}
void conditional_unload(uint8_t* rdram,recomp_context* ctx){
    const uint32_t syscall=word(rdram,stub350),delay=word(rdram,stub350+4);
    const auto request=tooie::overlays::decode_request(rdram,stub350);
    check(!request.already_patched&&request.id==350&&!request.alternate&&request.entry_offset==0,
        "conditional unload requires restored ordinary350 entry0");
    const uint32_t heap_before=word(rdram,heap_used),count_before=word(rdram,loaded_count);
    const uint32_t caller=ctx->r31;const uint64_t generation=tooie::overlays::generation(350);
    // Inject the original XORI request form. Original syscall_handler extracts
    // it at 80081EC4..D4; original func_80081F64 installs both alternate JALs.
    // Never synthesize the heap thunk or call the return wrapper in isolation.
    struct RestoreDelay {uint8_t* ram;uint32_t value;~RestoreDelay(){uint8_t* rdram=ram;MEM_W(4,guest(stub350))=value;}} restore_delay{rdram,delay};
    MEM_W(4,guest(stub350))=delay|0x18000000u;
    event("overlay_live_fixture_conditional_request",{{"id",350},{"injected_fixture_word",true},
        {"original_delay_word",tooie::hex32(delay)},{"injected_delay_word",tooie::hex32(delay|0x18000000u)},
        {"encoding","original XORI form; fixture-selected entry"},
        {"source_flag_extract_pc","0x80081EC4"},{"natural_game_callsite_claimed",false}});
    const uint32_t result=call(rdram,ctx,_chweldarbossfireball_entrypoint_0);
    check(!tooie::overlays::resident(350)&&tooie::overlays::generation(350)==generation+1,
        "original alternate path did not load and unload350 exactly once");
    check(word(rdram,loaded_count)==count_before&&word(rdram,heap_used)==heap_before,
        "original alternate path did not restore list/heap accounting");
    check(word(rdram,stub350)==syscall&&word(rdram,stub350+4)==(delay|0x18000000u),
        "original alternate unload did not restore the syscall encoding");
    check(static_cast<uint32_t>(ctx->r31)==caller,"original alternate return lost stored caller RA");
    check(result>=0x80000400&&result<0x80800000,"alternate getter return outside RDRAM");
    // Successful ovl_unload's original o32 argument spills alias saved f0.
    // Correlate its header/high half and v0 with the publication event in the
    // harness; the returned descriptor is already freed and is not dereferenced.
    event("overlay_live_fixture_conditional_unload",{{"id",350},{"generation",generation+1},
        {"returned_descriptor",tooie::hex32(result)},{"f0_high_word",tooie::hex32(ctx->f0.u64>>32)},
        {"f0_low_word",tooie::hex32(ctx->f0.u64)},{"stored_caller_ra_restored",true},
        {"heap_before",heap_before},{"heap_after",word(rdram,heap_used)},
        {"loaded_count_before",count_before},{"loaded_count_after",word(rdram,loaded_count)},
        {"original_wrapper","0x80081E30"},{"freed_descriptor_dereferenced",false}});
}
void fixture(uint8_t* ram,recomp_context* ctx){
    uint8_t* rdram=ram; // Pinned recomp MEM_* macros require this local name.
    check(static_cast<uint32_t>(ultramodern::this_thread())==tooie::main_meta::main_thread,"fixture requires original main worker");
    uint32_t initial_sp=static_cast<uint32_t>(ctx->r29);
    check(initial_sp>=tooie::main_meta::main_stack_start+0x1000&&initial_sp<=tooie::main_meta::main_stack_top-0x20,"fixture SP outside original main stack");
    SavedGuest saved(ram,ctx);
    const uint32_t heap_before=word(ram,heap_used);check(word(ram,loaded_count)==0,"fixture must precede first live overlay load");
    check(word(ram,0x80126cb8)==0,"fixture requires original low-side overlay allocation mode");
    event("overlay_live_fixture_begin",{{"heap_used",heap_before},{"saved_main_stack_bytes",saved.stack.size()}});
    const uint32_t scratch=allocate(ram,ctx,32),allocation=original_allocation_size(ram,350);
    std::vector<uint32_t> guards;uint32_t predecessor=0;
    // Find a bounded, actual allocator-produced adjacent pair. No allocator
    // state or original guest addresses are manufactured by the fixture.
    for(unsigned attempt=0;attempt<16&&!predecessor;++attempt){
        uint32_t guard=allocate(ram,ctx,allocation);guards.push_back(guard);
        uint32_t next=word(ram,guard-12);
        uint32_t candidate=call(ram,ctx,find_free_block,allocation,0);
        if(candidate==next)predecessor=guard;
    }
    check(predecessor!=0,"could not establish original adjacent allocation within 16 attempts");
    call(ram,ctx,_chweldarbossfireball_entrypoint_0);
    uint32_t first=header(ram,stub350,350);auto layout=tooie::overlays::read_layout(ram,first);
    check(word(ram,first-16)==predecessor-16,"original overlay was not allocated after selected guard");
    check(static_cast<uint32_t>(ctx->r2)==layout.text+0x410,"original overlay350 descriptor return is wrong");
    call(ram,ctx,_chbaddieDll_entrypoint_0,scratch);
    check(MEM_BU(4,guest(scratch))==0&&MEM_BU(5,guest(scratch))==1,"original181 entry did not initialize argument bytes");
    call(ram,ctx,_gcstatusDll_entrypoint_0);
    check(word(ram,loaded_count)==3&&tooie::overlays::resident(350)&&tooie::overlays::resident(181)&&tooie::overlays::resident(679),"three original overlays not simultaneous");
    event("overlay_live_fixture_simultaneous",{{"ids",{350,181,679}},{"source_expected_gcstatus_nested_local_calls",8},{"cross_overlay_nested_scenario","separate focused adapter test"}});
    auto relocs=records(ram,layout);uint32_t dirty_offset=layout.bss-layout.text-4;
    uint32_t text_bytes=half(ram,first)*16;
    while(dirty_offset>=text_bytes&&std::any_of(relocs.begin(),relocs.end(),[&](uint16_t r){return (r&~3u)==dirty_offset;}))dirty_offset-=4;
    check(dirty_offset>=text_bytes&&layout.end-layout.bss>=4,"selected350 lacks non-relocation data/BSS fixture storage");
    constexpr uint32_t dirty_data=0x2468ace0,dirty_bss=0x13579bdf;
    MEM_W(dirty_offset,guest(layout.text))=dirty_data;MEM_W(0,guest(layout.bss))=dirty_bss;
    auto initialized_before=bytes(ram,layout.text,layout.bss-layout.text),bss_before=bytes(ram,layout.bss,layout.end-layout.bss);
    uint32_t old_jump=word(ram,stub350);uint64_t before_generation=tooie::overlays::generation(350);
    call(ram,ctx,heap_free,predecessor);guards.erase(std::find(guards.begin(),guards.end(),predecessor));
    call(ram,ctx,func_8001B840); // Reset original defrag byte budget.
    call(ram,ctx,defragment_overlays);
    uint32_t moved=header(ram,stub350,350);auto current=tooie::overlays::read_layout(ram,moved);
    int32_t delta=static_cast<int32_t>(moved-first);
    check(delta!=0&&tooie::overlays::generation(350)==before_generation+1,"original defrag did not move350 exactly once");
    check(bytes(ram,current.text,current.bss-current.text)==moved_expectation(initialized_before,relocs,delta),"actual original move differs from independent dirty-image relocation expectation");
    check(bytes(ram,current.bss,current.end-current.bss)==bss_before,"actual original defrag changed dirty BSS");
    check(word(ram,loaded_list+half(ram,moved+0x30)*4)==moved,"original defrag did not update loaded-list address");
    check(word(ram,current.text+dirty_offset)==dirty_data&&word(ram,current.bss)==dirty_bss,"dirty fixture fields lost");
    call(ram,ctx,_chweldarbossfireball_entrypoint_0);check(static_cast<uint32_t>(ctx->r2)==current.text+0x410,"moved native entry used stale section base");
    // Deliberate negative test on the actual original stub, restored immediately.
    uint32_t new_jump=word(ram,stub350);MEM_W(0,guest(stub350))=old_jump;bool rejected=false;
    try{tooie::overlays::dispatch_syscall(ram,ctx,stub350);}catch(const std::runtime_error&){rejected=true;}
    MEM_W(0,guest(stub350))=new_jump;check(rejected,"stale original jump was accepted after move");
    event("overlay_live_fixture_move",{{"id",350},{"first_header",tooie::hex32(first)},{"moved_header",tooie::hex32(moved)},{"delta",delta},{"initialized_bytes_checked",initialized_before.size()},{"dirty_bss_bytes_checked",bss_before.size()},{"packed_relocations_checked",relocs.size()},{"stale_dispatch_rejected",true},{"direct_missing_runtime_lookup_tested",false}});
    uint8_t flags=MEM_BU(15,guest(moved));MEM_B(15,guest(moved))=flags|4;
    uint32_t refusal=call(ram,ctx,ovl_unload,moved,1);
    check(tooie::overlays::resident(350),"protected original unload removed350");MEM_B(15,guest(moved))=flags;
    check(refusal==0&&word(ram,loaded_count)==3,"protected original unload changed loaded list");
    event("overlay_live_fixture_protected_unload",{{"id",350},{"original_return",refusal}});
    unload(ram,ctx,stub350,350);
    uint32_t pad=allocate(ram,ctx,256);guards.push_back(pad);
    call(ram,ctx,_chweldarbossfireball_entrypoint_0);
    uint32_t reloaded=header(ram,stub350,350);auto fresh=tooie::overlays::read_layout(ram,reloaded);
    check(reloaded!=first&&reloaded!=moved,"actual reload failed to obtain a third allocation base");
    check(static_cast<uint32_t>(ctx->r2)==fresh.text+0x410&&word(ram,fresh.bss)==0&&word(ram,fresh.text+dirty_offset)!=dirty_data,"actual reload retained stale dirty state or native base");
    event("overlay_live_fixture_reload",{{"id",350},{"first_header",tooie::hex32(first)},{"moved_header",tooie::hex32(moved)},{"reloaded_header",tooie::hex32(reloaded)},{"generation",tooie::overlays::generation(350)}});
    unload(ram,ctx,stub679,679);unload(ram,ctx,stub181,181);unload(ram,ctx,stub350,350);
    conditional_unload(ram,ctx);
    for(auto it=guards.rbegin();it!=guards.rend();++it)call(ram,ctx,heap_free,*it);
    call(ram,ctx,heap_free,scratch);
    check(word(ram,loaded_count)==0&&word(ram,heap_used)==heap_before,"fixture did not restore original loaded-list count and heap byte accounting");
    event("overlay_live_fixture_resources_restored",{{"heap_before",heap_before},{"heap_after",word(ram,heap_used)},{"loaded_count",word(ram,loaded_count)}});
}
}
namespace tooie {
void enable_live_overlay_fixture(bool value){check(!started,"fixture mode cannot change after entry");enabled=value;}
bool maybe_run_live_overlay_fixture(uint8_t* ram,recomp_context* ctx,uint32_t stub){
    if(!enabled||started)return false;
    check(stub==stub679,"first fixture request is not original gcstatus startup stub");started=true;
    try{
        const auto original_context=meaningful_context(*ctx);
        const auto original_stack=bytes(ram,main_meta::main_stack_start,main_meta::main_stack_size);
        fixture(ram,ctx); // RAII restores original context and complete main stack.
        check(meaningful_context(*ctx)==original_context,"post-fixture named context fields differ from original snapshot");
        check(bytes(ram,main_meta::main_stack_start,main_meta::main_stack_size)==original_stack,"post-fixture original main stack bytes differ from snapshot");
        event("overlay_live_fixture_context_verified",{{"stack_bytes_compared",original_stack.size()},
            {"gpr_values_compared",32},{"fpr_bit_patterns_compared",32},{"hi_lo_compared",true},
            {"f_odd_pointer_compared",true},{"status_and_float_mode_compared",true},{"padding_compared",false}});
        event("overlay_live_fixture_complete",{{"original_context_and_stack_restored",true},{"post_restore_comparison_passed",true},{"terminal_clean_stop_requested",true}});
    }catch(const std::exception& error){
        trace("overlay_live_fixture_failed","G4-live-fixture","failure",stub,ctx,{{"message",error.what()},{"isolated_fixture",true},{"gameplay_continuation",false}});throw;
    }
    lifecycle::request_stop();lifecycle::poll();
    throw std::logic_error("Fixture stop poll unexpectedly returned");
}
}
