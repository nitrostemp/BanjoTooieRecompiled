#include "main_thread_gate.hpp"
#include "game.hpp"
#include "core1_metadata.hpp"
#include "main_metadata.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/threads.hpp"
#include "ultramodern/ultramodern.hpp"
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void func_80013678(uint8_t*, recomp_context*);
extern "C" void func_800124EC(uint8_t*, recomp_context*);
namespace {
namespace idle = tooie::core1_meta;
namespace meta = tooie::main_meta;
struct Worker {
    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exit_armed=false, created=false;
    UltraThreadContext* context=nullptr;
    tooie::context_observer::Token token{};
};
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    Worker idle_worker, main_worker;
    bool enabled=false, installed=false, cleaned=false, main_arrived=false;
    bool main_parked=false, idle_failed=false, release_main=false, release_idle=false;
    bool callback_seen=false, reached=false, read_grant=false, delayed=false;
    unsigned next_stage=0;
    std::exception_ptr error;
    std::thread::id launcher;
    std::vector<uint8_t> stack, record_tail, odd_tail;
    std::array<uint32_t,6> guards{};
    uint32_t odd_header=0;
} gate;
gpr guest(uint32_t v) { return (gpr)(int32_t)v; }
uint32_t word(uint8_t* rdram,uint32_t a) { return MEM_W(0,guest(a)); }
void require(bool c,const char* s) { if(!c) throw std::runtime_error(s); }
void event(const char* name,tooie::Json extra=tooie::Json::object(),const char* outcome="observed") {
    extra["original_main_body_executed"]=false;
    tooie::trace(name,"main-thread-gate",outcome,0,nullptr,std::move(extra));
}
void remember_error() { if(!gate.error) gate.error=std::current_exception(); }
bool injected(const char* site) {
    const char* value=std::getenv("TOOIE_TEST_MAIN_GATE_FAILURE");
    if(!value || std::string(value)!=site) return false;
    event("main_gate_injected_failure",{{"site",site}},"failure");
    return true;
}
// This supported callback runs before the worker signals initialized. Ownership
// and exit notification are therefore established even for never-started main.
std::string name_worker(const OSThread* thread) noexcept {
    Worker* w=nullptr;
    try {
        std::lock_guard lock(gate.mutex);
        const bool is_idle=thread->id==idle::idle_id;
        require(is_idle || thread->id==meta::main_id,"Unexpected native worker in closed creation set");
        w=is_idle?&gate.idle_worker:&gate.main_worker;
        require(!w->created,"Duplicate native worker creation");
        w->context=thread->context;
        w->created=true;
        w->token=tooie::context_observer::watch_context(w->context);
    } catch(...) {
        std::lock_guard lock(gate.mutex);
        remember_error();
    }
    if(w) {
        std::unique_lock lock(w->exit_mutex);
        w->exit_armed=true;
        std::notify_all_at_thread_exit(w->exit_cv,std::move(lock));
    }
    return "Tooie gate";
}
std::vector<uint8_t> bytes(uint8_t* rdram,uint32_t begin,uint32_t end) {
    std::vector<uint8_t> out(end-begin);
    for(uint32_t i=0;i<out.size();++i)out[i]=MEM_B(i,guest(begin));
    return out;
}
std::array<uint32_t,6> guard_words(uint8_t* rdram) {
    return {word(rdram,meta::main_stack_start-4),word(rdram,meta::main_odd_end),
            word(rdram,meta::queue1-4),word(rdram,meta::queue1+24),
            word(rdram,meta::queue2-4),word(rdram,meta::queue2+24)};
}
bool storage_preserved(uint8_t* rdram) {
    return bytes(rdram,meta::main_stack_start,meta::main_stack_top)==gate.stack &&
           bytes(rdram,meta::main_thread+sizeof(OSThread),meta::main_record_end)==gate.record_tail &&
           bytes(rdram,meta::main_odd_storage+16,meta::main_odd_end)==gate.odd_tail &&
           guard_words(rdram)==gate.guards;
}
void verify_queue(uint8_t* rdram,unsigned number,bool emit=true) {
    uint32_t q=number==1?meta::queue1:meta::queue2;
    uint32_t b=number==1?meta::queue1_buffer:meta::queue2_buffer;
    uint32_t n=number==1?meta::queue1_capacity:meta::queue2_capacity;
    bool valid=word(rdram,q)==0 && word(rdram,q+4)==0 && word(rdram,q+8)==0 &&
               word(rdram,q+12)==0 && word(rdram,q+16)==n && word(rdram,q+20)==b;
    if(emit) event(number==1?"main_queue_1_verified":"main_queue_2_verified",
        {{"queue",tooie::hex32(q)},{"buffer",tooie::hex32(word(rdram,q+20))},
         {"capacity",word(rdram,q+16)},{"wait_queues_null",word(rdram,q)==0&&word(rdram,q+4)==0},
         {"valid_count",word(rdram,q+8)},{"first",word(rdram,q+12)}},valid?"pass":"failure");
    require(valid,"Original queue setup mismatch");
}
bool sole_idle_queue(uint8_t* rdram) {
    auto* t=TO_PTR(OSThread,int32_t(idle::idle_thread));
    auto* m=TO_PTR(OSThread,int32_t(meta::main_thread));
    return ultramodern::thread_queue_peek(rdram,ultramodern::running_queue)==int32_t(idle::idle_thread) &&
           t->next==0 && t->queue==ultramodern::running_queue && t->state==OSThreadState::QUEUED &&
           m->queue==0 && m->state==OSThreadState::QUEUED;
}
void main_boundary(uint8_t* rdram,recomp_context* ctx) {
    std::unique_lock lock(gate.mutex);
    try {
        if(gate.error)std::rethrow_exception(gate.error);
        require(gate.next_stage==9,"Original idle queue/create/start sequence incomplete");
        require(gate.callback_seen && std::this_thread::get_id()!=gate.launcher,
                "Main entry bypassed actual runtime dispatcher");
        require(ultramodern::this_thread()==int32_t(meta::main_thread) &&
                osGetThreadId(rdram,0)==meta::main_id && osGetThreadPri(rdram,0)==meta::main_priority,
                "Wrong main worker identity or priority");
        bool fresh=ctx->r29==guest(meta::main_first_sp) && ctx->r4==meta::main_arg &&
            ctx->r31==guest(idle::first_ra) && ctx->status_reg==idle::first_sr &&
            !ctx->mips3_float_mode && ctx->f_odd==&ctx->f0.u32h;
        bool topology=sole_idle_queue(rdram);
        require(fresh && topology && storage_preserved(rdram),"Main fresh context, queue topology or guards mismatch");
        require(TO_PTR(OSThread,int32_t(meta::main_thread))->context==gate.main_worker.context &&
                TO_PTR(OSThread,int32_t(idle::idle_thread))->context==gate.idle_worker.context,
                "Owned native contexts changed before main entry");
        verify_queue(rdram,1,false);verify_queue(rdram,2,false);
        tooie::capture_guest_state(rdram,ctx);
        tooie::trace("main_thread_entry","main-entry","diagnostic_stop",meta::main_entry,ctx,
            {{"runtime_dispatch_callback_observed",true},{"fresh_context_valid",fresh},
             {"current_thread",tooie::hex32(ultramodern::this_thread())},{"guest_thread_id",osGetThreadId(rdram,0)},
             {"priority",osGetThreadPri(rdram,0)},{"status_register",tooie::hex32(ctx->status_reg)},
             {"fr",0},{"cu1",0},{"odd_register_mapping_valid",true},
             {"idle_sole_running_queue_member",topology},{"original_idle_osStartThread_returned",false},
             {"idle_guest_access_quiescent_until_destroy",true},{"original_main_body_executed",false}});
        require(!injected("main-entry"),"Injected main entry failure");
        gate.reached=true;
    } catch(...) { remember_error(); }
    gate.main_arrived=true;
    gate.cv.notify_all();
    if(gate.delayed) {
        gate.cv.wait(lock,[]{return gate.read_grant;});
        try {
            require(word(rdram,meta::main_odd_storage+12)==1 &&
                    get_function(int32_t(meta::main_entry))==main_boundary &&
                    recomp::current_game_id()==u8"bt.n64.us.1.0","Delayed main lost retained state");
            event("main_gate_delayed_worker_read",{{"rdram_retained",true},{"mapping_retained",true},
                  {"active_game_retained",true},{"correctness_sleeps",false}});
        } catch(...) { remember_error(); }
    }
    gate.main_parked=true;
    gate.cv.notify_all();
    gate.cv.wait(lock,[]{return gate.release_main;});
    // Return only from the outer diagnostic entry, never original main.
}
void destroy_worker(uint8_t* rdram,Worker& w,uint32_t object,bool main_worker) {
    if(!w.created)return;
    osDestroyThread(rdram,int32_t(object));
    require(TO_PTR(OSThread,int32_t(object))->context==nullptr,"External destroy did not clear native context");
    event("main_gate_worker_destroyed",{{"guest_id",main_worker?meta::main_id:idle::idle_id},
          {"native_context_cleared",true},{"external_public_osDestroyThread",true},{"rdram_retained",true}});
    {
        std::lock_guard lock(gate.mutex);
        if(main_worker)gate.release_main=true;else gate.release_idle=true;
        gate.cv.notify_all();
    }
    // The exit mutex was transferred to notify_all_at_thread_exit during native
    // initialization. Taking it now proves return past runtime cleanup enqueue.
    {
        std::unique_lock lock(w.exit_mutex);
        w.exit_cv.wait(lock,[&]{return w.exit_armed;});
    }
    event("main_gate_native_thread_exited",{{"guest_id",main_worker?meta::main_id:idle::idle_id},
          {"notification","std::notify_all_at_thread_exit"},{"cleanup_already_enqueued",true},
          {"rdram_retained",true}});
}
void close_gate(uint8_t* rdram) {
    if(!gate.enabled || gate.cleaned)return;
    bool main_arrived=false;
    if(gate.idle_worker.created) {
        std::unique_lock lock(gate.mutex);
        gate.cv.wait(lock,[]{return gate.main_arrived || gate.idle_failed;});
        main_arrived=gate.main_arrived;
        if(main_arrived && gate.delayed) {
            bool retained=get_function(int32_t(meta::main_entry))==main_boundary &&
                recomp::current_game_id()==u8"bt.n64.us.1.0";
            unsigned deleted=tooie::context_observer::deletion_count(gate.idle_worker.token)+
                tooie::context_observer::deletion_count(gate.main_worker.token);
            event("main_gate_delayed_release_deferred",{{"rdram_retained",true},{"mapping_retained",retained},
                  {"active_game_retained",recomp::current_game_id()==u8"bt.n64.us.1.0"},
                  {"context_deletion_count",deleted},{"correctness_sleeps",false}});
            gate.read_grant=true;
            gate.cv.notify_all();
        }
        if(main_arrived)gate.cv.wait(lock,[]{return gate.main_parked;});
        event("main_gate_workers_parked",{{"creation_set_closed",true},{"rdram_retained",true},
              {"main_boundary_parked",main_arrived},{"idle_failure_parked",gate.idle_failed},
              {"idle_suspended_inside_original_osStartThread",main_arrived},
              {"workers_created",gate.main_worker.created?2:1}});
    }
    if(main_arrived) {
        // Never invoke the pinned non-head-removal path: only the sole idle head.
        require(sole_idle_queue(rdram),"Unsafe runtime queue topology at cleanup");
        destroy_worker(rdram,gate.idle_worker,idle::idle_thread,false);
        require(ultramodern::thread_queue_empty(rdram,ultramodern::running_queue),"Idle removal left queued workers");
        destroy_worker(rdram,gate.main_worker,meta::main_thread,true);
    } else {
        // Main's initialization callback armed exit even if run_thread_function
        // was never invoked. No artificial main start is needed for this path.
        if(gate.main_worker.created)require(TO_PTR(OSThread,int32_t(meta::main_thread))->state==OSThreadState::STOPPED,
                                          "Unstarted main is not stopped");
        destroy_worker(rdram,gate.main_worker,meta::main_thread,true);
        destroy_worker(rdram,gate.idle_worker,idle::idle_thread,false);
    }
    bool empty=ultramodern::thread_queue_empty(rdram,ultramodern::running_queue);
    require(empty,"Runtime running queue not empty after native exits");
    unsigned count=unsigned(gate.idle_worker.created)+unsigned(gate.main_worker.created);
    event("main_gate_producers_quiescent",{{"workers",count},{"no_future_producers",true},{"running_queue_empty",empty}});
    ultramodern::quit();
    event("main_gate_quit_after_native_exit",{{"active_game_retained_through_native_exit",true}});
    ultramodern::join_thread_cleaner_thread();
    unsigned d0=gate.idle_worker.created?tooie::context_observer::deletion_count(gate.idle_worker.token):0;
    unsigned d1=gate.main_worker.created?tooie::context_observer::deletion_count(gate.main_worker.token):0;
    bool disposed=d0==unsigned(gate.idle_worker.created) && d1==unsigned(gate.main_worker.created);
    event("main_gate_cleanup_complete",{{"context_deletion_count",d0+d1},{"per_worker_deletions",{d0,d1}},
          {"workers_created",count},{"all_native_workers_joined",disposed},{"salvage_used",false},
          {"rdram_retained",true}},disposed?"pass":"failure");
    require(disposed,"Each created worker must be disposed exactly once");
    ultramodern::threads::set_callbacks({});
    if(gate.installed) {
        recomp::overlays::add_loaded_function(int32_t(meta::main_entry),func_800124EC);
        bool restored=get_function(int32_t(meta::main_entry))==func_800124EC;
        event("main_mapping_restored",{{"real_entry_mapping_verified",restored}},restored?"pass":"failure");
        require(restored,"Main mapping restoration failed");
        gate.installed=false;
    }
    gate.cleaned=true;
    gate.enabled=false;
}
}

namespace tooie {
void prepare_main_thread_gate(uint8_t*) {
    require(!gate.enabled && !gate.cleaned,"Main gate is single-use per process");
    // The caller already started the cleaner. Own its cleanup even if the
    // following registry validation fails before any worker can be created.
    gate.enabled=true;
    require(get_function(int32_t(meta::main_entry))==func_800124EC,"Original main mapping missing");
    gate.launcher=std::this_thread::get_id();
    gate.delayed=std::getenv("TOOIE_TEST_MAIN_GATE_DELAY")!=nullptr;
    ultramodern::threads::set_callbacks({name_worker});
    recomp::overlays::add_loaded_function(int32_t(meta::main_entry),main_boundary);
    gate.installed=true;
    require(get_function(int32_t(meta::main_entry))==main_boundary,"Main diagnostic mapping missing");
    event("main_gate_prepared",{{"main_entry",hex32(meta::main_entry)},
          {"native_exit_callback_installed_before_creation",true}},"pass");
}
context_observer::Token main_gate_context_token(int id) {
    std::lock_guard lock(gate.mutex);
    require(id==idle::idle_id || id==meta::main_id,"Unknown owned worker");
    Worker& w=id==idle::idle_id?gate.idle_worker:gate.main_worker;
    require(w.created,"Native context ownership not established");
    return w.token;
}
void main_thread_create_callback(uint8_t* rdram,recomp_context* ctx) {
    std::lock_guard lock(gate.mutex);
    try {
        require(ultramodern::this_thread()==int32_t(meta::main_thread) && osGetThreadId(rdram,0)==meta::main_id,
                "Unexpected main dispatcher callback identity");
        require(ctx->r29==guest(meta::main_first_sp) && ctx->r4==meta::main_arg && ctx->r31==0 &&
                ctx->status_reg==0 && !ctx->mips3_float_mode && ctx->f_odd==&ctx->f0.u32h,
                "Unexpected fresh native main context");
        ctx->r31=guest(idle::first_ra);
        ctx->status_reg=idle::first_sr;
        gate.callback_seen=true;
    } catch(...) { remember_error(); }
}
void run_original_idle(uint8_t* rdram,recomp_context* ctx,std::exception_ptr initial_error) {
    try {
        if(initial_error)std::rethrow_exception(initial_error);
        { std::lock_guard lock(gate.mutex);if(gate.error)std::rethrow_exception(gate.error); }
        func_80013678(rdram,ctx);
        throw std::runtime_error("Original idle unexpectedly returned before external destruction");
    } catch(ultramodern::thread_terminated&) { throw; }
      catch(...) {
        std::unique_lock lock(gate.mutex);
        remember_error();
        gate.idle_failed=true;
        gate.cv.notify_all();
        gate.cv.wait(lock,[]{return gate.release_idle;});
        // Only this outer wrapper returns. An inner hook must never resume the
        // original body after a failure or it could start an already-dead main.
    }
}
void finish_main_thread_gate(uint8_t* rdram) {
    close_gate(rdram);
    if(gate.error)std::rethrow_exception(gate.error);
    require(gate.reached,"Main entry not reached");
    event("main_handoff_complete",{{"bounded_priority_transfer",true},{"all_native_workers_joined",true},
          {"rdram_retained_until_cleanup",true}},"pass");
}
void cleanup_main_thread_gate(uint8_t* rdram) { close_gate(rdram); }
bool main_gate_memory_safe() { return (!gate.idle_worker.created && !gate.main_worker.created) || gate.cleaned; }
}

extern "C" bool tooie_main_hooks_active() { return gate.enabled; }
extern "C" void tooie_main_hook(uint8_t* rdram,recomp_context* ctx,unsigned stage) {
    if(!gate.enabled)return;
    require(stage==gate.next_stage++,"Original main hook order mismatch");
    require(ultramodern::this_thread()==int32_t(idle::idle_thread) && osGetThreadId(rdram,0)==idle::idle_id,
            "Original main setup must run on guest idle");
    if(stage==0) {
        gate.stack=bytes(rdram,meta::main_stack_start,meta::main_stack_top);
        gate.record_tail=bytes(rdram,meta::main_thread+sizeof(OSThread),meta::main_record_end);
        gate.odd_tail=bytes(rdram,meta::main_odd_storage+16,meta::main_odd_end);
        gate.guards=guard_words(rdram);gate.odd_header=word(rdram,meta::main_odd_storage);
        event("original_idle_body_entered",{{"original_idle_body_executed",true},{"guest_id",idle::idle_id}},"entered");
    } else if(stage==1 || stage==2) {
        verify_queue(rdram,stage);
    } else if(stage==3) {
        require(ctx->r4==meta::pi_priority && ctx->r5==guest(meta::queue2) && ctx->r6==guest(meta::queue2_buffer),
                "Original PI manager call arguments mismatch before delay slot");
    } else if(stage==4) {
        require(ctx->r7==meta::queue2_capacity,"Original PI manager capacity delay slot mismatch");
        verify_queue(rdram,1,false);verify_queue(rdram,2,false);
        event("main_pi_manager_boundary",{{"priority",meta::pi_priority},{"queue",tooie::hex32(meta::queue2)},
              {"buffer",tooie::hex32(meta::queue2_buffer)},{"capacity",meta::queue2_capacity},
              {"native_import_empty",true},{"original_queue_setup_preserved",true},
              {"dma_completion_claimed",false},{"full_pi_initialization_claimed",false}},"pass");
    } else if(stage==5) {
        require(ctx->r4==guest(meta::main_thread) && ctx->r5==meta::main_id &&
                ctx->r6==guest(meta::main_entry) && ctx->r7==meta::main_arg &&
                word(rdram,uint32_t(ctx->r29)+16)==meta::main_stack_top &&
                ctx->r15==meta::main_priority && ctx->r16==guest(meta::main_odd_storage),
                "Original second Rare wrapper create arguments mismatch");
        require(!gate.main_worker.created,"Unexpected duplicate main creation");
        event("main_create_arguments",{{"thread_object",tooie::hex32(meta::main_thread)},
              {"entry",tooie::hex32(meta::main_entry)},{"guest_id",meta::main_id},{"priority",meta::main_priority},
              {"argument",meta::main_arg},{"stack_start",tooie::hex32(meta::main_stack_start)},
              {"stack_top",tooie::hex32(meta::main_stack_top)},{"stack_size",meta::main_stack_size}},"pass");
    } else if(stage==6) {
        auto* t=TO_PTR(OSThread,int32_t(meta::main_thread));
        require(gate.main_worker.created && t->context==gate.main_worker.context &&
                t->id==meta::main_id && t->priority==meta::main_priority &&
                t->state==OSThreadState::STOPPED && t->queue==0 && t->next==0 &&
                t->sp==int32_t(meta::main_first_sp),"Native main creation state mismatch");
        event("main_thread_created",{{"guest_id",t->id},{"priority",t->priority},
              {"runtime_context_created",true},{"initial_state_stopped",true},
              {"native_exit_armed_before_create_return",true}},"pass");
        require(!injected("after-main-create"),"Injected failure after main creation before start");
    } else if(stage==7) {
        auto* t=TO_PTR(OSThread,int32_t(meta::main_thread));
        tooie::Json flags={word(rdram,meta::main_odd_storage+4),word(rdram,meta::main_odd_storage+8),
                          word(rdram,meta::main_odd_storage+12)};
        bool preserved=storage_preserved(rdram);
        bool pointer=t->context==gate.main_worker.context;
        uint32_t table=word(rdram,idle::thread_table+meta::main_id*4);
        require(pointer && preserved && word(rdram,meta::main_thread+0x1c)==meta::main_odd_storage &&
                table==meta::main_thread && flags==tooie::Json({0,0,1}) &&
                word(rdram,meta::main_odd_storage)==gate.odd_header && t->sp==int32_t(meta::main_first_sp),
                "Main Rare bookkeeping, native storage or guards mismatch");
        event("main_rare_bookkeeping_verified",{{"native_context_preserved",pointer},{"odd_storage_flags",flags},
              {"odd_storage_header",gate.odd_header},{"rare_id_table",tooie::hex32(table)},
              {"stack_and_guards_preserved",preserved},{"original_generated_wrapper_preserved",true}},"pass");
    } else if(stage==8) {
        require(ctx->r2==guest(meta::main_thread) && osGetThreadPri(rdram,0)==idle::idle_priority,
                "Original idle main-start target or caller priority mismatch");
        event("main_start_from_original_idle",{{"caller_id",osGetThreadId(rdram,0)},
              {"caller_priority",osGetThreadPri(rdram,0)},{"target",tooie::hex32(uint32_t(ctx->r2))},
              {"target_id",meta::main_id},{"target_priority",meta::main_priority},
              {"host_manual_main_start",false}},"pass");
    } else {
        event("main_start_returned_forbidden",{},"failure");
        throw std::runtime_error("Original idle osStartThread unexpectedly returned");
    }
}
