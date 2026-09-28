#include "persistent_state_scheduler.hpp"
#include "tooie_overlays.hpp"
#include "boot_bridge.hpp"
#include "replay_timing.hpp"
#include "scene_observer.hpp"
#include "runtime_lifecycle.hpp"
#include "ultramodern/ultramodern.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <limits>
extern uint8_t dmem[0x1000];

namespace tooie::persistent_state::scheduler {
namespace {
enum class Phase {Starting,Running,SchedulerWait,ExternalWait,Parked,Unwound,DeferredResume,Stopping};
struct Worker {
    uint8_t* rdram=nullptr;
    uint32_t address=0;
    uint64_t entry=0,sp=0,arg=0;
    UltraThreadContext* native=nullptr;
    continuation::Machine* machine=nullptr;
    recomp_context* context=nullptr;
    Phase phase=Phase::Starting;
    bool wake_pending=false;
    ThreadImage restored;
    uint32_t replay_active=0;
    uint32_t rounding_mode=0;
};
std::mutex mutex;
std::condition_variable cv;
std::map<uint32_t,Worker*> workers;
thread_local Worker self;
thread_local bool retirement_pending=false;
bool requested=false,leased=false,unwinding=false,committed=false;
uint32_t idle=0;
size_t retiring=0;
std::string last_error;
bool all_parked() {
    if(!idle||workers.empty()||retiring)return false;
    return std::all_of(workers.begin(),workers.end(),[](const auto& item){
        return item.second->phase==Phase::Parked||item.second->phase==Phase::SchedulerWait;
    });
}
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void validate_queues(const Snapshot& input) {
    auto valid_address=[](uint32_t address,size_t size,size_t alignment) {
        return address>=0x80000400&&address%alignment==0&&uint64_t(address)+size<=0x80800000;
    };
    std::map<int32_t,const OSThread*> threads;
    std::map<int32_t,int32_t> roots;
    roots.emplace(ultramodern::running_queue,input.running_queue_head);
    for(const auto& image:input.threads) {
        check(valid_address(image.address,sizeof(OSThread),alignof(OSThread)),"snapshot thread outside RDRAM");
        auto* thread=reinterpret_cast<const OSThread*>(input.rdram.data()+(image.address&0x1FFFFFFF));
        check(thread->context==nullptr,"snapshot contains native thread pointer");
        threads.emplace(static_cast<int32_t>(image.address),thread);
        if(thread->queue&&thread->queue!=ultramodern::running_queue) {
            check(valid_address(static_cast<uint32_t>(thread->queue),4,4),"snapshot wait queue outside RDRAM");
            int32_t head;std::memcpy(&head,input.rdram.data()+(static_cast<uint32_t>(thread->queue)&0x1FFFFFFF),4);
            roots.emplace(thread->queue,head);
        }
    }
    std::map<int32_t,bool> linked;
    for(const auto& [queue,head]:roots) {
        auto address=head;int64_t previous_priority=std::numeric_limits<int32_t>::max();
        while(address) {
            check(threads.contains(address)&&linked.emplace(address,true).second,"snapshot queue cycle, duplicate or unknown thread");
            const auto* thread=threads.at(address);
            check(thread->queue==queue&&thread->priority<=previous_priority,"snapshot queue ownership/priority mismatch");
            previous_priority=thread->priority;address=thread->next;
        }
    }
    for(const auto& [address,thread]:threads)
        check(!thread->queue||linked.contains(address),"snapshot queued thread is unreachable from its head");
    check(threads.contains(static_cast<int32_t>(input.external_wait_thread)),"snapshot idle owner missing");
    for(const auto& message:input.external_messages)
        check(valid_address(static_cast<uint32_t>(message.queue),sizeof(OSMesgQueue),alignof(OSMesgQueue)),"external message queue outside RDRAM");
}
void stop_check(){if(lifecycle::stopping())throw ultramodern::thread_terminated{};}
void park(std::unique_lock<std::mutex>& lock) {
    self.phase=Phase::Parked;idle=self.address;cv.notify_all();
    while(requested&&!unwinding) {
        cv.wait_for(lock,std::chrono::milliseconds{10});if(!leased)stop_check();
    }
    if(unwinding)throw RestoreUnwind{};
    stop_check();
    self.phase=Phase::Running;
}
}
void worker_started(uint8_t* rdram,uint32_t address,uint64_t entry,uint64_t sp,uint64_t arg,UltraThreadContext* native) {
    if(!devices::experiment_enabled())return;
    std::unique_lock lock(mutex);
    check(!leased,"worker creation during parked checkpoint");
    auto* guest=reinterpret_cast<OSThread*>(rdram+(address&0x1FFFFFFF));
    check(native&&guest->context==native,"new worker does not own guest thread context");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{10};
    // osDestroyThread wakes the old host worker asynchronously. osCreateThread
    // may already have replaced the same guest structure before it retires.
    // The creator is still Running and waiting for our initialized signal, so
    // admission cannot mistake this unregistered successor for a parked epoch.
    while(workers.contains(address)||idle) {
        if(const auto old=workers.find(address);old!=workers.end())
            check(old->second->native!=native,"duplicate native worker registration");
        stop_check();
        check(!leased,"worker replacement during parked checkpoint");
        check(std::chrono::steady_clock::now()<deadline,"guest worker retirement timed out");
        cv.wait_for(lock,std::chrono::milliseconds{10});
    }
    stop_check();
    check(!leased&&guest->context==native,"worker context changed during retirement handshake");
    self={};self.rdram=rdram;self.address=address;self.entry=entry;self.sp=sp;self.arg=arg;self.native=native;
    workers.emplace(address,&self);
}
void worker_attached(continuation::Machine* machine,recomp_context* context) {
    std::lock_guard lock(mutex);self.machine=machine;self.context=context;self.phase=Phase::Running;
}
void worker_context_leaving() noexcept {
    std::lock_guard lock(mutex);self.context=nullptr;self.machine=nullptr;self.phase=Phase::Stopping;cv.notify_all();
}
void worker_finished() noexcept {
    std::lock_guard lock(mutex);
    if(const auto found=workers.find(self.address);found!=workers.end()&&found->second==&self) {
        ++retiring;retirement_pending=true;workers.erase(found);
    }
    self={};cv.notify_all();
}
void worker_retirement_complete() noexcept {
    std::lock_guard lock(mutex);
    if(retirement_pending){--retiring;retirement_pending=false;}
    cv.notify_all();
}
void before_scheduler_wait() {
    std::lock_guard lock(mutex);if(self.address) {
        self.restored.overlay_callsites=overlays::persistent_callsites();
        // A successor can run and wake us again before this handoff reaches
        // the native wait. Never publish that pending token as a blocked thread.
        self.phase=self.wake_pending?Phase::DeferredResume:Phase::SchedulerWait;
        self.replay_active=replay_timing::active_divisor;
        self.rounding_mode=get_cop1_cs();
        cv.notify_all();
    }
}
void before_scheduler_signal(UltraThreadContext* target) {
    std::lock_guard lock(mutex);
    for(auto& [address,worker]:workers)if(worker->native==target) {
        check(!leased,"guest scheduling signal during checkpoint lease");
        worker->wake_pending=true;
        worker->phase=Phase::DeferredResume;
        cv.notify_all();
        return;
    }
}
void after_scheduler_wait() {
    std::unique_lock lock(mutex);
    if(!self.address)return;
    if(unwinding&&leased)throw RestoreUnwind{};
    self.wake_pending=false;
    // A semaphore token may have been pending when the host requested a lease.
    // Do not execute guest instructions during capture, and do not pretend
    // this consumed wake is an ordinary blocked thread in a persistent image.
    if(requested&&(leased||idle)) {
        self.phase=Phase::DeferredResume;cv.notify_all();
        while(requested&&!unwinding) {cv.wait_for(lock,std::chrono::milliseconds{10});if(!leased)stop_check();}
        if(unwinding)throw RestoreUnwind{};
    }
    stop_check();self.phase=Phase::Running;
}
void before_external_wait() {
    std::unique_lock lock(mutex);
    if(!self.address)return;
    self.restored.overlay_callsites=overlays::persistent_callsites();
    self.replay_active=replay_timing::active_divisor;
    self.rounding_mode=get_cop1_cs();
    if(requested)park(lock);
    self.phase=Phase::ExternalWait;
}
void after_external_wait() {
    std::unique_lock lock(mutex);
    if(!self.address)return;
    self.phase=Phase::Running;
}
bool freeze(std::chrono::milliseconds timeout,std::string& reason) {
    std::unique_lock lock(mutex);
    if(requested||leased||workers.empty()) {reason="guest lease busy or no workers";return false;}
    requested=true;idle=0;committed=false;
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    while(!all_parked()&&!lifecycle::stopping()) {
        if(cv.wait_until(lock,deadline)==std::cv_status::timeout)break;
    }
    if(!all_parked()) {
        reason="guest boundary unavailable idle="+std::to_string(idle)+" retiring="+std::to_string(retiring)+" phases=";
        for(const auto& [address,worker]:workers)
            reason+=std::to_string(address)+":"+std::to_string(static_cast<unsigned>(worker->phase))+",";
        reason+=" (0=start,1=run,2=sched,3=external,4=park,5=unwound,6=deferred,7=stop)";
        requested=false;idle=0;cv.notify_all();return false;
    }
    leased=true;return true;
}
void release() noexcept {
    std::lock_guard lock(mutex);
    // A failed mutated restore never reaches this normal release path.
    requested=false;leased=false;unwinding=false;committed=true;idle=0;cv.notify_all();
}
void terminal_release() noexcept {
    std::lock_guard lock(mutex);
    requested=false;leased=false;unwinding=false;committed=false;idle=0;cv.notify_all();
}
std::string refusal_reason(){std::lock_guard lock(mutex);return last_error;}
bool capture_guest(void* pointer,uint64_t epoch) noexcept {
    try {
        std::lock_guard lock(mutex);
        check(leased&&all_parked(),"guest capture lacks parked lease");
        check(!lifecycle::stopping(),"guest stopped during capture");
        auto& output=*static_cast<Snapshot*>(pointer);
        const auto scene=scene::snapshot();
        check(!scene.activation_active,"capture during map activation is unsupported");
        output.scene_map_available=scene.map_available;output.scene_map_id=scene.map_id;
        std::string host_reason;
        if(!frontend::capture_frozen(output.host,host_reason))throw std::runtime_error(host_reason);
        output.epoch=epoch;output.threads.clear();
        auto* rdram=workers.begin()->second->rdram;
        output.rdram.assign(rdram,rdram+continuation::Machine::memory_bytes);
        for(const auto& [address,worker]:workers) {
            check(worker->machine&&worker->context,"unstarted worker cannot yet be serialized");
            for(const auto& frame:worker->machine->frames) {
                try {worker->machine->validate_snapshot_frames(std::span<const continuation::Frame>(&frame,1));}
                catch(const std::exception& error) {
                    throw std::runtime_error("thread="+std::to_string(address)+" function="+std::to_string(frame.function)+
                        " pc="+std::to_string(frame.pc)+" "+error.what());
                }
            }
            check(!worker->machine->frames.empty(),"empty live continuation stack");
            const auto top=worker->machine->frames.back().function;
            check(address==idle?top==0xF0000007:(top>=0xF0000001&&top<=0xF0000006),
                "capture requires an explicitly staged HLE wait boundary");
            ThreadImage image;
            image.address=address;image.entry=worker->entry;image.initial_sp=worker->sp;image.initial_arg=worker->arg;
            image.phase=address==idle?ThreadPhase::ExternalWait:ThreadPhase::SchedulerWait;
            image.context=*worker->context;image.context.f_odd=nullptr;
            image.frames=worker->machine->frames;
            // Captured by each worker at its native wait boundary.
            image.overlay_callsites=worker->restored.overlay_callsites;
            image.replay_active_divisor=worker->replay_active;
            image.cop1_rounding_mode=worker->rounding_mode;
            output.threads.push_back(std::move(image));
            auto* guest_thread=reinterpret_cast<OSThread*>(output.rdram.data()+(address&0x1FFFFFFF));
            guest_thread->context=nullptr;
        }
        output.external_wait_thread=idle;
        output.running_queue_head=ultramodern::thread_queue_empty(rdram,ultramodern::running_queue)?0:
            ultramodern::thread_queue_peek(rdram,ultramodern::running_queue);
        output.boot_fcsr=boot_fcsr_shadow();
        auto overlays=overlays::persistent_export();
        output.overlays.clear();output.overlay_generations.clear();
        for(const auto& value:overlays.residents)output.overlays.push_back({value.id,value.header,value.text,value.active,value.generation});
        for(const auto& value:overlays.generations)output.overlay_generations.push_back({value.id,value.generation});
        output.si=si::persistent_export();
        output.replay_pending_divisor=replay_timing::pending_divisor.load();
        output.replay_published_refresh_rate=replay_timing::published_refresh_rate.load();
        std::copy(std::begin(dmem),std::end(dmem),output.rsp_dmem.begin());
        check(tooie_persistent_external_export(&output.external_messages),"external message export refused");
        validate_queues(output);
        return true;
    } catch(const std::exception& error) {last_error=error.what();return false;}
    catch(...) {last_error="unknown guest capture failure";return false;}
}
bool validate_restore(const Snapshot& input,std::string& reason) {
    try {
        std::lock_guard lock(mutex);
        check(input.rdram.size()==continuation::Machine::memory_bytes,"snapshot RDRAM length");
        validate_queues(input);
        check(input.threads.size()==workers.size(),"live and snapshot worker sets differ");
        check(input.boot_fcsr==boot_fcsr_shadow(),"unsupported boot FCSR restore");
        check(input.si.version==1,"SI snapshot version");
        if(!frontend::validate_restore(input.host,reason))throw std::runtime_error(reason);
        check(input.replay_pending_divisor<=15&&input.replay_published_refresh_rate<=60,"invalid replay timing globals");
        size_t external_count=0;
        std::map<uint32_t,bool> seen;
        for(const auto& image:input.threads) {
            check(workers.contains(image.address)&&seen.emplace(image.address,true).second,"snapshot worker identity mismatch");
            const auto* worker=workers.at(image.address);
            check(worker->entry==image.entry&&worker->sp==image.initial_sp&&worker->arg==image.initial_arg,"snapshot worker creation differs");
            check(worker->machine&&worker->context,"unstarted restore worker");
            worker->machine->validate_snapshot_frames();
            worker->machine->validate_snapshot_frames(image.frames);
            check(image.frames.back().function==(image.phase==ThreadPhase::ExternalWait?0xF0000007:image.frames.back().function),"restored idle frame mismatch");
            if(image.phase==ThreadPhase::SchedulerWait)
                check(image.frames.back().function>=0xF0000001&&image.frames.back().function<=0xF0000006,"restored scheduler frame unsupported");
            check(!image.context.mips3_float_mode,"FR=1 restore unsupported");
            check(image.replay_active_divisor<=15,"invalid per-thread replay divisor");
            check(image.cop1_rounding_mode<=3,"invalid per-thread COP1 rounding mode");
            if(image.phase==ThreadPhase::ExternalWait) {++external_count;check(image.address==input.external_wait_thread,"idle identity mismatch");}
            else check(image.phase==ThreadPhase::SchedulerWait,"unsupported thread phase");
            check(image.overlay_callsites.size()<=1024,"oversized callsite stack");
        }
        check(external_count==1,"snapshot needs exactly one external idle owner");
        overlays::PersistentState overlay;
        for(const auto& value:input.overlays)overlay.residents.push_back({value.id,value.header,value.text,value.active,value.generation});
        for(const auto& value:input.overlay_generations)overlay.generations.push_back({value.id,value.generation});
        overlays::persistent_validate(const_cast<uint8_t*>(input.rdram.data()),overlay);
        return true;
    } catch(const std::exception& error) {reason=error.what();return false;}
}
bool restore_guest(void* pointer,uint64_t) noexcept {
    try {
        const auto& input=*static_cast<const Snapshot*>(pointer);
        std::unique_lock lock(mutex);
        check(leased&&all_parked(),"restore lacks parked guest lease");
        auto* rdram=workers.begin()->second->rdram;
        // Allocate all per-worker images before destroying old native stacks.
        for(const auto& image:input.threads)workers.at(image.address)->restored=image;
        unwinding=true;cv.notify_all();
        for(const auto& [address,worker]:workers)
            if(worker->phase==Phase::SchedulerWait)worker->native->running.signal();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{3};
        while(!std::all_of(workers.begin(),workers.end(),[](const auto& item){return item.second->phase==Phase::Unwound;}))
            if(cv.wait_until(lock,deadline)==std::cv_status::timeout)throw std::runtime_error("guest native unwind timed out");
        // Save the new runnable order before mutating its intrusive next links.
        std::vector<int32_t> ready;
        auto next=input.running_queue_head;
        while(next) {
            check(ready.size()<workers.size()&&workers.contains(static_cast<uint32_t>(next)),"invalid snapshot ready chain");
            check(std::find(ready.begin(),ready.end(),next)==ready.end(),"cyclic snapshot ready chain");
            ready.push_back(next);
            auto* thread=reinterpret_cast<const OSThread*>(input.rdram.data()+(static_cast<uint32_t>(next)&0x1FFFFFFF));
            check(thread->queue==ultramodern::running_queue,"snapshot ready queue ownership mismatch");
            next=thread->next;
        }
        // Empty the old private ready head using its public scheduler API before
        // replacing RDRAM; never traverse old pointers through the new image.
        while(!ultramodern::thread_queue_empty(rdram,ultramodern::running_queue))
            ultramodern::thread_queue_pop(rdram,ultramodern::running_queue);
        std::memcpy(rdram,input.rdram.data(),input.rdram.size());
        for(const auto& [address,worker]:workers)TO_PTR(OSThread,static_cast<int32_t>(address))->context=worker->native;
        for(auto address:ready)ultramodern::thread_queue_insert(rdram,ultramodern::running_queue,address);
        overlays::PersistentState overlay;
        for(const auto& value:input.overlays)overlay.residents.push_back({value.id,value.header,value.text,value.active,value.generation});
        for(const auto& value:input.overlay_generations)overlay.generations.push_back({value.id,value.generation});
        overlays::persistent_import(rdram,overlay);si::persistent_import(input.si);
        replay_timing::pending_divisor.store(input.replay_pending_divisor);
        replay_timing::published_refresh_rate.store(input.replay_published_refresh_rate);
        scene::reset();if(input.scene_map_available)scene::observe_map(input.scene_map_id);
        frontend::restore_epoch(input.host);
        std::copy(input.rsp_dmem.begin(),input.rsp_dmem.end(),std::begin(dmem));
        check(tooie_persistent_external_import(&input.external_messages),"external message restore failed");
        idle=input.external_wait_thread;
        return true;
    } catch(const std::exception& error) {last_error=error.what();return false;}
    catch(...) {last_error="unknown guest restore failure";return false;}
}
ThreadImage await_restored_image() {
    std::unique_lock lock(mutex);self.phase=Phase::Unwound;cv.notify_all();
    while(!committed) {cv.wait_for(lock,std::chrono::milliseconds{10});stop_check();}
    stop_check();
    auto image=self.restored;
    if(image.phase==ThreadPhase::SchedulerWait) {
        self.phase=self.wake_pending?Phase::DeferredResume:Phase::SchedulerWait;lock.unlock();
        self.native->running.wait();stop_check();
        after_scheduler_wait();
        lock.lock();
    }
    stop_check();self.phase=Phase::Running;return image;
}
bool external_queue_ready() noexcept {
    std::vector<ExternalMessage> messages;
    return tooie_persistent_external_export(&messages);
}
}
