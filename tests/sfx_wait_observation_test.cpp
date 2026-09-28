#include "sfx_wait_observation.hpp"
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>
using namespace tooie::sfx_wait;
int main() {
    std::vector<uint8_t> ram(8*1024*1024,0);recomp_context ctx{};
    ctx.r17=0x80128c10;ctx.r18=1;ctx.r19=7;ctx.r29=0x80045600;ctx.r31=0x800329a8;
    auto put=[&](uint32_t address,uint32_t value){std::memcpy(ram.data()+(address&0x1fffffff),&value,4);};
    // Word/byte views differ in swizzled RDRAM; state lives in bits10..12.
    put(0x80128c88,0x01020c00);put(0x80128b18,0x80301234);
    const auto before=ram;const auto context_before=ctx;
    auto result=capture(ram.data(),ram.size(),ctx,0x800c2ab8,0);
    assert(result.complete && result.slots.size()==59 && result.unresolved==1);
    assert(result.slots[0].index==1 && result.slots[0].address==0x80128c10);
    assert(result.slots[0].flags==0x01020c00 && result.slots[0].state==3 && result.slots[0].first_byte==1 && result.slots[0].handle==2);
    assert(result.slots[0].target_address==0x80128b18 && result.slots[0].target_word==0x80301234 && result.slots[0].unresolved);
    assert(result.slots[58].index==59 && result.slots[58].address==0x8012a910);
    assert(ram==before && std::memcmp(&ctx,&context_before,sizeof(ctx))==0);
    // Full predicate truth table, including nonzero firstbyte and max handle.
    for(unsigned first:{0u,1u})for(unsigned state=0;state<8;++state)for(unsigned handle:{0u,2u,255u})for(unsigned target:{0u,1u}) {
        std::fill(ram.begin(),ram.end(),0);put(0x80128c88,(first<<24)|(handle<<16)|(state<<10));
        put(0x80128b08+8*handle,target);
        auto s=capture(ram.data(),ram.size(),ctx,0x800c2af4,1).slots[0];
        assert(s.unresolved==(first && state==3 && handle && target));
    }
    assert(!capture(nullptr,ram.size(),ctx,0x800c2ab8,0).complete);
    assert(!capture(ram.data(),0x12a98b,ctx,0x800c2ab8,0).complete);
    assert(capture(ram.data(),0x12a98c,ctx,0x800c2ab8,0).complete);
    const auto before_sampling=ram;const auto ctx_before_sampling=ctx;
    Worker worker;Counters stats;uint64_t now=10,clock_calls=0;
    auto clock=[&]{++clock_calls;return now;};
    assert(!observe(false,0x800c2ab8,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(!worker.active && clock_calls==0 && stats.attempts==0);
    assert(!observe(true,0x12345678,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(observe(true,0x800c2ab8,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(observe(true,0x800c2af4,ram.data(),ram.size(),ctx,worker,stats,clock));
    auto clock_at_first=clock_calls;
    for(unsigned i=2;i<4096;++i)assert(!observe(true,0x800c2af4,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(clock_calls==clock_at_first);
    assert(!observe(true,0x800c2af4,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(clock_calls==clock_at_first+1 && stats.rate_skipped==1);
    now+=1000000000;
    for(unsigned i=4097;i<8192;++i)assert(!observe(true,0x800c2af4,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(observe(true,0x800c2af4,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(observe(true,0x800fb968,ram.data(),ram.size(),ctx,worker,stats,clock));
    assert(!worker.active && !observe(true,0x800fb968,ram.data(),ram.size(),ctx,worker,stats,clock));
    // Shared budget remains512 across threads; each worker owns its state.
    Counters shared;
    auto work=[&]{Worker w;for(unsigned i=0;i<400;++i)observe(true,0x800c2ab8,ram.data(),ram.size(),ctx,w,shared,[]{return uint64_t(1);});};
    std::thread a(work),b(work);a.join();b.join();
    assert(shared.emitted==512 && shared.attempts==800 && shared.capped==288);
    assert(ram==before_sampling && std::memcmp(&ctx,&ctx_before_sampling,sizeof(ctx))==0);
    auto memory_before=ram;auto ctx_before=ctx;
    Worker invalid;Counters invalid_stats;
    auto bad=observe(true,0x800c2ab8,nullptr,0,ctx,invalid,invalid_stats,clock);
    assert(bad && !bad->complete && invalid_stats.invalid==1);
    assert(ram==memory_before && std::memcmp(&ctx,&ctx_before,sizeof(ctx))==0);
}
