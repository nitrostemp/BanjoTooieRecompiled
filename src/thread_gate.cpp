#include "thread_gate.hpp"
#include "main_thread_gate.hpp"
#include "game.hpp"
#include "context_observer.hpp"
#include "core1_bridge.hpp"
#include "core1_metadata.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void func_80013678(uint8_t*, recomp_context*);
namespace {
namespace meta = tooie::core1_meta;
using tooie::Json;
static_assert(sizeof(void*) == 8 && sizeof(OSThread) == 0x30);
static_assert(offsetof(OSThread, context) == 0x20 && offsetof(OSThread, sp) == 0x28);
static_assert(offsetof(OSThread, id) == 0x14 && offsetof(OSThread, pad3) == 0x18);
// Rare's 32-bit odd-save word at +0x1c occupies native alignment padding.
static_assert(offsetof(OSThread, pad3) + sizeof(OSThread::pad3) == 0x1c);
struct Gate {
    std::mutex mutex, exit_mutex;
    std::condition_variable cv, exit_cv;
    bool enabled=false, installed=false, cleaner=false, created=false, started=false;
    bool main_mode=false, launcher_finished=false;
    bool arrived=false, observe=false, parked=false, release=false, native_exit=false;
    bool cleaned=false, reached=false, callback_seen=false;
    unsigned next_stage=0;
    std::thread::id launcher;
    UltraThreadContext* context=nullptr;
    tooie::context_observer::Token token{};
    std::exception_ptr worker_error;
    uint32_t end_guard=0, before_guard=0, initial_status=0;
    uint64_t initial_clock=0;
    std::vector<uint8_t> image;
} gate;
constexpr uint32_t image_start=meta::core1_entry;
gpr guest(uint32_t value) { return (gpr)(int32_t)value; }
uint32_t word(uint8_t* rdram,uint32_t address) { return MEM_W(0,guest(address)); }
uint64_t doubleword(uint8_t* rdram,uint32_t address) {
    return (uint64_t(word(rdram,address))<<32)|word(rdram,address+4);
}
void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool image_matches(uint8_t* rdram,bool after_init) {
    for (uint32_t i=0;i<gate.image.size();++i) {
        uint32_t address=image_start+i;
        if (after_init && ((address>=meta::clock_rate && address<meta::clock_rate+8) ||
                           (address>=meta::vi_clock && address<meta::vi_clock+4))) continue;
        if (uint8_t(MEM_B(0,guest(address)))!=gate.image[i]) return false;
    }
    return true;
}
bool guards_match(uint8_t* rdram) {
    return word(rdram,meta::bss_start-4)==gate.before_guard && word(rdram,meta::bss_end)==gate.end_guard;
}
bool injected(const char* site) {
    const char* requested=std::getenv("TOOIE_TEST_THREAD_GATE_FAILURE");
    if (!requested || std::string(requested)!=site) return false;
    tooie::trace("thread_gate_injected_failure","test-only","failure",0,nullptr,{{"site",site}});
    return true;
}
void idle_boundary(uint8_t* rdram,recomp_context* ctx) {
    // Do not read shared guest memory while generated launcher frames unwind.
    // The callback only inspects its own freshly created register context.
    std::unique_lock lock(gate.mutex);
    gate.arrived=true;
    gate.cv.notify_all();
    gate.cv.wait(lock,[]{return gate.observe;});
    try {
        if (gate.worker_error) std::rethrow_exception(gate.worker_error);
        require(!injected("worker-entry"),"Injected worker entry failure");
        require(gate.callback_seen && std::this_thread::get_id()!=gate.launcher,
                "Idle entry did not use the actual runtime dispatcher");
        require(ultramodern::this_thread()==int32_t(meta::idle_thread) && osGetThreadId(rdram,0)==meta::idle_id,
                "Wrong idle guest identity");
        require(osGetThreadPri(rdram,0)==meta::idle_priority && ctx->r29==guest(meta::first_sp) &&
                ctx->r4==meta::idle_arg && ctx->r31==guest(meta::first_ra) && ctx->status_reg==meta::first_sr &&
                !ctx->mips3_float_mode && ctx->f_odd==&ctx->f0.u32h,"Wrong initial idle context");
        require(TO_PTR(OSThread,int32_t(meta::idle_thread))->context==gate.context,"Idle native context changed");
        require(image_matches(rdram,true) && guards_match(rdram),"Core image or BSS guards changed before idle entry");
        tooie::capture_guest_state(rdram,ctx);
        tooie::trace("idle_thread_entry","F-entry","diagnostic_stop",meta::idle_entry,ctx,
            {{"runtime_dispatch_callback_observed",true},{"current_thread",tooie::hex32(ultramodern::this_thread())},
             {"guest_thread_id",osGetThreadId(rdram,0)},{"priority",osGetThreadPri(rdram,0)},
             {"status_register",tooie::hex32(ctx->status_reg)},{"fr",0},{"cu1",0},
             {"odd_register_mapping_valid",true},{"original_func_80012030_executed",true},
             {"original_idle_body_executed",false},{"core1_image_preserved_except_initialized_data",true}});
        gate.reached=true;
    } catch (...) { gate.worker_error=std::current_exception(); }
    if (gate.main_mode) {
        auto error=gate.worker_error;
        lock.unlock();
        tooie::run_original_idle(rdram,ctx,error);
        return;
    }
    gate.parked=true;
    gate.cv.notify_all();
    gate.cv.wait(lock,[]{return gate.release;});
    lock.unlock();
    // Lock release/notification happens at native exit, after _thread_func's
    // cleanup enqueue. The controller cannot mistake boundary return for exit.
    std::unique_lock exit_lock(gate.exit_mutex);
    gate.native_exit=true;
    std::notify_all_at_thread_exit(gate.exit_cv,std::move(exit_lock));
}

void close_gate(uint8_t* rdram) {
    if (!gate.enabled || gate.cleaned) return;
    if (gate.main_mode) {
        // The main helper owns both native contexts and the cleaner in this mode.
        // On launcher failure allow only an aborting idle wrapper to run.
        {
            std::lock_guard lock(gate.mutex);
            if (!gate.launcher_finished && !gate.worker_error)
                gate.worker_error=std::make_exception_ptr(std::runtime_error("Launcher aborted before idle progression"));
            gate.observe=true;
            gate.cv.notify_all();
        }
        if (gate.created && !gate.started) {
            osStartThread(rdram,int32_t(meta::idle_thread));
            gate.started=true;
        }
        tooie::cleanup_main_thread_gate(rdram);
        gate.cleaned=tooie::main_gate_memory_safe();
        require(gate.cleaned,"Two-worker cleanup incomplete");
        gate.cleaner=false;
        if (gate.installed) {
            recomp::overlays::add_loaded_function(int32_t(meta::idle_entry),func_80013678);
            gate.installed=false;
            bool restored=get_function(int32_t(meta::idle_entry))==func_80013678;
            tooie::trace("idle_mapping_restored","F-stop",restored?"pass":"failure",meta::idle_entry,nullptr,{{"real_entry_mapping_verified",restored}});
            require(restored,"Idle native mapping restoration failed");
        }
        gate.enabled=false;
        return;
    }
    if (gate.created) {
        // If a launcher validation failed before original osStartThread, release
        // the already-created native worker only into our diagnostic boundary.
        if (!gate.started) {
            tooie::trace("idle_cleanup_start","F-stop","observed",meta::idle_entry,nullptr,
                         {{"reason","launcher failed before original start"},{"original_idle_body_executed",false}});
            osStartThread(rdram,int32_t(meta::idle_thread));
            gate.started=true;
        }
        {
            std::unique_lock lock(gate.mutex);
            gate.observe=true;
            gate.cv.notify_all();
            gate.cv.wait(lock,[]{return gate.parked;});
            tooie::trace("idle_worker_parked","F-stop","observed",meta::idle_entry,nullptr,
                         {{"rdram_retained",true},{"worker_released",false},{"native_exit",false}});
            // No worker guest access can occur while this handshake lock is held.
            osDestroyThread(rdram,int32_t(meta::idle_thread));
            bool detached=TO_PTR(OSThread,int32_t(meta::idle_thread))->context==nullptr;
            gate.release=true;
            gate.cv.notify_all();
            tooie::trace("idle_destroyed_while_parked","F-stop",detached?"pass":"failure",meta::idle_entry,nullptr,
                         {{"native_context_cleared",detached},{"external_public_osDestroyThread",true},
                          {"rdram_retained",true},{"boundary_release_granted",true}});
        }
        {
            std::unique_lock lock(gate.exit_mutex);
            gate.exit_cv.wait(lock,[]{return gate.native_exit;});
        }
        tooie::trace("idle_native_thread_exited","F-stop","observed",meta::idle_entry,nullptr,
            {{"notification","std::notify_all_at_thread_exit"},{"cleanup_already_enqueued",true},
             {"no_future_producers",true},{"launcher_guest_execution_finished",true},{"rdram_retained",true}});
    }
    tooie::trace("idle_cleanup_producers_quiescent","F-stop","observed",0,nullptr,
                {{"workers",gate.created?1:0},{"no_future_producers",true}});
    ultramodern::quit();
    tooie::trace("idle_quit_after_native_exit","F-stop","observed",0,nullptr,
                {{"active_game_retained_through_native_exit",true}});
    if (gate.cleaner) { ultramodern::join_thread_cleaner_thread(); gate.cleaner=false; }
    // Memory/mappings become releasable only after exactly-once context disposal,
    // not merely after the cleaner join returns. The observer never reads freed storage.
    unsigned deletes=gate.created?tooie::context_observer::deletion_count(gate.token):0;
    gate.cleaned=deletes==(gate.created?1u:0u);
    tooie::trace("idle_cleanup_complete","F-stop",deletes==(gate.created?1u:0u)?"pass":"failure",0,nullptr,
        {{"context_deletion_count",deletes},{"all_native_workers_joined",deletes==(gate.created?1u:0u)},
         {"workers_created",gate.created?1:0},{"salvage_used",false},{"rdram_retained",true}});
    require(gate.cleaned,"Idle native context was not deleted exactly once");
    if (gate.installed) {
        recomp::overlays::add_loaded_function(int32_t(meta::idle_entry),func_80013678);
        gate.installed=false;
        bool restored=get_function(int32_t(meta::idle_entry))==func_80013678;
        tooie::trace("idle_mapping_restored","F-stop",restored?"pass":"failure",meta::idle_entry,nullptr,
                    {{"real_entry_mapping_verified",restored}});
        require(restored,"Idle native mapping restoration failed");
    }
    gate.enabled=false;
    require(deletes==(gate.created?1u:0u),"Idle native context was not deleted exactly once");
}
}

namespace tooie {
bool thread_gate_enabled() { return gate.enabled; }
bool thread_gate_reached() { return gate.reached; }
bool thread_gate_memory_safe() { return gate.main_mode?main_gate_memory_safe():(!gate.created || gate.cleaned); }
void prepare_thread_gate(uint8_t* rdram,bool main) {
    require(!gate.enabled && !gate.created,"Thread gate is single-use per process");
    require(get_function(int32_t(meta::idle_entry))==func_80013678,"Original idle registry mapping missing");
    gate.launcher=std::this_thread::get_id();
    gate.main_mode=main;
    gate.image.resize(meta::bss_start-image_start);
    for (uint32_t i=0;i<gate.image.size();++i) gate.image[i]=MEM_B(i,guest(image_start));
    gate.before_guard=word(rdram,meta::bss_start-4);
    gate.end_guard=0x39C6A571;
    MEM_W(0,guest(meta::bss_end))=gate.end_guard;
    for (uint32_t address=meta::bss_start;address<meta::bss_end;++address) MEM_B(0,guest(address))=0xA5;
    gate.initial_clock=doubleword(rdram,meta::clock_rate);
    gate.enabled=true;
    recomp::start_game(u8"bt.n64.us.1.0","");
    require(recomp::current_game_id()==u8"bt.n64.us.1.0","Active Tooie game missing");
    ultramodern::init_thread_cleanup();
    gate.cleaner=true;
    if(main)prepare_main_thread_gate(rdram);
    recomp::overlays::add_loaded_function(int32_t(meta::idle_entry),idle_boundary);
    gate.installed=true;
    require(get_function(int32_t(meta::idle_entry))==idle_boundary,"Idle diagnostic mapping installation failed");
    trace("idle_gate_prepared","E","pass",meta::idle_entry,nullptr,
        {{"active_game_public_api",true},{"cleaner_started_before_worker",true},
         {"bss_poisoned_bytes",meta::bss_end-meta::bss_start},{"diagnostic_boundary_installed",true}});
}
void thread_create_callback(uint8_t* rdram,recomp_context* ctx) {
    if(gate.main_mode && ultramodern::this_thread()!=int32_t(meta::idle_thread)) {
        main_thread_create_callback(rdram,ctx);
        return;
    }
    try {
        require(gate.enabled,"Thread callback outside idle diagnostic");
        require(ctx->r29==guest(meta::first_sp) && ctx->r4==meta::idle_arg && ctx->r31==0 &&
                ctx->status_reg==0 && !ctx->mips3_float_mode && ctx->f_odd==&ctx->f0.u32h,
                "Unexpected native fresh worker context");
        // Only the two derived architectural fields differ from the fresh runtime
        // context. Preserve the runtime's SP/A0 and native FR=0/FPR mapping.
        ctx->r31=guest(meta::first_ra);
        ctx->status_reg=meta::first_sr;
        std::lock_guard lock(gate.mutex);
        gate.callback_seen=true;
    } catch (...) {
        std::lock_guard lock(gate.mutex);
        gate.worker_error=std::current_exception();
    }
}
void finish_thread_gate(uint8_t* rdram) {
    std::exception_ptr error;
    try {
        require(gate.enabled && gate.next_stage==7 && gate.created && gate.started,
                "Original core1/create/start hook sequence incomplete");
        trace("launcher_guest_execution_finished","F-stop","observed",0,nullptr,
              {{"rdram_retained",true},{"original_idle_body_executed",false}});
        if(gate.main_mode) {
            {
                std::lock_guard lock(gate.mutex);
                gate.launcher_finished=true;
                gate.observe=true;
                gate.cv.notify_all();
            }
            finish_main_thread_gate(rdram);
        }
    } catch (...) { error=std::current_exception(); }
    close_gate(rdram);
    if (error) std::rethrow_exception(error);
    if (gate.worker_error) std::rethrow_exception(gate.worker_error);
    require(gate.reached,"Original idle worker entry not reached");
    if(gate.main_mode)return;
    trace("idle_handoff_complete","F-stop","pass",meta::idle_entry,nullptr,
          {{"bounded_first_scheduled_tooie_thread",true},{"original_idle_body_executed",false},
           {"all_native_workers_joined",true},{"rdram_retained_until_cleanup",true}});
}
void cleanup_thread_gate(uint8_t* rdram) { close_gate(rdram); }
}

extern "C" bool tooie_thread_hooks_active() { return tooie::thread_gate_enabled(); }
extern "C" void tooie_thread_hook(uint8_t* rdram,recomp_context* ctx,unsigned stage) {
    if (!gate.enabled) return;
    // Acquire ownership before tracing or validating the post-create hook: those
    // operations may throw, but the native worker is already parked and alive.
    if (stage==4) {
        gate.context=TO_PTR(OSThread,int32_t(meta::idle_thread))->context;
        gate.created=gate.context!=nullptr;
        require(gate.created,"Native osCreateThread did not allocate a context");
        gate.token=gate.main_mode?tooie::main_gate_context_token(meta::idle_id):tooie::context_observer::watch_context(gate.context);
    }
    if (stage==6) gate.started=true;
    require(stage==gate.next_stage++,"Original core1 thread hook order mismatch");
    if (stage<6) tooie::capture_guest_state(rdram,ctx);
    if (stage==0) {
        gate.initial_status=ctx->status_reg;
        tooie::trace("original_core1_entered","E","entered",0x80012030,ctx,
                    {{"original_func_80012030_executed",true},{"original_idle_body_executed",false}});
    } else if (stage==1) {
        bool zero=true;
        for (uint32_t a=meta::bss_start;a<meta::bss_end;++a) zero&=MEM_B(0,guest(a))==0;
        bool guards=guards_match(rdram),image=image_matches(rdram,false);
        tooie::trace("core1_bss_verified","E",zero&&guards&&image?"pass":"failure",0x80012054,ctx,
                    {{"bss_zero",zero},{"bss_bytes",meta::bss_end-meta::bss_start},
                     {"bss_start",tooie::hex32(meta::bss_start)},{"bss_end",tooie::hex32(meta::bss_end)},
                     {"guards_preserved",guards},{"core1_image_preserved",image},
                     {"old_image_end_guard_expected_cleared",true}});
        require(zero&&guards&&image,"Original core1 BSS or boundary validation failed");
    } else if (stage==2) {
        bool vectors=true,nmi=true;
        for (uint32_t dest:{0x80000000u,0x80000080u,0x80000100u,0x80000180u})
            for (unsigned i=0;i<16;++i) vectors&=MEM_B(i,guest(dest))==MEM_B(i,guest(meta::exception_preamble));
        if (word(rdram,0x8000030C)==0) for(unsigned i=0;i<64;++i)nmi&=MEM_B(i,guest(0x8000031C))==0;
        uint32_t tv=word(rdram,0x80000300),vi=tv==0?0x02F5B2D2:tv==2?0x02E6025C:0x02E6D354;
        bool valid=word(rdram,meta::finalrom)==1 && ctx->status_reg==(gate.initial_status|0x20000000u) &&
            doubleword(rdram,meta::clock_rate)==gate.initial_clock*3/4 && word(rdram,meta::vi_clock)==vi &&
            MEM_B(0,guest(meta::pi_dom1_type))==7 && MEM_B(0,guest(meta::pi_dom2_type))==7 && vectors&&nmi &&
            tooie::core1_fcsr_shadow()==0x01000800 && guards_match(rdram) && image_matches(rdram,true);
        tooie::trace("core1_initialization_verified","E",valid?"pass":"failure",0x80012064,ctx,
            {{"guest_effects_validated",valid},{"distinct_core1_initialize",tooie::hex32(meta::os_initialize)},
             {"finalrom_marker",word(rdram,meta::finalrom)},{"clock_hz",doubleword(rdram,meta::clock_rate)},
             {"vi_clock",word(rdram,meta::vi_clock)},{"vectors_valid",vectors},{"cold_nmi_valid",nmi},
             {"fcsr_shadow",tooie::hex32(tooie::core1_fcsr_shadow())},{"hardware_effects_deferred",true}});
        require(valid,"Core1 initialization guest effects mismatch");
    } else if (stage==3) {
        // Hooks run before JAL's delay slot: priority still lives in t7, and the
        // Rare odd-storage argument has moved into s0 before the native call.
        require(ctx->r4==guest(meta::idle_thread)&&ctx->r5==meta::idle_id&&ctx->r6==guest(meta::idle_entry)&&
                ctx->r7==meta::idle_arg&&word(rdram,uint32_t(ctx->r29)+0x10)==meta::idle_stack_top&&
                ctx->r15==meta::idle_priority&&ctx->r16==guest(meta::idle_odd_storage),
                "Original Rare native create arguments mismatch");
        require(!gate.created,"Unexpected additional worker creation");
        tooie::trace("idle_create_arguments","F-create","pass",0x8001DCE4,ctx,
            {{"thread_object",tooie::hex32(meta::idle_thread)},{"entry",tooie::hex32(meta::idle_entry)},
             {"guest_id",meta::idle_id},{"priority",meta::idle_priority},{"argument",meta::idle_arg},
             {"stack_start",tooie::hex32(meta::idle_stack_start)},{"stack_top",tooie::hex32(meta::idle_stack_top)}});
    } else if (stage==4) {
        auto* thread=TO_PTR(OSThread,int32_t(meta::idle_thread));
        bool valid=thread->id==meta::idle_id&&thread->priority==meta::idle_priority&&
            thread->sp==int32_t(meta::first_sp)&&thread->state==OSThreadState::STOPPED&&thread->queue==0;
        tooie::trace("idle_thread_created","F-create",valid?"pass":"failure",0x8001DCEC,ctx,
            {{"runtime_context_created",true},{"guest_id",thread->id},{"priority",thread->priority},
             {"initial_state_stopped",thread->state==OSThreadState::STOPPED},
             {"native_context_offset",offsetof(OSThread,context)},{"native_sp_offset",offsetof(OSThread,sp)}});
        require(valid,"Native created thread fields mismatch");
        require(!injected("after-create"),"Injected failure after native create");
    } else if (stage==5) {
        auto* thread=TO_PTR(OSThread,int32_t(meta::idle_thread));
        Json flags={word(rdram,meta::idle_odd_storage+4),word(rdram,meta::idle_odd_storage+8),word(rdram,meta::idle_odd_storage+12)};
        bool pointer=thread->context==gate.context;
        uint32_t table=word(rdram,meta::thread_table+meta::idle_id*4),header=word(rdram,meta::idle_odd_storage);
        bool valid=pointer&&word(rdram,meta::idle_thread+0x1c)==meta::idle_odd_storage&&
            table==meta::idle_thread&&flags==Json({0,0,0})&&header==0&&thread->sp==int32_t(meta::first_sp);
        tooie::trace("idle_rare_bookkeeping_verified","F-create",valid?"pass":"failure",0x80013660,ctx,
            {{"native_context_preserved",pointer},{"odd_storage",tooie::hex32(word(rdram,meta::idle_thread+0x1c))},
             {"odd_storage_header",header},{"odd_storage_flags",flags},{"rare_id_table",tooie::hex32(table)},
             {"original_generated_wrapper_preserved",true}});
        require(valid,"Original Rare bookkeeping damaged native thread state");
    } else if (stage==6) {
        gate.started=true;
        tooie::trace("idle_thread_started","F-start","pass",0x80013668,ctx,
            {{"original_osStartThread_returned",true},{"native_resume_invoked",true},{"guest_id",meta::idle_id}});
    }
}
