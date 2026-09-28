// Runtime-only prerequisite: no original Tooie boot, idle or main execution.
#include "game.hpp"
#include "context_observer.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/threads.hpp"
#include "ultramodern/ultramodern.hpp"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

extern void init(uint8_t*,recomp_context*,gpr);
extern "C" void func_80013678(uint8_t*,recomp_context*);
extern "C" void func_800124EC(uint8_t*,recomp_context*);
namespace {
constexpr int32_t idle_object=int32_t(0x80600000),main_object=int32_t(0x80600400);
constexpr int32_t idle_entry=int32_t(0x80013678),main_entry=int32_t(0x800124EC);
constexpr int32_t sentinel=int32_t(0x80700000);
struct Worker {
    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exit_armed=false;
    tooie::context_observer::Token token{};
};
std::array<Worker,2> workers;
std::mutex mutex;
std::condition_variable cv;
bool arrived=false,parked=false,release=false,read_grant=false;
bool never_started=false,entry_failure=false,delayed=false;
unsigned main_dispatches=0;
thread_local bool callback_seen=false;
void require(bool c,const char* s) { if(!c)throw std::runtime_error(s); }
void event(const char* name,tooie::Json fields=tooie::Json::object()) {
    fields["original_core1_executed"]=false;
    fields["original_idle_body_executed"]=false;
    fields["original_main_body_executed"]=false;
    fields["main_handoff_acceptance"]=false;
    tooie::trace(name,"priority-prerequisite","observed",0,nullptr,std::move(fields));
}
std::string naming(const OSThread* t) {
    require(t->id==1 || t->id==6,"Unexpected prerequisite worker");
    auto& w=workers[t->id==1?0:1];
    w.token=tooie::context_observer::watch_context(t->context);
    std::unique_lock lock(w.exit_mutex);
    w.exit_armed=true;
    std::notify_all_at_thread_exit(w.exit_cv,std::move(lock));
    return "Priority probe";
}
void callback(uint8_t*,recomp_context* ctx) {
    require(ctx->r31==0 && ctx->status_reg==0 && !ctx->mips3_float_mode && ctx->f_odd==&ctx->f0.u32h,
            "Invalid runtime fresh context");
    callback_seen=true;
}
void main_boundary(uint8_t* rdram,recomp_context*) {
    require(callback_seen && ultramodern::this_thread()==main_object && osGetThreadPri(rdram,0)==20,
            "Main prerequisite bypassed dispatcher");
    auto* idle=TO_PTR(OSThread,idle_object);
    require(ultramodern::thread_queue_peek(rdram,ultramodern::running_queue)==idle_object &&
            idle->next==0 && idle->queue==ultramodern::running_queue && idle->state==OSThreadState::QUEUED &&
            TO_PTR(OSThread,main_object)->queue==0,"Priority transfer did not queue sole idle");
    std::unique_lock lock(mutex);
    ++main_dispatches;
    event("priority_transfer_observed",{{"idle_priority",0},{"main_priority",20},
          {"idle_sole_running_queue_member",true},{"idle_osStartThread_returned",false}});
    if(entry_failure)event("prerequisite_injected_entry_failure");
    arrived=true;cv.notify_all();
    if(delayed) {
        cv.wait(lock,[]{return read_grant;});
        require(MEM_W(0,sentinel)==0x1234abcd && get_function(main_entry)==main_boundary &&
                recomp::current_game_id()==u8"bt.n64.us.1.0","Delayed state prematurely released");
        event("prerequisite_delayed_worker_read",{{"rdram_retained",true},{"mapping_retained",true}});
    }
    parked=true;cv.notify_all();
    cv.wait(lock,[]{return release;});
}
void idle_boundary(uint8_t* rdram,recomp_context*) {
    require(callback_seen && ultramodern::this_thread()==idle_object && osGetThreadPri(rdram,0)==0,
            "Idle prerequisite bypassed dispatcher");
    osCreateThread(rdram,main_object,6,main_entry,0,int32_t(0x80501000),20);
    event("prerequisite_main_created",{{"native_exit_armed_before_create_return",true},
          {"state_stopped",TO_PTR(OSThread,main_object)->state==OSThreadState::STOPPED}});
    if(never_started) {
        std::unique_lock lock(mutex);
        arrived=parked=true;cv.notify_all();
        cv.wait(lock,[]{return release;});
        return;
    }
    osStartThread(rdram,main_object);
    // Expected cleanup throws thread_terminated out of osStartThread. No catch
    // consumes that signal or sends this worker back to a gate that cannot open.
    throw std::runtime_error("Idle unexpectedly resumed after priority transfer");
}
void destroy(uint8_t* rdram,unsigned index) {
    int32_t object=index==0?idle_object:main_object;
    osDestroyThread(rdram,object);
    require(TO_PTR(OSThread,object)->context==nullptr,"Runtime destroy failed");
    event("prerequisite_worker_destroyed",{{"guest_id",index==0?1:6}});
    if((never_started && index==0) || (!never_started && index==1)) {
        std::lock_guard lock(mutex);release=true;cv.notify_all();
    }
    auto& w=workers[index];
    {std::unique_lock lock(w.exit_mutex);w.exit_cv.wait(lock,[&]{return w.exit_armed;});}
    event("prerequisite_native_thread_exited",{{"guest_id",index==0?1:6},
          {"cleanup_already_enqueued",true},{"notification","std::notify_all_at_thread_exit"}});
}
}
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    std::string scenario=argv[3];
    never_started=scenario=="never-started";entry_failure=scenario=="entry-failure";delayed=scenario=="delayed-worker";
    require(never_started || entry_failure || delayed || scenario=="ordinary","Unknown priority prerequisite case");
    tooie::start_trace(argv[2]);tooie::start_watchdog(20);
    auto identity=tooie::validate_and_install_rom(argv[1]);auto game=tooie::game_entry(identity.xxh3);
    game.thread_create_callback=callback;recomp::register_game(game);recomp::start_game(game.game_id,"");
    std::vector<uint8_t> memory(8*1024*1024);uint8_t* rdram=memory.data();recomp_context ctx{};
    tooie::register_overlays();init(rdram,&ctx,(gpr)int32_t(0x80000400));
    MEM_W(0,sentinel)=0x1234abcd;
    recomp::overlays::add_loaded_function(idle_entry,idle_boundary);
    recomp::overlays::add_loaded_function(main_entry,main_boundary);
    ultramodern::threads::set_callbacks({naming});
    ultramodern::init_thread_cleanup();
    osCreateThread(rdram,idle_object,1,idle_entry,0,int32_t(0x80500000),0);
    osStartThread(rdram,idle_object);
    {
        std::unique_lock lock(mutex);cv.wait(lock,[]{return arrived;});
        if(delayed) {
            require(tooie::context_observer::deletion_count(workers[0].token)==0 &&
                    tooie::context_observer::deletion_count(workers[1].token)==0,
                    "Live prerequisite context prematurely disposed");
            event("prerequisite_delayed_release_deferred",{{"rdram_retained",true},
                  {"mapping_retained",get_function(main_entry)==main_boundary},
                  {"active_game_retained",recomp::current_game_id()==game.game_id},{"correctness_sleeps",false}});
            read_grant=true;cv.notify_all();
        }
        cv.wait(lock,[]{return parked;});
    }
    event("prerequisite_creation_set_closed",{{"workers",2},{"scenario",scenario}});
    if(never_started) {
        require(TO_PTR(OSThread,main_object)->state==OSThreadState::STOPPED,"Never-started worker not stopped");
        destroy(rdram,1);destroy(rdram,0);
    } else {
        destroy(rdram,0);
        require(ultramodern::thread_queue_empty(rdram,ultramodern::running_queue),"Sole idle removal failed");
        destroy(rdram,1);
    }
    require(main_dispatches==(never_started?0u:1u),"Unexpected main dispatcher count");
    require(ultramodern::thread_queue_empty(rdram,ultramodern::running_queue),"Runnable references remain");
    event("prerequisite_producers_quiescent",{{"workers",2},{"main_dispatches",main_dispatches}});
    ultramodern::quit();ultramodern::join_thread_cleaner_thread();
    for(auto& w:workers)require(tooie::context_observer::deletion_count(w.token)==1,"Expected exactly one context disposal");
    ultramodern::threads::set_callbacks({});
    recomp::overlays::add_loaded_function(idle_entry,func_80013678);
    recomp::overlays::add_loaded_function(main_entry,func_800124EC);
    require(get_function(idle_entry)==func_80013678 && get_function(main_entry)==func_800124EC,"Mapping restoration failed");
    event("prerequisite_mappings_restored");
    std::vector<uint8_t>().swap(memory);
    event("prerequisite_rdram_released");
    event("priority_prerequisite_pass",{{"scenario",scenario},{"exactly_once_context_deletions",2},
          {"all_native_workers_joined",true},{"salvage_used",false}});
    tooie::stop_watchdog();return 0;
}
