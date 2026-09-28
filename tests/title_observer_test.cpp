// Synthetic observer contracts; no game acceptance claim.
#include "title_observation.hpp"
#include "boot_hooks.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <type_traits>
static_assert(std::is_same_v<decltype(&tooie_observe_title),void(*)(const uint8_t*,const recomp_context*,uint32_t)>);
using namespace tooie::title_observation;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    std::vector<uint8_t> ram(0x800000);recomp_context ctx{};ctx.r19=0x80200000;ctx.r18=0x18b;ctx.r16=1;
    auto put=[&](uint32_t a,unsigned n,uint32_t v){for(unsigned i=0;i<n;++i)ram[((a&0x1fffffff)+i)^3]=uint8_t(v>>(8*(n-i-1)));};
    put(0x80132dc2,2,0x18b);put(0x80200079,1,0x1f);put(0x8020002c,4,0x41200000);put(0x80079d69,1,1);
    put(0x8012c760,4,0x3dcccccd);put(0x8012c764,4,0x3f800000);put(0x80135470,1,2);put(0x80135474,4,0x40400000);
    const auto original=ram;Observer observer;std::vector<Record> records;
    auto sink=[&](const Record& r){records.push_back(r);};
    auto observe=[&](uint32_t site){const auto before=ctx;observer.observe(ram.data(),ram.size(),&ctx,site,sink);require(ram==original && std::memcmp(&ctx,&before,sizeof(ctx))==0,"observer changed RAM/context");};
    observe(0x80800614);require(records.back().state_valid && records.back().actor_valid && records.back().mode==1 && records.back().map==0x18b && records.back().context_map==0x18b,"map/mode extraction");
    ctx.r2=1;observe(0x80800644);require(records.back().result==1,"flag result lost");
    ctx.r2=0;observe(0x80800654);observe(0x80800664);require(records.back().result==0 && records.back().compare_rhs==1,"Start compare lost");
    observe(0x8080066c);
    ctx.r4=0x158;ctx.r5=0;ctx.r6=1;observe(0x808007ac);require(records.back().requested_map==0x158 && records.back().requested_exit==0 && records.back().requested_transition==1,"frontend destination lost");
    ctx.r4=0x80132dc0;ctx.r5=0x18b;ctx.r15=0x158;observe(0x808002c4);require(records.back().global_map_target && records.back().stored_map==0x18b && records.back().previous_map==0x158,"poststore map extraction");
    const auto timing_records=records.size();ctx.r19=0x80200000;observe(0x808006c4);
    require(records.size()==timing_records && observer.seen[7]==1 && observer.title_timing_valid.load() && observer.title_actor.load()==0x80200000 && observer.title_elapsed_bits.load()==0x41200000 && observer.title_scheduler_delta_bits.load()==0x3dcccccd && observer.title_scheduler_ticks_bits.load()==0x3f800000,"title atomic timing snapshot");
    observe(0x80800050);require(records.size()==timing_records && observer.seen[8]==1 && observer.attract_timing_valid.load() && observer.attract_phase.load()==2 && observer.attract_clock_bits.load()==0x40400000 && observer.attract_scheduler_delta_bits.load()==0x3dcccccd && observer.attract_scheduler_ticks_bits.load()==0x3f800000,"attract atomic timing snapshot");
    ctx.r4=0xa0132dc0;observe(0x808002c4);require(records.back().global_map_target,"KSEG1 map target");
    ctx.r4=0x80200000;observe(0x808002c4);require(!records.back().global_map_target,"non-global target claimed activation");
    ctx.r19=0x807ffff0;observe(0x80800614);require(!records.back().actor_valid,"actor overflow accepted");
    ctx.r19=0x80200001;observe(0x80800614);require(!records.back().actor_valid,"unaligned actor accepted");
    ctx.r19=0x00200000;observe(0x80800614);require(!records.back().actor_valid,"noncanonical actor accepted");
    ctx.r19=0xa0200000;observe(0x80800614);require(records.back().actor_valid && records.back().mode==1,"KSEG1 actor lost");
    uint8_t tiny[8]{};observer.observe(tiny,sizeof(tiny),&ctx,0x80800614,sink);require(!records.back().state_valid && !records.back().actor_valid,"tiny RAM read");
    auto failed=observer.failures.load();observer.observe(nullptr,0,nullptr,0,sink);require(observer.failures==failed+1,"null context failure");
    observe(0xdeadbeef);require(observer.failures==failed+2,"unknown site failure");
    Observer capped;unsigned n=0;ctx.r19=0x80200000;auto count=[&](const Record&){++n;};
    for(unsigned i=0;i<100;++i)capped.observe(ram.data(),ram.size(),&ctx,0x80800614,count);
    require(n==8 && capped.seen[0]==100 && capped.suppressed[0]==92,"unchanged state unbounded");
    ctx.r18=0xa1;capped.observe(ram.data(),ram.size(),&ctx,0x80800614,count);require(n==9,"changed context state suppressed");
    for(unsigned i=0;i<limits[0]+36;++i){ctx.r18=i;capped.observe(ram.data(),ram.size(),&ctx,0x80800614,count);}
    require(n==limits[0] && capped.emitted[0]==limits[0] && capped.log_attempts[0]==limits[0],"state hard cap");
    for(unsigned i=0;i<limits[4]+8;++i)capped.observe(ram.data(),ram.size(),&ctx,0x8080066c,count);
    require(capped.seen[4]==limits[4]+8 && capped.emitted[4]==limits[4] && capped.suppressed[4]==8,"transition hard cap");
    Observer throwing;throwing.observe(ram.data(),ram.size(),&ctx,0x80800614,[](const Record&){throw std::runtime_error("log");});
    require(throwing.failures==1 && throwing.emitted[0]==0 && throwing.log_attempts[0]==1,"logging failure escaped/miscounted");
    std::puts("PASS synthetic title observer: nine sites, atomic timing snapshots, const context/RAM, bounded reads, aliases, change filtering/caps, logging failure");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
