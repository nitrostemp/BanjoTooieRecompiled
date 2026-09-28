#include "trace_transport.hpp"
#include <limits>
#include <stdexcept>
namespace tooie {
TraceTransport::~TraceTransport() noexcept {try{stop();}catch(...){}}
void TraceTransport::start(std::ostream& sink,TraceTransportOptions options) {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);
    if(running_||flusher_.joinable())throw std::logic_error("Trace transport already started");
    if(options.period.count()<=0||options.pending_bytes==0)throw std::invalid_argument("Invalid trace flush options");
    if(!sink)throw std::runtime_error("Trace sink unavailable");
    sink_=&sink;options_=options;pending_=0;stopping_=false;failure_=nullptr;running_=true;
    try{if(options_.buffered)flusher_=std::thread([this]{worker();});}
    catch(...){running_=false;sink_=nullptr;throw;}
}
void TraceTransport::check_locked() {
    if(failure_)std::rethrow_exception(failure_);
    if(!running_||stopping_)throw std::logic_error("Trace transport is stopped");
}
void TraceTransport::fail_locked(std::exception_ptr error) noexcept {
    if(!failure_)failure_=error;
    cv_.notify_all();
}
void TraceTransport::flush_locked() {
    sink_->flush();
    if(!*sink_)throw std::runtime_error("Trace sink flush failed");
    pending_=0;
}
void TraceTransport::append(std::string_view row,bool barrier) {
    std::lock_guard lock(mutex_);check_locked();
    try{
        if(row.size()>size_t(std::numeric_limits<std::streamsize>::max())||row.size()==std::numeric_limits<size_t>::max())
            throw std::length_error("Trace row exceeds stream capacity");
        if(pending_>std::numeric_limits<size_t>::max()-row.size()-1)flush_locked();
        sink_->write(row.data(),static_cast<std::streamsize>(row.size()));sink_->put('\n');
        if(!*sink_)throw std::runtime_error("Trace sink write failed");
        pending_+=row.size()+1;
        if(!options_.buffered||barrier||pending_>=options_.pending_bytes)flush_locked();
    }catch(...){fail_locked(std::current_exception());throw;}
}
void TraceTransport::flush() {
    std::lock_guard lock(mutex_);check_locked();
    try{flush_locked();}catch(...){fail_locked(std::current_exception());throw;}
}
void TraceTransport::check_health() {std::lock_guard lock(mutex_);check_locked();}
void TraceTransport::worker() noexcept {
    std::unique_lock lock(mutex_);
    while(!stopping_&&!failure_){
        cv_.wait_for(lock,options_.period,[this]{return stopping_||bool(failure_);});
        if(stopping_||failure_)break;
        if(pending_)try{flush_locked();}catch(...){fail_locked(std::current_exception());}
    }
}
void TraceTransport::stop() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    {std::lock_guard lock(mutex_);stopping_=true;cv_.notify_all();}
    if(flusher_.joinable())flusher_.join();
    std::lock_guard lock(mutex_);
    if(running_&&!failure_&&pending_)try{flush_locked();}catch(...){fail_locked(std::current_exception());}
    running_=false;sink_=nullptr;
    if(failure_)std::rethrow_exception(failure_);
}
}
