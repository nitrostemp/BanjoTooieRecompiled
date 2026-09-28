#include "tooie_overlays.hpp"
#include "librecomp/overlays.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "persistent_state_continuation.hpp"
#endif

extern "C" void load_overlay_by_id(uint32_t,uint32_t);
extern "C" void unload_overlay_by_id(uint32_t);
namespace {
constexpr uint32_t ram_first=0x80000400,ram_end=0x80800000,syscall_first=0x80082540;
std::span<const SectionTableEntry> sections;
std::span<const int> stable_ids;
tooie::overlays::Callbacks callbacks;
struct Resident { uint32_t header,text; uint64_t generation; unsigned active=0; };
std::unordered_map<uint32_t,Resident> residents;
std::unordered_map<uint32_t,uint64_t> generations;
thread_local std::vector<uint32_t> callsites;
gpr guest(uint32_t value) { return static_cast<gpr>(static_cast<int32_t>(value)); }
std::string hex(uint32_t value) { std::ostringstream out;out<<"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<value;return out.str(); }
void require(bool value,const char* message) {
    if(!value) throw std::runtime_error(std::string("Tooie overlay: ")+message);
}
template<class Message> void require_lazy(bool value,Message&& message) {
    if(!value) throw std::runtime_error("Tooie overlay: "+message());
}
void span_check(uint32_t start,uint64_t size,unsigned alignment=1) {
    require_lazy(start>=ram_first && start<ram_end && start%alignment==0 && uint64_t(start)+size<=ram_end, [&]() { return "invalid guest range "+hex(start)+" size "+std::to_string(size); });
}
uint32_t word(uint8_t* rdram,uint32_t address) { span_check(address,4,4);return static_cast<uint32_t>(MEM_W(0,guest(address))); }
uint16_t half(uint8_t* rdram,uint32_t address) { span_check(address,2,2);return MEM_HU(0,guest(address)); }
uint32_t align(uint32_t value,uint32_t amount) { return (value+amount-1)&~(amount-1); }
std::size_t section_index(uint32_t id) {
    require_lazy(id && id<stable_ids.size(), [&]() { return "invalid stable id "+std::to_string(id); });
    int index=stable_ids[id];
    require_lazy(index>=0 && static_cast<std::size_t>(index)<sections.size(), [&]() { return "empty or uncompiled stable id "+std::to_string(id); });
    return static_cast<std::size_t>(index);
}
void notify(const char* operation,uint32_t id,const Resident& live,uint32_t entry=0,uint32_t caller=0) {
    if(callbacks.event) callbacks.event({operation,id,live.header,live.text,entry,caller,live.generation});
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
constexpr uint32_t persistent_dispatch_id=0xF0000101;
constexpr uint32_t persistent_loader_return=1,persistent_target_return=2,persistent_alternate_return=3;
void persistent_dispatch_entry(uint8_t* rdram,recomp_context* ctx) {
    tooie::overlays::dispatch_syscall(rdram,ctx,0);
}
void persistent_finish_dispatch(uint8_t* rdram,recomp_context* ctx) {
    auto& machine=tooie::continuation::Machine::current();
    const auto saved=machine.top(); // Never retain a vector reference across calls.
    if(saved.pc==persistent_target_return) {
        const auto id=static_cast<uint32_t>(saved.operands[1]);
        auto& live=residents.at(id);
        require(live.generation==saved.hi&&live.active,"restored dispatch residency mismatch");
        --live.active;
        notify("return",id,live,static_cast<uint32_t>(saved.operands[2]),static_cast<uint32_t>(saved.operands[3]));
        if(saved.result) {
            require(callbacks.original_alternate_return,"alternate-return function missing");
            ctx->r31=guest(static_cast<uint32_t>(saved.lo)+0x20);
            machine.top().pc=persistent_alternate_return;
            machine.validate_function_pointer(callbacks.original_alternate_return)(rdram,ctx);
        }
    } else require(saved.pc==persistent_loader_return||saved.pc==persistent_alternate_return,"invalid dispatch resume stage");
    machine.leave(persistent_dispatch_id);
}
#endif
}

namespace tooie::overlays {
void configure(std::span<const SectionTableEntry> new_sections,std::span<const int> ids,Callbacks cb) {
    require(residents.empty(),"cannot configure while overlays are resident");
    sections=new_sections;stable_ids=ids;callbacks=cb;generations.clear();
}
Layout read_layout(uint8_t* rdram,uint32_t header) {
    span_check(header,0x38,16);
    uint32_t text_units=half(rdram,header),rodata_units=half(rdram,header+2),data_units=half(rdram,header+4),bss_units=half(rdram,header+6);
    Layout result{};
    result.header=header;result.entry_table=header+0x38;
    result.entries=half(rdram,header+8);result.packed_count=half(rdram,header+10);result.secondary_count=half(rdram,header+12);
    auto name_bytes=static_cast<uint8_t>(MEM_B(0,guest(header+14)));
    result.flags=static_cast<uint8_t>(MEM_B(0,guest(header+15)));
    result.id=half(rdram,header+0x2c);result.syscall_index=half(rdram,header+0x2e);
    require(result.entries>0 && result.entries<0x8000 && result.packed_count<0x8000 && result.secondary_count<0x8000,
            "invalid signed header entry/relocation count");
    require(text_units>0 && text_units<0x8000 && rodata_units<0x8000 && data_units<0x8000 && bss_units<0x8000,
            "invalid signed header section sizes");
    result.packed_relocs=align(result.entry_table+result.entries*4+name_bytes,4);
    result.secondary_relocs=align(result.packed_relocs+result.packed_count*2,4);
    result.text=align(result.secondary_relocs+result.secondary_count*4,16);
    result.bss=result.text+(text_units+rodata_units+data_units)*16;
    result.end=result.bss+bss_units*16;
    span_check(header,uint64_t(result.end)-header,16);
    return result;
}
Request decode_request(uint8_t* rdram,uint32_t stub) {
    uint32_t first=0,delay=0;bool words_available=false;
    try {
    span_check(stub,8,8);
    require(stub>=syscall_first,"syscall stub precedes original dispatch table");
    first=word(rdram,stub);delay=word(rdram,stub+4);words_available=true;
    Request request{};request.stub=stub;request.word=first;
    // Original ROM/ELF table contains only ADDI and XORI t0,zero,entry*4.
    // Both execute t0=entry*4; syscall_handler separately extracts bit28
    // from XORI as its high-side-load/alternate-thunk flag. ANDI is not an
    // original table encoding and would have a different executed result.
    require((delay&0xffff0000u)==0x20080000u||(delay&0xffff0000u)==0x38080000u,
            "unknown entry delay-word encoding");
    request.alternate=(delay&0x10000000u)!=0;
    request.entry_offset=delay&0xffff;
    require(request.entry_offset<0x8000 && request.entry_offset%4==0,"invalid entry-table offset");
    uint32_t rewind=request.entry_offset*2;
    require(stub>=syscall_first+rewind,"entry offset underflows syscall group");
    request.table_start=stub-rewind;
    if ((request.word&0xFC00003Fu)==0x0000000Cu) {
        request.id=(request.word>>6)&0x7ffff;
        request.load_flag=(request.word>>25)&1;
    } else {
        require_lazy((request.word&0xFC000000u)==0x08000000u, [&]() { return "neither syscall nor patched J at "+hex(stub); });
        request.already_patched=true;
        uint32_t hook=(request.word<<2 & 0x0fffffffu)|(stub&0xf0000000u);
        require(hook>=ram_first+0x10,"invalid patched hook address");
        request.id=half(rdram,hook-0x10+0x2c);
    }
    return request;
    } catch(const std::runtime_error& error) {
        throw std::runtime_error(std::string(error.what())+" stub="+hex(stub)+
            " first_word="+(words_available?hex(first):"unavailable")+
            " delay_word="+(words_available?hex(delay):"unavailable")+
            " observed_callsite="+hex(current_callsite()));
    }
}
bool resident(uint32_t id) { return residents.contains(id); }
uint64_t generation(uint32_t id) { auto it=generations.find(id);return it==generations.end()?0:it->second; }
uint32_t current_callsite() {return callsites.empty()?0:callsites.back();}
void after_relocate(uint8_t* rdram,uint32_t header,int32_t delta) {
    auto layout=read_layout(rdram,header);
    auto index=section_index(layout.id);
    const auto& section=sections[index];
    require_lazy(layout.bss-layout.text==section.size, [&]() { return "live image extent differs from compiled section id "+std::to_string(layout.id); });
    require(word(rdram,header+0x34)==layout.text,"original text_start field disagrees with header layout");
    auto it=residents.find(layout.id);
    if (!delta) {
        require_lazy(it==residents.end(), [&]() { return "duplicate first load id "+std::to_string(layout.id); });
        require(section_addresses && section_addresses[section.index]==static_cast<int32_t>(section.ram_addr),"runtime section already resident before first publication");
        for(uint32_t a=layout.bss;a<layout.end;++a)
            require(static_cast<uint8_t>(MEM_B(0,guest(a)))==0,"first-load BSS was not cleared by original loader");
        // No guest bytes are written here. The original packed relocation loop
        // has already handled all four kinds, including R_MIPS_32 data pointers.
        load_overlay_by_id(layout.id,layout.text);
        auto generation=++generations[layout.id];
        auto [published,inserted]=residents.emplace(layout.id,Resident{header,layout.text,generation});
        (void)inserted;
        notify("published",layout.id,published->second);
    } else {
        require(it!=residents.end(),"move has no resident predecessor");
        require(!it->second.active,"moving an overlay with active native frames");
        require(int64_t(it->second.header)+delta==header && int64_t(it->second.text)+delta==layout.text,"move base/delta mismatch");
        require(section_addresses && section_addresses[section.index]==static_cast<int32_t>(it->second.text),"runtime base diverged before move");
        // Pinned runtime's second load_overlay_by_id argument is a DELTA when
        // already resident. Guest defrag/ovl_shift performed copying/relocation.
        load_overlay_by_id(layout.id,static_cast<uint32_t>(delta));
        it->second.header=header;it->second.text=layout.text;it->second.generation=++generations[layout.id];
        notify("moved",layout.id,it->second);
    }
}
void before_free(uint8_t* rdram,uint32_t header) {
    uint32_t id=half(rdram,header+0x2c);
    auto it=residents.find(id);
    require(it!=residents.end() && it->second.header==header,"unload refers to an unknown residency");
    require(!it->second.active,"unloading an overlay with active native frames");
    require((static_cast<uint8_t>(MEM_B(0,guest(header+15)))&4)==0,"protected overlay reached successful unload path");
    notify("unloaded",id,it->second);
    unload_overlay_by_id(id);
    residents.erase(it);
}
void dispatch_syscall(uint8_t* rdram,recomp_context* ctx,uint32_t instruction_vram) {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    auto* persistent_machine=continuation::Machine::current_if_bound();
    if(persistent_machine) {
        const auto resume_pc=persistent_machine->enter(persistent_dispatch_id).pc;
        if(resume_pc) {persistent_finish_dispatch(rdram,ctx);return;}
        persistent_machine->top().operands[0]=instruction_vram;
    }
#endif
    auto request=decode_request(rdram,instruction_vram);
    // Report an uncompiled request before it can enter the original loader.
    // No global successful-import stub or synthesized image is permitted.
    std::size_t index;
    try { index=section_index(request.id); }
    catch(const std::exception& error) {
        throw std::runtime_error(std::string(error.what())+" stub="+hex(instruction_vram)+" raw_ra="+hex(static_cast<uint32_t>(ctx->r31))+
            " observed_callsite="+hex(current_callsite())+
            " resident="+(resident(request.id)?"true":"false")+" generation="+std::to_string(generation(request.id)));
    }
    if (!request.already_patched) {
        require(callbacks.original_syscall_handler,"original syscall handler is not configured");
        require(!residents.contains(request.id),"unpatched stub still references resident overlay");
        notify("request",request.id,Resident{0,0,generation(request.id)},instruction_vram,static_cast<uint32_t>(ctx->r31));
        // This is the original exception dispatcher's EPC -> t0 handoff.
        // Original handler saves arguments, loads/relocates, patches guest
        // instructions and re-enters this same generated 8-byte stub via JR t0.
        ctx->r8=guest(instruction_vram);
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        if(persistent_machine)persistent_machine->top().pc=persistent_loader_return;
        if(persistent_machine)
            persistent_machine->validate_function_pointer(callbacks.original_syscall_handler)(rdram,ctx);
        else
#endif
        callbacks.original_syscall_handler(rdram,ctx);
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        if(persistent_machine)persistent_machine->leave(persistent_dispatch_id);
#endif
        return;
    }
    uint32_t hook=(request.word<<2 & 0x0fffffffu)|(instruction_vram&0xf0000000u);
    uint32_t header=hook-0x10;
    auto found=residents.find(request.id);
    require_lazy(found!=residents.end() && found->second.header==header, [&]() { return "stale or unpublished thunk for id "+std::to_string(request.id); });
    auto layout=read_layout(rdram,header);
    require(request.table_start==syscall_first+layout.syscall_index*8u,"syscall group/header identity mismatch");
    require(request.entry_offset/4<layout.entries,"entry offset exceeds live table");
    const uint32_t delay_offset=request.entry_offset; // Both original ADDI/XORI preserve the offset.
    uint32_t target=word(rdram,layout.entry_table+delay_offset);
    // Once patched, all stubs in the group jump to the installed thunk.
    // Its actual opcode controls behavior even when a different entry's
    // original delay-word flag differs (or flags prevent conditional unload).
    const bool alternate_thunk=(word(rdram,header+0x10)&0xFC000000u)==0x0C000000u;
    if (alternate_thunk) {
        uint32_t jal=word(rdram,header+0x10);
        require((jal&0xFC000000u)==0x0C000000u && word(rdram,header+0x14)==0 &&
                word(rdram,header+0x18)==0x0C02078Cu && word(rdram,header+0x1c)==0 && word(rdram,header+0x24)==0,
                "alternate thunk differs from original template");
        target=((jal&0x03ffffffu)<<2)|(header&0xf0000000u);
    } else {
        require(word(rdram,header+0x10)==(0x3c090000u|(layout.entry_table>>16)) &&
                word(rdram,header+0x14)==(0x35290000u|(layout.entry_table&0xffff)) &&
                word(rdram,header+0x18)==0x01095020u && word(rdram,header+0x1c)==0x8d4b0000u &&
                word(rdram,header+0x20)==0x01600008u && word(rdram,header+0x24)==0xa520fffau,
                "normal thunk differs from original template");
    }
    require(target>=layout.text && target<layout.bss && (target&3)==0,"entry target outside live image");
    auto* function=recomp::overlays::get_func_by_section_index_function_offset(static_cast<uint16_t>(index),target-layout.text);
    require_lazy(function, [&]() { return "missing native entry: id="+std::to_string(request.id)+" section="+std::to_string(index)+" original="+hex(sections[index].ram_addr+target-layout.text)+" live="+hex(target)+" raw_ra="+hex(static_cast<uint32_t>(ctx->r31))+" observed_callsite="+hex(current_callsite())+" generation="+std::to_string(found->second.generation); });
    require(section_addresses[sections[index].index]==static_cast<int32_t>(layout.text),"native section base does not match residency");
    const uint64_t current_generation=found->second.generation;
    const gpr caller_ra=ctx->r31;
    notify("entry",request.id,found->second,target,static_cast<uint32_t>(caller_ra));
    ++found->second.active;
    // Do not retain unordered_map iterators across nested calls/publications.
    try {
        ctx->r8=guest(delay_offset);
        if (alternate_thunk) {
            require((word(rdram,header+0x10)&0xFC000000u)==0x0C000000u,"alternate thunk lacks original JAL");
            ctx->r31=guest(header+0x18);
        } else {
            ctx->r9=guest(layout.entry_table);
            ctx->r10=guest(layout.entry_table+delay_offset);
            ctx->r11=guest(target);
            MEM_H(0,guest(header+0x32))=0; // Original normal thunk delay slot.
        }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        if(persistent_machine) {
            auto& frame=persistent_machine->top();
            frame.pc=persistent_target_return;frame.operands[1]=request.id;
            frame.operands[2]=target;frame.operands[3]=caller_ra;
            frame.hi=current_generation;frame.lo=header;frame.result=alternate_thunk;
            // Section-table overrides must obey the same continuation closure
            // as generated indirect calls. Unknown native code may run, but
            // cannot be checkpointed while it owns unrepresented locals.
            function=persistent_machine->validate_function_pointer(function);
        }
#endif
        function(rdram,ctx);
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        // A successful native return closes an unknown dependency. Rearm the
        // ordinary return stage before the shared finish routine reads it.
        if(persistent_machine)persistent_machine->top().pc=persistent_target_return;
#endif
    } catch (...) {
        --residents.at(request.id).active;
        throw;
    }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if(persistent_machine) {persistent_finish_dispatch(rdram,ctx);return;}
#endif
    auto& live=residents.at(request.id);
    require(live.generation==current_generation,"residency changed during native entry");
    --live.active;
    notify("return",request.id,live,target,static_cast<uint32_t>(caller_ra));
    if (alternate_thunk) {
        require(callbacks.original_alternate_return,"original alternate-return function is not configured");
        // Execute the second heap JAL, then use the actual original wrapper.
        // It owns its stack frame, v0/v1/f0/f2 saves, conditional ovl_unload,
        // and the caller address stored at header+0x20.
        ctx->r31=guest(header+0x20);
        callbacks.original_alternate_return(rdram,ctx);
    }
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
PersistentState persistent_export() {
    PersistentState result;
    for(const auto& [id,live]:residents)result.residents.push_back({id,live.header,live.text,live.active,live.generation});
    for(const auto& [id,value]:generations)result.generations.push_back({id,value});
    std::sort(result.residents.begin(),result.residents.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::sort(result.generations.begin(),result.generations.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    return result;
}
void persistent_validate(uint8_t* rdram,const PersistentState& state) {
    require(state.residents.size()<=sections.size()&&state.generations.size()<=sections.size(),"excess snapshot overlay records");
    std::unordered_map<uint32_t,uint64_t> seen;
    for(const auto& record:state.generations) {
        section_index(record.id);
        require(record.generation&&seen.emplace(record.id,record.generation).second,"duplicate/invalid snapshot generation");
    }
    std::unordered_map<uint32_t,bool> live_ids;
    for(const auto& record:state.residents) {
        const auto index=section_index(record.id);
        const auto layout=read_layout(rdram,record.header);
        require(live_ids.emplace(record.id,true).second&&layout.id==record.id&&layout.text==record.text&&
            layout.bss-layout.text==sections[index].size&&record.active<=1024&&seen.contains(record.id)&&
            seen.at(record.id)==record.generation,"invalid snapshot overlay residency");
    }
}
void persistent_import(uint8_t* rdram,const PersistentState& state) {
    persistent_validate(rdram,state);
    for(const auto& [id,live]:residents)unload_overlay_by_id(id);
    residents.clear();generations.clear();
    for(const auto& record:state.generations)generations.emplace(record.id,record.generation);
    for(const auto& record:state.residents) {
        load_overlay_by_id(record.id,record.text);
        residents.emplace(record.id,Resident{record.header,record.text,record.generation,record.active});
    }
}
std::vector<uint32_t> persistent_callsites(){return callsites;}
void persistent_restore_callsites(std::span<const uint32_t> values) {
    require(values.size()<=1024,"snapshot callsite stack overflow");
    for(auto value:values)require(value>=ram_first&&value<0xa0000000&&value%4==0,"invalid snapshot callsite");
    callsites.assign(values.begin(),values.end());
}
std::span<const continuation::Function> persistent_functions() {
    static constexpr uint32_t pcs[]={persistent_loader_return,persistent_target_return,persistent_alternate_return};
    static const continuation::Function values[]={{persistent_dispatch_id,persistent_dispatch_entry,pcs}};
    return values;
}
#endif
}
extern "C" void tooie_overlay_after_relocate(uint8_t* rdram,uint32_t header,int32_t delta) { tooie::overlays::after_relocate(rdram,header,delta); }
extern "C" void tooie_overlay_before_free(uint8_t* rdram,uint32_t header) { tooie::overlays::before_free(rdram,header); }
extern "C" void tooie_overlay_callsite_push(uint32_t original_callsite_pc) {
    require(original_callsite_pc>=0x80000400 && original_callsite_pc<0xa0000000 && original_callsite_pc%4==0,"invalid observed callsite PC");
    require(callsites.size()<1024,"observational callsite stack exceeds 1024 frames");
    callsites.push_back(original_callsite_pc);
}
extern "C" void tooie_overlay_callsite_pop() {
    require(!callsites.empty(),"observational callsite stack underflow");
    callsites.pop_back();
}
