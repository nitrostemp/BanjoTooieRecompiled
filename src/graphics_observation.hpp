#pragma once
#include "recomp.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

// Read-only snapshots on the original guest worker. No retained guest pointers,
// queue changes, parser calls, completion signals, or task-success assertions.
namespace tooie::graphics_observation {
inline constexpr std::array<uint32_t,16> state_addresses={
    0x8007847c,0x80078480,0x80078484,0x80078488,
    0x8007848c,0x80078490,0x80078494,0x80078ea8,
    0x80078eac,0x80078f00,0x80078f04,0x80078f08,
    0x80078f0c,0x80078f10,0x80078f14,0x80078478};
inline constexpr std::array<const char*,6> categories={
    "receive_other","receive_vi","receive_sp","receive_dp","submit_audio","submit_graphics"};
inline constexpr std::array<uint64_t,6> limits={32,8,128,128,64,64};
struct Record {
    uint32_t site=0,message=0,task_address=0;
    int32_t result=0;
    bool receive_return=false,receive_success=false,message_valid=false;
    bool task_valid=false,state_valid=false;
    unsigned category=0;
    uint64_t category_sequence=0;
    std::array<uint32_t,16> state{},task{};
};
inline bool read_words(const uint8_t* ram,uint32_t address,uint32_t* out,size_t count) noexcept {
    const uint32_t segment=address&0xe0000000u,physical=address&0x1fffffffu;
    if(!ram || (segment!=0x80000000u && segment!=0xa0000000u) || (address&3) ||
       uint64_t(physical)+uint64_t(count)*4>0x800000ull)return false;
    // Native RDRAM is word-swapped; aligned words have host byte order. memcpy
    // avoids alignment/aliasing UB, and the caller owns the live guest boundary.
    std::memcpy(out,ram+physical,count*4);return true;
}
struct Observer {
    std::array<std::atomic<uint64_t>,6> seen{},suppressed{},emitted{};
    std::atomic<uint64_t> failures{0};
    template<class Sink> void observe(const uint8_t* ram,const recomp_context* ctx,uint32_t site,Sink&& sink) noexcept {
        try {
            if(!ctx){++failures;return;}
            Record r;r.site=site;
            if(site==0x80014aac) {
                r.receive_return=true;r.result=int32_t(ctx->r2);r.receive_success=r.result==0;
                if(r.receive_success) {
                    const uint64_t output=uint64_t(uint32_t(ctx->r29))+0x1c;
                    r.message_valid=output<=0xffffffffull && read_words(ram,uint32_t(output),&r.message,1);
                    if(!r.message_valid)++failures;
                }
                if(r.message_valid)r.category=r.message==5?1:r.message==6?2:r.message==4?3:0;
            } else if(site==0x80013ecc || site==0x80013f7c || site==0x80014070 || site==0x800147a8) {
                r.category=site==0x80013ecc?4:5;
                r.task_address=uint32_t(ctx->r4);
            } else {++failures;return;}
            r.category_sequence=++seen[r.category];
            if(r.category_sequence>limits[r.category]){++suppressed[r.category];return;}
            if(!r.receive_return) {
                r.task_valid=read_words(ram,r.task_address,r.task.data(),r.task.size());
                if(!r.task_valid)++failures;
            }
            r.state_valid=true;
            for(size_t i=0;i<state_addresses.size();++i)
                r.state_valid=read_words(ram,state_addresses[i],&r.state[i],1) && r.state_valid;
            if(!r.state_valid)++failures;
            sink(r);++emitted[r.category];
        }catch(...){++failures;} // An observer/logging error cannot alter scheduling.
    }
};
} // namespace tooie::graphics_observation
