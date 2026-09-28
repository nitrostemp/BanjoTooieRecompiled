#pragma once
#include "recomp.h"
#include <array>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Synchronous const snapshots only. No guest calls, retained guest pointers,
// input injection, transition changes, or title/gameplay acceptance claims.
namespace tooie::title_observation {
inline constexpr std::array<uint32_t,9> sites={0x80800614,0x80800644,0x80800654,0x80800664,0x8080066c,0x808007ac,0x808002c4,0x808006c4,0x80800050};
inline constexpr std::array<const char*,9> names={"title_ticker_state","title_start_flag_return","title_pending_warp_return","title_start_compare","title_start_transition_reached","title_frontend_map_request","title_current_map_stored","title_ticker_elapsed_stored","attract_clock_stored"};
inline constexpr std::array<uint64_t,9> limits={256,256,256,256,256,256,256,128,128};
inline bool valid_range(const uint8_t* ram,size_t size,uint32_t a,size_t n) noexcept {
    const auto segment=a&0xe0000000u;
    return ram && size==0x800000 && (segment==0x80000000 || segment==0xa0000000) &&
        uint64_t(a&0x1fffffffu)+n<=size;
}
inline bool read(const uint8_t* ram,size_t size,uint32_t a,unsigned n,uint32_t& out) noexcept {
    if((n!=1 && n!=2 && n!=4) || (a&(n-1)) || !valid_range(ram,size,a,n))return false;
    out=0;for(unsigned i=0;i<n;++i)out=(out<<8)|ram[((a&0x1fffffffu)+i)^3];return true;
}
struct Record {
    uint32_t site=0,map=0,context_map=0,actor=0,mode=0,pending=0,start=0,b=0,ready=0,blocked=0;
    uint32_t compare_rhs=0,requested_map=0,requested_exit=0,requested_transition=0,map_state_address=0,previous_map=0,stored_map=0;
    int32_t result=0;
    bool state_valid=false,actor_valid=false,global_map_target=false,store_valid=false;
    unsigned category=0;uint64_t category_sequence=0;
    auto key() const noexcept {return std::array<uint32_t,23>{map,context_map,actor,mode,pending,start,b,ready,blocked,
        compare_rhs,requested_map,requested_exit,requested_transition,map_state_address,previous_map,stored_map,
        uint32_t(result),uint32_t(state_valid),uint32_t(actor_valid),uint32_t(global_map_target),uint32_t(store_valid),site,category};}
};
struct Observer {
    std::array<std::atomic<uint64_t>,9> seen{},emitted{},suppressed{},log_attempts{};
    // The two per-frame timing sites retain only their latest native snapshot.
    // They never take the title trace mutex or serialize a per-frame trace.
    std::atomic<uint32_t> title_actor{},title_elapsed_bits{},title_scheduler_delta_bits{},title_scheduler_ticks_bits{};
    std::atomic<uint32_t> attract_phase{},attract_clock_bits{},attract_scheduler_delta_bits{},attract_scheduler_ticks_bits{};
    std::atomic<uint64_t> title_first_host_ns{},title_latest_host_ns{},attract_first_host_ns{},attract_latest_host_ns{};
    std::atomic<bool> title_timing_valid{false},attract_timing_valid{false};
    std::atomic<uint64_t> failures{};
    std::mutex filter_mutex;
    std::array<std::array<uint32_t,23>,9> last{};std::array<bool,9> have_last{};
    void observe_timing(const uint8_t* ram,size_t size,const recomp_context* ctx,unsigned category) noexcept {
        if(!ctx){++failures;return;}
        ++seen[category];
        uint32_t a=0,b=0,c=0,d=0;bool valid=false;
        const auto host_ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if(category==7){
            const auto actor=uint32_t(ctx->r19);
            valid=!(actor&3) && valid_range(ram,size,actor,0x30) && read(ram,size,actor+0x2c,4,a);
            valid=read(ram,size,0x8012c760,4,b) && valid;
            valid=read(ram,size,0x8012c764,4,c) && valid;
            title_actor.store(actor,std::memory_order_relaxed);
            title_elapsed_bits.store(a,std::memory_order_relaxed);
            title_scheduler_delta_bits.store(b,std::memory_order_relaxed);
            title_scheduler_ticks_bits.store(c,std::memory_order_relaxed);
            uint64_t zero=0;title_first_host_ns.compare_exchange_strong(zero,host_ns,std::memory_order_relaxed);
            title_latest_host_ns.store(host_ns,std::memory_order_relaxed);
            title_timing_valid.store(valid,std::memory_order_release);
        }else{
            valid=read(ram,size,0x80135470,1,a);
            valid=read(ram,size,0x80135474,4,b) && valid;
            valid=read(ram,size,0x8012c760,4,c) && valid;
            valid=read(ram,size,0x8012c764,4,d) && valid;
            attract_phase.store(a,std::memory_order_relaxed);
            attract_clock_bits.store(b,std::memory_order_relaxed);
            attract_scheduler_delta_bits.store(c,std::memory_order_relaxed);
            attract_scheduler_ticks_bits.store(d,std::memory_order_relaxed);
            uint64_t zero=0;attract_first_host_ns.compare_exchange_strong(zero,host_ns,std::memory_order_relaxed);
            attract_latest_host_ns.store(host_ns,std::memory_order_relaxed);
            attract_timing_valid.store(valid,std::memory_order_release);
        }
        if(!valid)++failures;
    }
    template<class Sink> void observe(const uint8_t* ram,size_t size,const recomp_context* ctx,uint32_t site,Sink&& sink) noexcept {
        try {
            if(!ctx){++failures;return;}
            unsigned category=0;while(category<sites.size() && sites[category]!=site)++category;
            if(category==sites.size()){++failures;return;}
            if(category>=7){observe_timing(ram,size,ctx,category);return;}
            Record r;r.site=site;r.category=category;r.category_sequence=++seen[category];
            bool valid=true;
            if(category<=6){
                r.state_valid=read(ram,size,0x80132dc2,2,r.map);
                r.state_valid=read(ram,size,0x80127642,1,r.pending) && r.state_valid;
                r.state_valid=read(ram,size,0x80079af4,4,r.start) && r.state_valid;
                r.state_valid=read(ram,size,0x800799e4,4,r.b) && r.state_valid;
                r.state_valid=read(ram,size,0x80079d69,1,r.ready) && r.state_valid;
                r.state_valid=read(ram,size,0x80079b93,1,r.blocked) && r.state_valid;
                valid=r.state_valid;
            }
            if(category<=4){
                r.actor=uint32_t(ctx->r19);r.context_map=uint32_t(ctx->r18);
                r.actor_valid=!(r.actor&3) && valid_range(ram,size,r.actor,0x7a) && read(ram,size,r.actor+0x79,1,r.mode);
                r.mode>>=4;valid=valid && r.actor_valid;
                if(category>=1 && category<=3)r.result=int32_t(ctx->r2);
                if(category==3)r.compare_rhs=uint32_t(ctx->r16);
            }else if(category==5){r.requested_map=uint32_t(ctx->r4);r.requested_exit=uint32_t(ctx->r5);r.requested_transition=uint32_t(ctx->r6);}
            else if(category==6){
                r.map_state_address=uint32_t(ctx->r4);r.requested_map=uint32_t(ctx->r5);r.previous_map=uint32_t(ctx->r15)&0xffff;
                r.store_valid=!(r.map_state_address&1) && valid_range(ram,size,r.map_state_address,4) && read(ram,size,r.map_state_address+2,2,r.stored_map);
                r.global_map_target=r.store_valid && (r.map_state_address&0x1fffffff)==0x132dc0;
                valid=valid && r.store_valid;
            }
            if(!valid)++failures;
            {
                std::lock_guard<std::mutex> lock(filter_mutex);
                const auto key=r.key();const bool changed=!have_last[category] || last[category]!=key;
                last[category]=key;have_last[category]=true;
                if(log_attempts[category]>=limits[category] || (category<4 && r.category_sequence>8 && !changed)){++suppressed[category];return;}
                ++log_attempts[category];
            }
            sink(r);++emitted[category];
        }catch(...){++failures;} // Diagnostic/logging failure never changes guest control flow.
    }
};
} // namespace tooie::title_observation
