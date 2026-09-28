// Registration structure follows BanjoRecomp/src/main/register_overlays.cpp.
#include "game.hpp"
#include "core1_bridge.hpp"
#include "core1_metadata.hpp"
#include "tooie_overlays.hpp"
#include "overlay_call_trace.hpp"
#include "recomp_overlays.inl"
#include "librecomp/overlays.hpp"
#include <algorithm>
#include <stdexcept>
extern "C" void load_overlay_by_id(uint32_t,uint32_t);
extern "C" void unload_overlay_by_id(uint32_t);
extern "C" void syscall_handler(uint8_t*,recomp_context*);
extern "C" void func_80081E30(uint8_t*,recomp_context*);
namespace tooie {
static bool boot_retired=false;
void retire_boot_mappings() {
    if(boot_retired)throw std::logic_error("Boot mappings already retired");
    const auto& boot=section_table[0];
    if(boot.ram_addr!=0x80000400 || boot.rom_addr!=0x1000)
        throw std::logic_error("Unexpected boot section identity");
    unload_overlays((int32_t)boot.ram_addr,boot.size);
    boot_retired=true;
    trace("boot_mappings_retired","G1","pass",0,nullptr,
        {{"base",hex32(boot.ram_addr)},{"bytes",boot.size},{"mappings_removed",boot.num_funcs},
         {"boot_native_frames_returned",true},{"first_guest_worker_body_started",false}});
}
static size_t index_for(uint32_t id) {
    if(id==0 || id>=ARRLEN(overlay_sections_by_index) || overlay_sections_by_index[id]<0)
        throw std::invalid_argument("Invalid, empty, or uncompiled overlay ID "+std::to_string(id));
    return (size_t)overlay_sections_by_index[id];
}
void register_overlays() {
    // N64Recomp omits explicitly ignored native imports from its table. Preserve
    // the original initializer address using a project-owned augmented table.
    // The original generated table and every other entry remain unchanged.
    static std::vector<FuncEntry> core1_functions=[] {
        std::vector<FuncEntry> entries(std::begin(section_1_core1_funcs),std::end(section_1_core1_funcs));
        uint32_t offset=core1_meta::os_initialize-core1_meta::core1_entry;
        if(std::any_of(entries.begin(),entries.end(),[offset](auto&e){return e.offset==offset;}))
            throw std::runtime_error("Core1 initializer unexpectedly already registered");
        entries.push_back({.func=tooie_core1_osInitialize,.offset=offset,.rom_size=core1_meta::os_initialize_size});
        std::sort(entries.begin(),entries.end(),[](auto&a,auto&b){return a.offset<b.offset;});
        return entries;
    }();
    section_table[1].funcs=core1_functions.data();
    section_table[1].num_funcs=core1_functions.size();
    // Runtime sorts by ROM; require the generator's original ID indexes already agree.
    if(!std::is_sorted(std::begin(section_table),std::end(section_table),[](auto&a,auto&b){return a.rom_addr<b.rom_addr;}))
        throw std::runtime_error("Generated section order differs from runtime ROM sort");
    if(ARRLEN(overlay_sections_by_index)!=885 || index_for(350)>=ARRLEN(section_table) || section_table[index_for(350)].rom_addr!=0x0200E3F0)
        throw std::runtime_error("Original overlay IDs were not preserved");
    recomp::overlays::register_overlays({section_table,ARRLEN(section_table),num_sections},{overlay_sections_by_index,ARRLEN(overlay_sections_by_index)});
    overlays::configure(section_table,overlay_sections_by_index,{syscall_handler,func_80081E30,observe_overlay_lifecycle});
}
size_t registered_function_count() {size_t n=0;for(auto&s:section_table)n+=s.num_funcs;return n;}
size_t registered_section_index(uint32_t id) {return section_table[index_for(id)].index;}
Json registry_snapshot() {
    Json rows=Json::array();
    for(auto&s:section_table)rows.push_back({{"index",s.index},{"rom",hex32(s.rom_addr)},{"canonical_base",hex32(s.ram_addr)},{"runtime_base",section_addresses?Json(hex32(section_addresses[s.index])):Json(nullptr)},{"bytes",s.size},{"functions",s.num_funcs}});
    const auto fixture=index_for(350);
    return {{"sections",rows},{"boot_mappings_retired",boot_retired},{"overlay_350_section_index",fixture},{"overlay_350_resident",section_addresses && section_addresses[section_table[fixture].index]!=(int32_t)0x80800000},{"thread_lifecycle","see mode-specific thread gate events; registry snapshot does not imply scheduler fidelity"}};
}
static void valid_text_base(uint32_t base,const SectionTableEntry& s) {
    if(base<0x80000400 || (base&3) || (uint64_t)base+s.size>0x80800000)
        throw std::invalid_argument("Overlay text must fit guest RDRAM");
}
void checked_load_overlay(uint32_t id,uint32_t text_base) {
    const auto& s=section_table[index_for(id)]; valid_text_base(text_base,s);
    if(!section_addresses || section_addresses[s.index]!=(int32_t)s.ram_addr)
        throw std::logic_error("Absolute load requires initialized, unloaded section");
    load_overlay_by_id(id,text_base);
}
void checked_move_overlay(uint32_t id,int32_t delta) {
    const auto& s=section_table[index_for(id)];
    if(!section_addresses || section_addresses[s.index]==(int32_t)s.ram_addr)throw std::logic_error("Move requires resident section");
    int64_t next=(uint32_t)section_addresses[s.index]+(int64_t)delta;
    if(next<0 || next>UINT32_MAX)throw std::invalid_argument("Move overflow");
    valid_text_base((uint32_t)next,s);
    load_overlay_by_id(id,(uint32_t)delta);
}
void checked_unload_overlay(uint32_t id) {index_for(id);unload_overlay_by_id(id);}
}
