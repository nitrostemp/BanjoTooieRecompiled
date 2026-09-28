#include "overlay_call_trace.hpp"
#include "overlay_call_observation.hpp"
#include <stdexcept>
namespace tooie {
namespace { overlay_calls::Recorder recorder; }
void configure_overlay_call_observation(bool enabled) {recorder.reset(enabled);}
void check_overlay_call_observation_health() {recorder.check_health();}
void observe_overlay_lifecycle(const overlays::Event& event) {
    const bool ordinary=overlay_calls::ordinary(event.operation);
    if(recorder.enabled()&&ordinary) {
        overlay_calls::Record row;
        row.operation=std::string_view(event.operation)=="entry"?overlay_calls::Operation::entry:overlay_calls::Operation::returned;
        row.id=event.id;row.header=event.header;row.text=event.text;row.entry=event.entry;
        row.raw_ra=event.caller;row.observed_callsite=overlays::current_callsite();row.generation=event.generation;
        recorder.record(row);
        return; // Observation only: the original dispatcher proceeds unchanged.
    }
    // Frontend play has neither the optional numeric recorder nor a trace
    // transport. Do not construct JSON, lock trace state, or retain a row for
    // routine overlay entries in that mode. State-changing lifecycle events
    // remain observable, and explicit mission tracing retains ordinary calls.
    if(ordinary&&!trace_enabled()) return;
    trace("overlay_lifecycle","G4",event.operation,event.entry,nullptr,
        {{"overlay_id",event.id},{"header",hex32(event.header)},{"text",hex32(event.text)},
         {"entry",hex32(event.entry)},{"raw_ra",hex32(event.caller)},
         {"observed_callsite",overlays::current_callsite()?Json(hex32(overlays::current_callsite())):Json(nullptr)},
         {"generation",event.generation}});
}
Json overlay_call_observation_snapshot(bool after_all_resource_joins) {
    if(!recorder.enabled())return nullptr;
    const auto snapshot=recorder.snapshot(); // Release numeric lock before JSON/trace lock.
    Json counts=Json::array(),history=Json::array();
    for(unsigned id=0;id<snapshot.by_id.size();++id) {
        const auto& count=snapshot.by_id[id];
        if(count.entries||count.returns)counts.push_back({{"overlay_id",id},{"entries",count.entries},{"returns",count.returns}});
    }
    for(std::size_t i=0;i<snapshot.retained;++i) {
        const auto& row=snapshot.history[i];
        history.push_back({{"ordinal",row.ordinal},{"host_monotonic_ns",row.host_monotonic_ns},
            {"operation",row.operation==overlay_calls::Operation::entry?"entry":"return"},
            {"overlay_id",row.id},{"header",row.header},{"text",row.text},{"entry",row.entry},
            {"raw_ra",row.raw_ra},{"observed_callsite",row.observed_callsite},{"generation",row.generation}});
    }
    return {{"schema",1},{"policy","ordinary-overlay-entry-return-summary"},
        {"enabled",true},{"valid",!snapshot.invalid},{"after_all_resource_joins",after_all_resource_joins},
        {"suppressed_total",snapshot.total},{"suppressed_entries",snapshot.entries},{"suppressed_returns",snapshot.returns},
        {"counts_complete",!snapshot.invalid},{"per_overlay",std::move(counts)},
        {"history_capacity",overlay_calls::capacity},{"history_retained",snapshot.retained},
        {"history_overwritten",snapshot.overwritten},{"history_complete",snapshot.overwritten==0&&!snapshot.invalid},
        {"history_order","suppressed-record ordinal only; not events.jsonl sequence"},
        {"recent_history",std::move(history)}};
}
void finish_overlay_call_observation() {
    auto snapshot=overlay_call_observation_snapshot(true);
    if(!snapshot.is_null()) {
        const bool valid=snapshot["valid"].get<bool>();
        // Existing live readers reject >1MiB rows. Leave ample room for the trace envelope.
        if(snapshot.dump().size()>512*1024)throw std::runtime_error("Overlay call summary exceeds bounded live trace payload");
        trace("overlay_call_observation_summary","diagnostic",valid?"recorded":"failure",0,nullptr,std::move(snapshot));
    }
    recorder.check_health();
}
}
