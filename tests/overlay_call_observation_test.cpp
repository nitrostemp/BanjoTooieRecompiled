#include "overlay_call_observation.hpp"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
using namespace tooie::overlay_calls;
int main() {
 Recorder r;
 Record v{};v.id=730;v.operation=Operation::entry;v.entry=0x80800100;v.observed_callsite=0x80100000;
 assert(!r.record(v));assert(r.snapshot().total==0);
 r.reset(true);
 assert(r.record(v));v.operation=Operation::returned;assert(r.record(v));
 auto s=r.snapshot();assert(s.total==2&&s.entries==1&&s.returns==1&&s.by_id[730].entries==1);
 assert(s.history[0].ordinal==1&&s.history[1].ordinal==2&&s.history[0].observed_callsite==0x80100000);
 for(unsigned i=0;i<capacity+5;++i)assert(r.record(v));
 s=r.snapshot();assert(s.total==capacity+7&&s.retained==capacity&&s.overwritten==7);
 assert(s.history[0].ordinal==8&&s.history[capacity-1].ordinal==capacity+7);
 r.reset(true);
 std::atomic_bool done=false;std::thread reader([&]{while(!done){auto p=r.snapshot();assert(p.total==p.entries+p.returns);assert(p.total==p.retained+p.overwritten);}});
 std::vector<std::thread> workers;
 for(unsigned t=0;t<4;++t)workers.emplace_back([&]{for(unsigned i=0;i<4000;++i)r.record(v);});
 for(auto&t:workers)t.join();done=true;reader.join();
 s=r.snapshot();assert(s.total==16000&&s.returns==16000&&s.by_id[730].returns==16000);
 r.reset(true);v.operation=Operation::entry;r.record(v);s=r.snapshot();assert(s.entries==1&&s.returns==0); // Exception may leave an unmatched entry.
 v.id=885;assert(r.record(v));assert(r.snapshot().invalid);bool failed=false;try{r.check_health();}catch(...){failed=true;}assert(failed);
 r.reset(false);assert(!r.record(v));r.check_health();assert(!r.snapshot().invalid);
 assert(!ordinary("request")&&!ordinary("published")&&!ordinary("moved")&&!ordinary("unloaded")&&!ordinary("failure")&&!ordinary("entry-extra")&&!ordinary(nullptr));
 assert(ordinary("entry")&&ordinary("return"));
 assert(!increment_overflows(UINT64_MAX-1)&&increment_overflows(UINT64_MAX));
 Recorder limited(2);limited.reset(true);v.id=730;limited.record(v);limited.record(v);limited.record(v);
 assert(limited.snapshot().invalid&&limited.snapshot().total==2);
}
