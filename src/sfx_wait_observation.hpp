#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include "recomp.h"

namespace tooie::sfx_wait {
inline constexpr uint32_t entry_pc=0x800c2ab8, outer_pc=0x800c2af4, after_pc=0x800fb968;
inline constexpr uint32_t slots_base=0x80128c10, handles_base=0x80128b08;
inline constexpr size_t slot_count=59, stride=0x80, cap=512;
struct Slot {
    uint32_t index{},address{},flags{},first_byte{},state{},handle{},target_address{},target_word{};
    bool unresolved{};
};
struct Snapshot {
    bool complete{};
    uint32_t pc{},unresolved{},s1{},s2{},s3{},sp{},ra{};
    uint64_t outer_pass{};
    std::array<Slot,slot_count> slots{};
};
// These are native little-endian, word-swizzled RDRAM observations. Nothing is
// supplied to guest memory, and no guest function, message or scheduler runs.
inline Snapshot capture(const uint8_t* rdram,size_t size,const recomp_context& ctx,uint32_t pc,uint64_t outer_pass) noexcept {
    static_assert(std::endian::native==std::endian::little);
    Snapshot result;result.pc=pc;result.outer_pass=outer_pass;
    result.s1=uint32_t(ctx.r17);result.s2=uint32_t(ctx.r18);result.s3=uint32_t(ctx.r19);
    result.sp=uint32_t(ctx.r29);result.ra=uint32_t(ctx.r31);
    constexpr size_t end=(slots_base&0x1fffffff)+(slot_count-1)*stride+0x78+4;
    constexpr size_t handle_end=(handles_base&0x1fffffff)+255*8+4;
    if(!rdram || size<end || size<handle_end)return result;
    auto word=[&](uint32_t address){uint32_t value;std::memcpy(&value,rdram+(address&0x1fffffff),4);return value;};
    auto byte=[&](uint32_t address){return rdram[(address&0x1fffffff)^3];};
    for(size_t i=0;i<slot_count;++i) {
        auto& slot=result.slots[i];slot.index=uint32_t(i+1);slot.address=slots_base+uint32_t(i*stride);
        slot.flags=word(slot.address+0x78);slot.first_byte=byte(slot.address+0x78);
        slot.state=(slot.flags>>10)&7;slot.handle=byte(slot.address+0x79);
        slot.target_address=handles_base+8*slot.handle;slot.target_word=word(slot.target_address);
        slot.unresolved=slot.first_byte!=0 && slot.state==3 && slot.handle!=0 && slot.target_word!=0;
        result.unresolved+=slot.unresolved;
    }
    result.complete=true;return result;
}
// Instantiate Worker as thread_local at the existing cooperative poll call.
// Counters is shared; the fixed total cap is atomic across all native workers.
struct Worker {bool active{};uint64_t outer_pass{},last_outer_ns{};};
struct Counters {
    std::atomic<uint64_t> attempts{},emitted{},capped{},invalid{},clock_checks{},rate_skipped{};
};
template<class Clock>
std::optional<Snapshot> observe(bool enabled,uint32_t pc,const uint8_t* rdram,size_t size,
        const recomp_context& ctx,Worker& worker,Counters& counters,Clock&& clock) {
    if(!enabled || (pc!=entry_pc && pc!=outer_pc && pc!=after_pc))return std::nullopt;
    if(pc==entry_pc) {worker.active=true;worker.outer_pass=0;}
    else if(!worker.active)return std::nullopt;
    if(pc==outer_pc) {
        ++worker.outer_pass;
        if(worker.outer_pass!=1 && worker.outer_pass%4096!=0)return std::nullopt;
        counters.clock_checks.fetch_add(1,std::memory_order_relaxed);
        const uint64_t now=clock();
        if(worker.outer_pass!=1 && (now<worker.last_outer_ns || now-worker.last_outer_ns<1000000000)) {
            counters.rate_skipped.fetch_add(1,std::memory_order_relaxed);return std::nullopt;
        }
        worker.last_outer_ns=now;
    }
    if(pc==after_pc)worker.active=false;
    counters.attempts.fetch_add(1,std::memory_order_relaxed);
    uint64_t current=counters.emitted.load(std::memory_order_relaxed);
    do {
        if(current>=cap) {counters.capped.fetch_add(1,std::memory_order_relaxed);return std::nullopt;}
    } while(!counters.emitted.compare_exchange_weak(current,current+1,std::memory_order_relaxed));
    auto snapshot=capture(rdram,size,ctx,pc,worker.outer_pass);
    if(!snapshot.complete)counters.invalid.fetch_add(1,std::memory_order_relaxed);
    return snapshot;
}
}
