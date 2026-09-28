#include "persistent_state_runtime.hpp"
#include "persistent_state_continuation.hpp"
#include "persistent_continuation_registry.hpp"
#include "persistent_state_hle.hpp"
#include "persistent_state_idle.hpp"
#include "persistent_state_scheduler.hpp"
#include "persistent_state_devices.hpp"
#include "continuous_host.hpp"
#include "replay_timing.hpp"
#include "tooie_overlays.hpp"
#include "trace.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace tooie::persistent_state::runtime {
namespace {
thread_local std::unique_ptr<continuation::Machine> machine;
thread_local std::uint32_t guest_thread_address=0;
const std::vector<continuation::Function>& runtime_functions() {
    static const auto registry=[] {
        auto source=continuation::generated::functions();
        std::vector<continuation::Function> result(source.begin(),source.end());
        auto hle=hle::functions();result.insert(result.end(),hle.begin(),hle.end());
        auto overlay=overlays::persistent_functions();result.insert(result.end(),overlay.begin(),overlay.end());
        auto idle=idle::functions();result.insert(result.end(),idle.begin(),idle.end());
        return result;
    }();
    return registry;
}
}
void attach_worker(std::uint8_t* rdram,recomp_context* context,std::uint32_t guest_thread) {
    // Normal sessions retain original HLE paths and pay no frame-stack cost.
    if(!devices::experiment_enabled())return;
    if(machine)throw std::runtime_error("Persistent runtime worker attached twice");
    auto next=std::make_unique<continuation::Machine>(runtime_functions(),true);
    next->bind_tracking(rdram,context);
    guest_thread_address=guest_thread;
    machine=std::move(next);
    scheduler::worker_attached(machine.get(),context);
    trace("persistent_runtime_worker_attached","experimental","live_continuations",0,context,
        {{"guest_thread",hex32(guest_thread)},{"program",continuation::generated::program_sha256()},
         {"engineering_checkpoint_protocol",true},{"private_practice_session",true}});
}
void detach_worker() noexcept {
    scheduler::worker_finished();
    if(!machine)return;
    machine->unbind_tracking();
    // Lifecycle stop unwinds native guest/HLE frames. A nonempty logical stack
    // is expected then; it is evidence, never a resumable game checkpoint.
    try {
        trace("persistent_runtime_worker_detached","experimental","live_continuations",0,nullptr,
            {{"guest_thread",hex32(guest_thread_address)},{"entries",machine->entries},
             {"maximum_depth",machine->maximum_depth},{"remaining_frames",machine->frames.size()},
             {"native_dependency_admissions",machine->native_dependencies},
             {"engineering_checkpoint_protocol",true},{"private_practice_session",true}});
        std::fprintf(stderr,"Persistent live continuations: thread=%08x entries=%llu depth=%zu remaining=%zu engineering_checkpoints=guarded\n",
            guest_thread_address,static_cast<unsigned long long>(machine->entries),machine->maximum_depth,machine->frames.size());
    } catch(...) {}
    machine.reset();guest_thread_address=0;
}
void run_worker(std::uint8_t* rdram,std::uint64_t entry,std::uint64_t sp,std::uint64_t arg) {
    recomp_context context{};context.r29=sp;context.r4=arg;
    context.mips3_float_mode=0;context.f_odd=&context.f0.u32h;
    try {
    continuous_thread_create_callback(rdram,&context);
    bool restoring=false;
    for(;;) {
        try {
            if(!restoring)get_function(static_cast<int32_t>(entry))(rdram,&context);
            else {
                if(!machine)throw std::runtime_error("Restore entered outside a private practice worker");
                const auto restored=scheduler::await_restored_image();
                continuous_poll();
                replay_timing::active_divisor=restored.replay_active_divisor;
                set_cop1_cs(restored.cop1_rounding_mode);
                overlays::persistent_restore_callsites(restored.overlay_callsites);
                machine->resume_borrowed(restored.frames,restored.context);
            }
            scheduler::worker_context_leaving();return;
        } catch(const scheduler::RestoreUnwind&) {restoring=true;}
    }
    } catch(...) {scheduler::worker_context_leaving();throw;}
}
}
