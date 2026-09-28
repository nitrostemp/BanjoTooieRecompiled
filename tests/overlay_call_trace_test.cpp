#include "overlay_call_trace.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <sstream>
#include <iomanip>
static std::atomic_bool reject_allocation=false;
void* operator new(std::size_t n) {if(reject_allocation)throw std::bad_alloc();if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p) noexcept {std::free(p);}
void operator delete(void*p,std::size_t) noexcept {std::free(p);}
namespace tooie {
static Json rows=Json::array();static unsigned hex_calls=0;static bool trace_is_enabled=true;
std::string hex32(uint32_t n){++hex_calls;std::ostringstream s;s<<"0x"<<std::uppercase<<std::hex<<std::setw(8)<<std::setfill('0')<<n;return s.str();}
bool trace_enabled() noexcept { return trace_is_enabled; }
namespace overlays {uint32_t current_callsite(){return 0x80102030;}}
void trace(const char* event,const char* checkpoint,const char* outcome,uint32_t pc,const recomp_context*,Json extra){
    extra.update({{"event",event},{"checkpoint",checkpoint},{"outcome",outcome},{"pc",pc}});rows.push_back(std::move(extra));
}
void original_observe(const overlays::Event& event);
}
static void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
 using namespace tooie;
 overlays::Event event{"entry",730,0x80110000,0x80110080,0x80110100,0x80102038,9};
 configure_overlay_call_observation(false);
 for(auto op:{"entry","return","request","published","moved","unloaded","future-operation"}){
  event.operation=op;original_observe(event);const auto expected=rows.back();observe_overlay_lifecycle(event);
  need(rows.back()==expected,"Default callback differs from original event fields");
 }
 need(overlay_call_observation_snapshot().is_null(),"Disabled snapshot was not null");
 trace_is_enabled=false;event.operation="entry";const auto no_trace_rows=rows.size();reject_allocation=true;observe_overlay_lifecycle(event);reject_allocation=false;
 need(rows.size()==no_trace_rows,"Inactive trace retained an ordinary overlay event");
 event.operation="published";observe_overlay_lifecycle(event);
 need(rows.size()==no_trace_rows+1,"Inactive trace suppressed a lifecycle event");
 trace_is_enabled=true;
 auto before=rows.size();configure_overlay_call_observation(true);hex_calls=0;
 for(auto op:{"entry","return"}){
  event.operation=op;
#ifdef BASELINE_CALLBACK
  original_observe(event);
#else
  reject_allocation=true;observe_overlay_lifecycle(event);reject_allocation=false;
#endif
 }
 need(rows.size()==before&&hex_calls==0,"Ordinary calls still formatted/emitted");
 auto snapshot=overlay_call_observation_snapshot();
 need(snapshot["suppressed_entries"]==1&&snapshot["suppressed_returns"]==1&&snapshot["valid"]==true,"Suppressed accounting invalid");
 for(auto op:{"request","published","moved","unloaded","future-operation"}){event.operation=op;observe_overlay_lifecycle(event);}
 need(rows.size()==before+5,"Critical/unknown lifecycle event suppressed");
 need(overlay_call_observation_snapshot()["suppressed_total"]==2,"Lifecycle rows counted as suppressed");
 // Construct the numeric schema's worst-width count/history payload independently.
 auto worst=snapshot;
 worst["per_overlay"]=Json::array();
 for(unsigned id=1;id<885;++id)worst["per_overlay"].push_back({{"overlay_id",id},{"entries",UINT64_MAX},{"returns",UINT64_MAX}});
 auto widest=worst["recent_history"][0];
 for(auto name:{"ordinal","host_monotonic_ns","generation"})widest[name]=UINT64_MAX;
 for(auto name:{"overlay_id","header","text","entry","raw_ra","observed_callsite"})widest[name]=UINT32_MAX;
 widest["operation"]="return";worst["recent_history"]=Json::array();
 for(unsigned i=0;i<1024;++i)worst["recent_history"].push_back(widest);
 need(worst.dump().size()<512*1024,"Worst-width payload exceeds live reader safety budget");
 finish_overlay_call_observation();need(rows.back()["event"]=="overlay_call_observation_summary"&&rows.back()["after_all_resource_joins"]==true,"Missing postjoin summary");
 configure_overlay_call_observation(true);event.operation="entry";event.id=885;observe_overlay_lifecycle(event);
 bool rejected=false;try{check_overlay_call_observation_health();}catch(...){rejected=true;}need(rejected,"Invalid recorder health ignored");
 rejected=false;try{finish_overlay_call_observation();}catch(...){rejected=true;}need(rejected&&rows.back()["outcome"]=="failure","Invalid summary not visible");
 std::cout<<"PASS exact default callback, no-allocation suppressed path, critical/unknown preservation, counters, final/invalid summary\n";
 return 0;
 }catch(const std::exception&e){reject_allocation=false;std::cerr<<e.what()<<'\n';return 1;}}
