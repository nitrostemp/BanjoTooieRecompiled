#pragma once
#include "recomp.h"
#include "librecomp/sections.h"
#include <cstdint>
#include <span>
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "persistent_state_continuation.hpp"
#include <vector>
#endif

namespace tooie::overlays {
struct Event {
    const char* operation;
    uint32_t id,header,text,entry,caller;
    uint64_t generation;
};
struct Callbacks {
    recomp_func_t* original_syscall_handler=nullptr;
    recomp_func_t* original_alternate_return=nullptr; // func_80081E30
    void (*event)(const Event&)=nullptr;
};
struct Layout {
    uint32_t header,entry_table,packed_relocs,secondary_relocs,text,bss,end;
    uint16_t entries,packed_count,secondary_count,id,syscall_index;
    uint8_t flags;
};
struct Request {
    uint32_t stub,word,entry_offset,table_start,id,load_flag;
    bool alternate,already_patched;
};
// Pass the same stable-ID tables registered with librecomp. Called before game
// execution; all subsequent mutations belong to the single-running-guest path.
void configure(std::span<const SectionTableEntry>,std::span<const int>,Callbacks);
Layout read_layout(uint8_t* rdram,uint32_t header);
Request decode_request(uint8_t* rdram,uint32_t stub);
void dispatch_syscall(uint8_t*,recomp_context*,uint32_t instruction_vram);
// Called only AFTER original func_80082088 completes, retaining its incoming
// a0/a1 as local hook values. delta==0 is a first load; otherwise a guest move.
void after_relocate(uint8_t*,uint32_t header,int32_t delta);
// Called on ovl_unload's successful path immediately before original heap_free.
// Flags and loaded-list maintenance remain owned by original guest code.
void before_free(uint8_t*,uint32_t header);
bool resident(uint32_t id);
uint64_t generation(uint32_t id);
// Observation only: original callsite PC, independent of the recomp_context RA
// convention. A missing callsite hook is reported as zero.
uint32_t current_callsite();
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
struct PersistentResidency { uint32_t id,header,text,active;uint64_t generation; };
struct PersistentGeneration { uint32_t id;uint64_t generation; };
struct PersistentState {
    std::vector<PersistentResidency> residents;
    std::vector<PersistentGeneration> generations;
};
// Caller must own the all-guest/device freeze. Import exceptions are fatal for
// that parked transaction; never release partially restored lookup mappings.
PersistentState persistent_export();
void persistent_validate(uint8_t* snapshot_rdram,const PersistentState&);
void persistent_import(uint8_t* restored_rdram,const PersistentState&);
std::vector<uint32_t> persistent_callsites();
void persistent_restore_callsites(std::span<const uint32_t>);
std::span<const continuation::Function> persistent_functions();
#endif
}
extern "C" void tooie_overlay_after_relocate(uint8_t*,uint32_t header,int32_t delta);
extern "C" void tooie_overlay_before_free(uint8_t*,uint32_t header);
extern "C" void tooie_overlay_callsite_push(uint32_t original_callsite_pc);
extern "C" void tooie_overlay_callsite_pop();
