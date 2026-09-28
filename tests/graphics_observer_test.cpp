// Synthetic observation contracts only; not a game graphics acceptance test.
#include "graphics_observation.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace tooie::graphics_observation;
static void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
int main(){try{
    std::vector<uint8_t> ram(0x800000,0xa5);
    auto put=[&](uint32_t a,uint32_t v){std::memcpy(ram.data()+(a&0x1fffffff),&v,4);};
    recomp_context ctx{};ctx.r29=0x80001000;ctx.r4=0x8003f3f0;ctx.r2=0;
    put(0x8000101c,6);for(unsigned i=0;i<16;++i)put(0x8003f3f0+i*4,0x100+i);
    put(0x8003f3f0,1);
    const auto original=ram;
    Observer observer;std::vector<Record> records;
    auto sink=[&](const Record& r){records.push_back(r);};
    auto observe=[&](uint32_t site){auto before=ctx;observer.observe(ram.data(),&ctx,site,sink);require(std::memcmp(&before,&ctx,sizeof(ctx))==0 && ram==original,"observer modified guest state");};
    observe(0x80014aac);
    require(records.back().receive_return && records.back().receive_success && records.back().message_valid && records.back().message==6,"SP receipt lost");
    require(records.back().state_valid && records.back().state[0]==0xa5a5a5a5,"scheduler state lost");
    ctx.r2=-1;ctx.r29=0xffffffff;observe(0x80014aac);
    require(!records.back().receive_success && !records.back().message_valid,"failed receive read stale output");
    ctx.r2=0;observe(0x80014aac);
    require(records.back().receive_success && !records.back().message_valid,"invalid stack claimed actual message");
    ctx.r29=0x80001000;observe(0x80013f7c);
    require(records.back().task_valid && records.back().task[0]==1 && records.back().task[15]==0x10f,"descriptor snapshot incorrect");
    ctx.r4=0xa003f3f0;observe(0x80014070);
    require(records.back().task_valid && records.back().task_address==0xa003f3f0,"KSEG1 task alias lost");
    ctx.r4=0x807fffc4;observe(0x800147a8);require(!records.back().task_valid,"overflow task read accepted");
    ctx.r4=0x8003f3f1;observe(0x80013ecc);require(!records.back().task_valid,"unaligned task accepted");
    ctx.r4=0x0003f3f0;observe(0x80013ecc);require(!records.back().task_valid,"noncanonical task accepted");
    auto failures=observer.failures.load();observe(0xdeadbeef);require(observer.failures.load()==failures+1,"unknown site not counted");
    observer.observe(ram.data(),&ctx,0x80014aac,[](const Record&){throw std::runtime_error("synthetic log failure");});
    require(observer.failures.load()==failures+2,"logging failure escaped or disappeared");
    Observer capped;unsigned emitted=0;auto count=[&](const Record&){++emitted;};
    for(unsigned i=0;i<140;++i)capped.observe(ram.data(),&ctx,0x80014aac,count);
    require(emitted==128 && capped.seen[2]==140 && capped.suppressed[2]==12,"SP budget/suppression accounting");
    put(0x8000101c,5);emitted=0;
    for(unsigned i=0;i<20;++i)capped.observe(ram.data(),&ctx,0x80014aac,count);
    require(emitted==8 && capped.seen[1]==20 && capped.suppressed[1]==12,"VI trace unbounded");
    put(0x8000101c,4);emitted=0;
    capped.observe(ram.data(),&ctx,0x80014aac,count);
    require(emitted==1 && capped.seen[3]==1,"VI budget suppressed meaningful DP");
    ctx.r4=0x8003f3f0;emitted=0;
    for(unsigned i=0;i<70;++i)capped.observe(ram.data(),&ctx,0x80013f7c,count);
    require(emitted==64 && capped.seen[5]==70 && capped.suppressed[5]==6,"graphics task cap incorrect");
    capped.observe(ram.data(),&ctx,0x80013ecc,count);
    require(capped.seen[4]==1,"audio source not distinguished");
    std::puts("PASS synthetic read-only scheduler/task observation, invalid reads, logging failure, independent caps");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
