// Synthetic observer contracts, never game/input acceptance.
#include <cstdio>
#if !__has_include("menu_observation.hpp")
int main(){std::fputs("FAIL menu observer implementation missing\n",stderr);return 1;}
#else
#include "menu_observation.hpp"
#include "boot_hooks.h"
#include <cstring>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
using namespace tooie::menu_observation;
static_assert(std::is_same_v<decltype(&tooie_observe_menu),void(*)(const uint8_t*,const recomp_context*,uint32_t)>);
static void need(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(){try{
 std::vector<uint8_t> ram(0x800000);recomp_context ctx{};Record last;Observer observer;
 auto put=[&](uint32_t a,unsigned n,uint32_t value){for(unsigned i=0;i<n;++i)ram[((a&0x1fffffff)+i)^3]=uint8_t(value>>(8*(n-i-1)));};
 constexpr uint32_t root=0x80200000,child=0x80200100,stack=0x80300000;
 put(0x80132dc2,2,0x158);put(root,4,0x80500100);put(root+0x72,2,1<<10);put(root+0x79,1,0x3f);
 put(child,4,0x80500200);put(child+0x3c,4,0x80500100);put(child+0x79,1,0x3a);put(child+0x64,4,1<<20);put(child+0x58,4,0x3f000000);
 for(unsigned i=0;i<3;++i){put(0x80127618+i*4,4,0x80400000+i);put(0x80400000+i,1,i==1?1:0);}
 ctx.r2=root;ctx.r16=child;ctx.r29=stack;
 put(stack+0x28,4,0x3e800000);put(stack+0x2c,4,0xbf000000);put(stack+0x30,4,root);
 auto observe=[&](unsigned cat){auto before=ctx;auto memory=ram;observer.observe(ram.data(),ram.size(),&ctx,sites[cat],[&](const Record& r){last=r;});need(memory==ram && !std::memcmp(&before,&ctx,sizeof(ctx)),"guest RAM/context changed");};
 observe(0);need(last.valid() && last.actor.address==root && last.actor.marker==0x80500100 && last.actor.state==1 && last.actor.selection==3,"resolved root contract");
 need(last.slots_present && last.slots_valid && last.slots[0].value==0 && last.slots[1].value==1 && last.slots[2].value==0,"three occupancy bytes");
 observe(1);need(last.valid() && last.actor.child && last.actor.parent_marker==0x80500100 && last.actor.flags==(1u<<20) && last.actor.timer_bits==0x3f000000,"child extraction");
 need(last.x_bits==0x3e800000 && last.y_bits==0xbf000000 && last.stick_valid && last.x_finite && last.y_finite,"original stack stick bits");
 ctx.r6=child;ctx.r10=0x2a;observe(2);need(last.old_selection==2 && last.actor.selection==3,"committed selection source");
 ctx.r2=1;for(unsigned i=3;i<=5;++i){observe(i);need(last.result==1 && last.actor.address==child,"original flag/button result");}
 ctx.r4=root;ctx.r5=3;observe(6);need(last.actor.address==root && last.event_selection==3,"A entry args");
 ctx.r4=0xdeadbeef;ctx.r2=0;observe(7);need(last.valid() && last.result==0 && last.actor.address==root,"A query stack root, not A0");
 put(stack+0x2c,4,5);observe(8);need(last.valid() && last.message==5 && last.actor.address==root,"A return message/root");
 ctx.r4=root;ctx.r5=3;observe(9);need(last.valid() && last.event_selection==3,"B dispatch");
 ctx.r4=0xdeadbeef;observe(10);need(last.valid() && !last.actor.present && !last.slots_present,"B return must not dereference clobbered A0");
 ctx.r2=0xa0200000;observe(0);need(last.valid() && last.actor.address==0xa0200000,"KSEG1 actor alias");
 put(0x80127618,4,0xa0400000);observe(0);need(last.valid(),"KSEG1 slot alias");
 ctx.r2=0x807ffff0;observe(0);need(!last.actor.valid && !last.valid(),"actor overrun");
 ctx.r2=0x80200001;observe(0);need(!last.actor.valid,"unaligned actor");
 ctx.r2=0x00200000;observe(0);need(!last.actor.valid,"noncanonical actor");
 ctx.r2=root;put(0x8012761c,4,0);observe(0);need(!last.slots_valid && !last.slots[1].valid && last.actor.valid,"invalid slot distinct from empty");
 put(0x8012761c,4,0x80400001);ctx.r29=0x807ffff0;observe(1);need(!last.stack_valid && !last.stick_valid,"stack overflow");
 ctx.r29=0xfffffff0;observe(7);need(!last.stack_valid && !last.actor.valid,"stack wrap");
 ctx.r29=stack;put(stack+0x28,4,0x7fc00001);put(child+0x58,4,0x7f800000);observe(1);
 need(last.x_bits==0x7fc00001 && !last.x_finite && !last.actor.timer_finite && !last.valid(),"nonfinite retained and flagged");
 put(stack+0x28,4,0);put(stack+0x2c,4,0);put(child+0x58,4,0);put(0x80127618,4,0x80400000);
 Observer small;uint8_t tiny[8]{};small.observe(tiny,8,&ctx,sites[1],[&](const Record&r){last=r;});need(!last.globals_valid && !last.actor.valid,"tiny RAM guard");
 auto failures=small.failures.load();small.observe(nullptr,0,nullptr,sites[0],[](const Record&){});small.observe(ram.data(),ram.size(),&ctx,0,[](const Record&){});need(small.failures==failures+2,"null/unknown failures");
 // A different parent is reported raw; no cached root lookup guesses a match.
 put(child+0x3c,4,0x80500900);observe(1);need(last.actor.parent_marker==0x80500900,"parent marker mismatch concealed");
 Observer filtered;unsigned n=0;auto count=[&](const Record&){++n;};
 for(unsigned i=0;i<100;++i){put(child+0x58,4,i);filtered.observe(ram.data(),ram.size(),&ctx,sites[1],count);}
 need(n==8 && filtered.filtered[1]==92,"timer changes consume event cap");
 put(stack+0x28,4,0x3f800000);filtered.observe(ram.data(),ram.size(),&ctx,sites[1],count);need(n==9,"stick change hidden");
 for(unsigned i=0;i<200;++i){put(stack+0x28,4,i);filtered.observe(ram.data(),ram.size(),&ctx,sites[1],count);}
 need(filtered.attempts[1]==128 && filtered.emitted[1]==128 && filtered.capped[1]>0,"recurring hard cap");
 for(unsigned i=0;i<50;++i)filtered.observe(ram.data(),ram.size(),&ctx,sites[10],count);
 need(filtered.emitted[10]==32 && filtered.capped[10]==18,"action hard cap");
 Observer throwing;throwing.observe(ram.data(),ram.size(),&ctx,sites[10],[](const Record&){throw std::runtime_error("sink");});need(throwing.failures==1 && throwing.emitted[10]==0 && throwing.attempts[10]==1,"sink failure isolation");
 Observer parallel;std::vector<std::thread> threads;std::atomic<unsigned> calls{0};const auto before=ram;
 for(unsigned i=0;i<12;++i)threads.emplace_back([&]{auto c=ctx;for(unsigned j=0;j<25;++j)parallel.observe(ram.data(),ram.size(),&c,sites[10],[&](const Record&){++calls;});});
 for(auto& t:threads)t.join();need(calls==32 && parallel.seen[10]==300 && parallel.capped[10]==268 && ram==before,"concurrent cap/const contract");
 puts("PASS menu observer: eleven contracts, slots, bounds/aliases, float bits, RAM/context, filtering/caps, concurrent admission, sink isolation");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
#endif
