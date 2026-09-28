#include "overlay_call_observation.hpp"
#include <chrono>
#include <stdexcept>
namespace tooie::overlay_calls {
void Recorder::reset(bool enabled) {
    std::lock_guard lock(mutex_);
    total_=entries_=returns_=0;by_id_={};ring_={};invalid_.store(false);
    enabled_.store(enabled,std::memory_order_relaxed);
}
bool Recorder::record(Record value) noexcept {
    if(!enabled())return false;
    try {
        std::lock_guard lock(mutex_);
        if(invalid_.load()||!value.id||value.id>=stable_ids||
           (value.operation!=Operation::entry&&value.operation!=Operation::returned)||
           total_>=ceiling_||increment_overflows(total_)) {invalid_.store(true);return true;}
        value.ordinal=++total_;
        value.host_monotonic_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if(value.operation==Operation::entry){++entries_;++by_id_[value.id].entries;}
        else {++returns_;++by_id_[value.id].returns;}
        ring_[(total_-1)%capacity]=value;
    }catch(...){invalid_.store(true);}
    return true;
}
Snapshot Recorder::snapshot() const {
    std::lock_guard lock(mutex_);
    Snapshot out;out.enabled=enabled();out.invalid=invalid_.load();
    out.total=total_;out.entries=entries_;out.returns=returns_;out.by_id=by_id_;
    out.retained=static_cast<std::size_t>(total_<capacity?total_:capacity);
    out.overwritten=total_-out.retained;
    for(std::size_t i=0;i<out.retained;++i)out.history[i]=ring_[(out.overwritten+i)%capacity];
    return out;
}
void Recorder::check_health() const {
    if(invalid_.load())throw std::runtime_error("Overlay call observation invalid: ID/operation/counter/recorder failure; counts are incomplete");
}
}
