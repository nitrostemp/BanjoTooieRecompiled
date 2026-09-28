#include "persistent_state_idle.hpp"
#include "persistent_state_scheduler.hpp"
#include "ultramodern/ultramodern.hpp"
extern "C" void pause_self(uint8_t*);
namespace {
constexpr uint32_t idle_id=0xF0000007;
constexpr uint32_t external_pc=1,scheduler_pc=2;
}
extern "C" void tooie_persistent_pause(uint8_t* rdram,recomp_context*) {
    auto* machine=tooie::continuation::Machine::current_if_bound();
    if(!machine){pause_self(rdram);return;}
    machine->enter(idle_id);
    for(;;) {
        machine->top().pc=external_pc;
        // Park before dequeue, never while a dequeued message lives only in a
        // native local. The restored external FIFO owns every pending message.
        tooie::persistent_state::scheduler::before_external_wait();
        ultramodern::wait_for_external_message(rdram);
        tooie::persistent_state::scheduler::after_external_wait();
        machine->top().pc=scheduler_pc;
        ultramodern::check_running_queue(rdram);
    }
}
namespace tooie::persistent_state::idle {
std::span<const continuation::Function> functions() {
    static constexpr uint32_t pcs[]={external_pc,scheduler_pc};
    static const continuation::Function functions[]={{idle_id,tooie_persistent_pause,pcs}};
    return functions;
}
}
