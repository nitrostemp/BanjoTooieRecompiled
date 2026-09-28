#pragma once
#include "recomp.h"
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Original chgameselect (205) boundaries. Synchronous const snapshots only:
// no guest calls, input, pointer traversal, writes, or acceptance decisions.
namespace tooie::menu_observation {
inline constexpr std::array<uint32_t,11> sites={0x80800d4c,0x80800eb0,0x8080140c,0x80801000,0x80801014,0x80801048,0x808006a0,0x8080074c,0x8080085c,0x8080086c,0x808008b0};
inline constexpr std::array<const char*,11> names={"menu_parent_resolved","menu_stick_return","menu_selection_committed","menu_buttons_gate","menu_b_counter","menu_a_counter","menu_a_dispatch","menu_a_any_save_result","menu_a_completed","menu_b_dispatch","menu_b_camera_returned"};
inline constexpr std::array<const char*,11> functions={"func_80800D3C_chgameselect","func_80800E24_chgameselect","func_808013C8_chgameselect","func_80800E24_chgameselect","func_80800E24_chgameselect","func_80800E24_chgameselect","func_808006A0_chgameselect","func_808006A0_chgameselect","func_808006A0_chgameselect","func_8080086C_chgameselect","func_8080086C_chgameselect"};
inline constexpr std::array<unsigned,11> limits={128,128,32,128,128,128,32,32,32,32,32};
inline constexpr bool recurring(unsigned category) noexcept {return category==0 || category==1 || (category>=3 && category<=5);}
inline bool range(const uint8_t* ram,size_t size,uint64_t a,size_t n,unsigned alignment) noexcept {
    if(!ram || size!=0x800000 || a>UINT32_MAX || (a&(alignment-1)))return false;
    const auto segment=uint32_t(a)&0xe0000000u;
    return (segment==0x80000000u || segment==0xa0000000u) && (a&0x1fffffffu)+n<=size;
}
inline bool read(const uint8_t* ram,size_t size,uint64_t a,unsigned n,uint32_t& out) noexcept {
    if((n!=1 && n!=2 && n!=4) || !range(ram,size,a,n,n))return false;
    out=0;for(unsigned i=0;i<n;++i)out=(out<<8)|ram[((a&0x1fffffffu)+i)^3];return true;
}
inline bool finite(uint32_t bits) noexcept {return std::isfinite(std::bit_cast<float>(bits));}
struct Actor {
    uint32_t address=0,marker=0,state=0,selection=0,parent_marker=0,flags=0,timer_bits=0;
    bool present=false,valid=false,child=false,timer_finite=false;
};
struct Slot {uint32_t address=0,value=0;bool valid=false;};
struct Record {
    unsigned category=0;uint64_t category_sequence=0;
    uint32_t site=0,map=0,pending=0,x_bits=0,y_bits=0,old_selection=0,event_selection=0;
    int32_t result=0,message=0;
    bool globals_valid=false,stack_valid=true,stick_present=false,stick_valid=false,x_finite=false,y_finite=false;
    bool slots_present=false,slots_valid=false;
    Actor actor;std::array<Slot,3> slots{};
    bool valid() const noexcept {
        return globals_valid && stack_valid && (!actor.present || actor.valid) &&
            (!actor.child || actor.timer_finite) && (!slots_present || slots_valid) &&
            (!stick_present || (stick_valid && x_finite && y_finite));
    }
    auto key() const noexcept {
        // Timer ticks and unrelated actor flags cannot exhaust recurring caps.
        // Stored numeric addresses/markers are values only, never later dereferenced.
        return std::array<uint32_t,40>{map,pending,actor.address,actor.marker,actor.state,actor.selection,
            actor.parent_marker,actor.flags&((1u<<19)|(1u<<20)),uint32_t(actor.present),uint32_t(actor.valid),
            uint32_t(actor.child),uint32_t(actor.timer_finite),x_bits,y_bits,old_selection,event_selection,
            uint32_t(result),uint32_t(message),uint32_t(globals_valid),uint32_t(stack_valid),uint32_t(stick_present),
            uint32_t(stick_valid),uint32_t(x_finite),uint32_t(y_finite),uint32_t(slots_present),uint32_t(slots_valid),
            slots[0].address,slots[0].value,uint32_t(slots[0].valid),slots[1].address,slots[1].value,uint32_t(slots[1].valid),
            slots[2].address,slots[2].value,uint32_t(slots[2].valid)};
    }
};
inline void actor_snapshot(Record& r,const uint8_t* ram,size_t size,uint32_t address,bool child) noexcept {
    auto& a=r.actor;a.present=true;a.child=child;a.address=address;
    if(!range(ram,size,address,0x7a,4))return;
    a.valid=read(ram,size,address,4,a.marker) && read(ram,size,uint64_t(address)+0x72,2,a.state) && read(ram,size,uint64_t(address)+0x79,1,a.selection);
    a.state>>=10;a.selection>>=4;
    if(child){
        a.valid=read(ram,size,uint64_t(address)+0x3c,4,a.parent_marker) && read(ram,size,uint64_t(address)+0x64,4,a.flags) && read(ram,size,uint64_t(address)+0x58,4,a.timer_bits) && a.valid;
        a.timer_finite=a.valid && finite(a.timer_bits);
    }
}
inline void slot_snapshot(Record& r,const uint8_t* ram,size_t size) noexcept {
    r.slots_present=true;r.slots_valid=true;
    for(unsigned i=0;i<3;++i){auto& s=r.slots[i];s.valid=read(ram,size,0x80127618+i*4,4,s.address) && read(ram,size,s.address,1,s.value);r.slots_valid=r.slots_valid && s.valid;}
}
struct Observer {
    std::array<std::atomic<uint64_t>,11> seen{},attempts{},emitted{},filtered{},capped{},invalid{};
    std::atomic<uint64_t> failures{0};
    std::mutex filter_mutex;std::array<std::array<uint32_t,40>,11> last{};std::array<bool,11> have_last{};
    template<class Sink> void observe(const uint8_t* ram,size_t size,const recomp_context* ctx,uint32_t site,Sink&& sink) noexcept {
        try {
            if(!ctx){++failures;return;}
            unsigned category=0;while(category<sites.size() && sites[category]!=site)++category;
            if(category==sites.size()){++failures;return;}
            Record r;r.site=site;r.category=category;r.category_sequence=++seen[category];
            r.globals_valid=read(ram,size,0x80132dc2,2,r.map);
            r.globals_valid=read(ram,size,0x80127642,1,r.pending) && r.globals_valid;
            const uint64_t sp=uint32_t(ctx->r29);
            if(category==0)actor_snapshot(r,ram,size,uint32_t(ctx->r2),false);
            else if(category==1 || (category>=3 && category<=5))actor_snapshot(r,ram,size,uint32_t(ctx->r16),true);
            else if(category==2){actor_snapshot(r,ram,size,uint32_t(ctx->r6),true);r.old_selection=(uint32_t(ctx->r10)>>4)&15;}
            else if(category==6 || category==9){actor_snapshot(r,ram,size,uint32_t(ctx->r4),false);r.event_selection=uint32_t(ctx->r5);}
            else if(category==7 || category==8){
                uint32_t address=0;r.stack_valid=read(ram,size,sp+0x30,4,address);
                actor_snapshot(r,ram,size,address,false);
                if(category==8){uint32_t message=0;r.stack_valid=read(ram,size,sp+0x2c,4,message) && r.stack_valid;r.message=int32_t(message);}
            }
            // Category10 has no surviving root register: do not guess one.
            if(r.actor.present && !r.actor.child)slot_snapshot(r,ram,size);
            if(category==1){
                r.stick_present=true;r.stack_valid=read(ram,size,sp+0x28,4,r.x_bits);
                r.stack_valid=read(ram,size,sp+0x2c,4,r.y_bits) && r.stack_valid;
                r.stick_valid=r.stack_valid;r.x_finite=r.stick_valid && finite(r.x_bits);r.y_finite=r.stick_valid && finite(r.y_bits);
            }
            if((category>=3 && category<=5) || category==7)r.result=int32_t(ctx->r2);
            if(!r.valid())++invalid[category];
            {
                std::lock_guard lock(filter_mutex);
                const auto key=r.key();const bool changed=!have_last[category] || last[category]!=key;
                last[category]=key;have_last[category]=true;
                if(attempts[category]>=limits[category]){++capped[category];return;}
                if(recurring(category) && r.category_sequence>8 && !changed){++filtered[category];return;}
                ++attempts[category];
            }
            sink(r);++emitted[category];
        }catch(...){++failures;} // Diagnostic failures cannot escape into guest execution.
    }
};
} // namespace tooie::menu_observation
