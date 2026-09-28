#include "map_actor_list.hpp"
#include "boot_hooks.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <type_traits>
static_assert(std::is_same_v<decltype(&tooie_observe_map_actor_list),void(*)(const uint8_t*,const recomp_context*,uint32_t)>);
using namespace tooie::map_actor_list;
static void need(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
int main(){try{
 std::vector<uint8_t> ram(0x800000);recomp_context c{};
 auto put=[&](uint32_t a,unsigned n,uint32_t v){for(unsigned i=0;i<n;++i)ram[((a&0x1fffffff)+i)^3]=uint8_t(v>>(8*(n-i-1)));};
 put(0x80132dc2,2,0x14f);c.r17=0x80200000;c.r20=3;
 for(unsigned i=0;i<500;++i){put(0x80200000+i*4,4,0x80300000+i*20);put(0x80300008+i*20,2,i==1?0x111:i+0x111);}
 const auto original=ram;Snapshot last;unsigned logs=0;
 auto sink=[&](const Snapshot& s){last=s;++logs;};
 auto check=[&](const char* error){Observer o;auto before=c;o.observe(ram.data(),ram.size(),&c,site,sink);need(std::memcmp(&before,&c,sizeof(c))==0 && ram==original,"guest mutation");need(last.error==error,"wrong rejection reason");return o.emitted.load();};
 check("");need(last.complete && last.count==3 && last.map==0x14f && last.rows[0].marker==0x111 && last.rows[1].marker==0x111 && last.rows[2].marker==0x113,"ordered duplicates");
 need(last.rows[2].bytes[8]==1 && last.rows[2].bytes[9]==0x13,"record byte order");
 c.r20=500;check("");need(last.count==500 && last.complete,"capacity500 rejected");
 c.r20=501;check("count_out_of_bounds");need(!last.complete && last.count==0,"partial batch exposed");
 c.r20=uint64_t(-1);check("count_out_of_bounds");c.r20=0;c.r17=0;check("");need(last.complete && !last.count,"zero list");
 c.r20=1;c.r17=0x807ffffc;check("invalid_record");
 c.r20=2;check("invalid_array");c.r17=0x80200001;check("invalid_array");c.r17=0x00200000;check("invalid_array");
 c.r17=0xa0200000;c.r20=3;check("");need(last.complete,"array alias");
 c.r19=1;check("wrong_iteration_state");c.r19=0;
 Observer tiny;uint8_t small[8]{};tiny.observe(small,8,&c,site,sink);need(!last.complete && last.error=="invalid_memory","tiny memory accepted");
 Observer null;null.observe(nullptr,0,nullptr,site,sink);need(null.failures==1 && null.emitted==0,"null ctx");
 null.observe(ram.data(),ram.size(),&c,site+4,sink);need(null.failures==2 && null.emitted==0,"wrongsite");
 // An invalid later record must suppress every row, not emit a valid prefix.
 put(0x80200008,4,0x807ffff0);Observer bad;bad.observe(ram.data(),ram.size(),&c,site,sink);
 need(!last.complete && last.error_index==2 && last.count==0 && bad.incomplete==1,"partial record set exposed");
 put(0x80200008,4,0xa0300028);Observer alias;alias.observe(ram.data(),ram.size(),&c,site,sink);need(last.complete && last.rows[2].marker==0x113,"record alias");
 put(0x80200008,4,0x80300029);Observer unaligned;unaligned.observe(ram.data(),ram.size(),&c,site,sink);need(!last.complete,"unaligned record");
 put(0x80200008,4,0x80300028);need(ram==original,"fixture not restored");
 Observer fail;fail.observe(ram.data(),ram.size(),&c,site,[](const Snapshot&){throw std::runtime_error("sink");});
 need(fail.failures==1 && fail.emitted==0 && fail.attempts==1,"sink failure escaped");
 Observer concurrent;std::atomic<unsigned> n{0};std::vector<std::thread> workers;
 for(unsigned i=0;i<12;++i)workers.emplace_back([&]{auto ctx=c;for(unsigned j=0;j<25;++j)concurrent.observe(ram.data(),ram.size(),&ctx,site,[&](const Snapshot& s){need(s.complete,"concurrent bad record");++n;});});
 for(auto& t:workers)t.join();need(n==8 && concurrent.seen==300 && concurrent.attempts==8 && concurrent.suppressed==292 && concurrent.emitted==8,"concurrent cap");
 need(ram==original,"concurrent RAM mutation");
 std::puts("PASS map actor list: full records/order/duplicates/500-cap, invalid and incomplete rejection, aliases, fullctx/8MiB conservation, exceptions, concurrent8-log limit");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
