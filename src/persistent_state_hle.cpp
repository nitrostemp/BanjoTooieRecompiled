#include "persistent_state_hle.hpp"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include <stdexcept>

// These are explicit equivalents of the pinned runtime's message paths, using
// its real queues/scheduler. A logical frame survives the scheduler's native
// wait. No file capture is enabled by these adapters on their own.
void dequeue_external_messages(uint8_t* rdram);
namespace {
using tooie::continuation::Machine;
constexpr uint32_t recv_id=0xF0000001,send_id=0xF0000002,jam_id=0xF0000003;
constexpr uint32_t start_id=0xF0000004,stop_id=0xF0000005,priority_id=0xF0000006;
constexpr uint32_t wait_pc=1,return_pc=2;
constexpr uint32_t message_pcs[]={wait_pc,return_pc};
constexpr uint32_t thread_pcs[]={return_pc};

void message(uint8_t* rdram,recomp_context* ctx,uint32_t id,bool receive,bool jam) {
    auto& machine=Machine::current();
    const auto initial_pc=machine.enter(id).pc;
    if(!initial_pc) {
        machine.top().operands={ctx->r4,ctx->r5,ctx->r6,0};
        // This may put threads on the runnable queue but cannot context switch.
        dequeue_external_messages(rdram);
    }
    auto operands=machine.top().operands;
    const auto mq_address=static_cast<int32_t>(operands[0]);
    const auto operand=static_cast<int32_t>(operands[1]);
    const bool block=static_cast<int32_t>(operands[2])==OS_MESG_BLOCK;
    auto* mq=TO_PTR(OSMesgQueue,mq_address);
    if(initial_pc!=return_pc) {
        if(!ultramodern::is_game_thread())throw std::runtime_error("Persistent message adapter requires guest worker");
        // Resuming wait_pc means the native scheduler already resumed this
        // worker; do not insert its saved blocked link a second time.
        while(receive?mq->validCount==0:mq->validCount>=mq->msgCount) {
            if(!block) {
                machine.top().operands[3]=static_cast<uint64_t>(-1);
                goto completed;
            }
            const auto queue=receive?GET_MEMBER(OSMesgQueue,mq_address,blocked_on_recv):GET_MEMBER(OSMesgQueue,mq_address,blocked_on_send);
            ultramodern::thread_queue_insert(rdram,queue,ultramodern::this_thread());
            machine.top().pc=wait_pc;
            ultramodern::run_next_thread_and_wait(rdram);
        }
        if(receive) {
            if(operand!=NULLPTR)*TO_PTR(OSMesg,operand)=TO_PTR(OSMesg,mq->msg)[mq->first];
            mq->first=(mq->first+1)%mq->msgCount;--mq->validCount;
        } else {
            if(jam)mq->first=(mq->first+mq->msgCount-1)%mq->msgCount;
            const auto index=jam?mq->first:(mq->first+mq->validCount)%mq->msgCount;
            TO_PTR(OSMesg,mq->msg)[index]=static_cast<OSMesg>(operand);++mq->validCount;
        }
        {
            const auto opposite=receive?GET_MEMBER(OSMesgQueue,mq_address,blocked_on_send):GET_MEMBER(OSMesgQueue,mq_address,blocked_on_recv);
            if(!ultramodern::thread_queue_empty(rdram,opposite))
                ultramodern::schedule_running_thread(rdram,ultramodern::thread_queue_pop(rdram,opposite));
        }
        machine.top().operands[3]=0;
completed:
        // After this stage all queue/message effects have happened exactly once.
        // check_running_queue can preempt; a restored frame skips those effects.
        machine.top().pc=return_pc;
        ultramodern::check_running_queue(rdram);
    }
    ctx->r2=machine.top().operands[3];
    machine.leave(id);
}
void thread_operation(uint8_t* rdram,recomp_context* ctx,uint32_t id) {
    auto& machine=Machine::current();
    const auto pc=machine.enter(id).pc;
    if(!pc) {
        machine.top().operands={ctx->r4,ctx->r5,0,0};
        // The pinned operations mutate their scheduler state before their only
        // wait. Capture admission must be restricted to the wait boundary;
        // arbitrary instruction-time capture here is expressly unsupported.
        machine.top().pc=return_pc;
        if(id==start_id)osStartThread(rdram,static_cast<int32_t>(ctx->r4));
        else if(id==stop_id)osStopThread(rdram,static_cast<int32_t>(ctx->r4));
        else osSetThreadPri(rdram,static_cast<int32_t>(ctx->r4),static_cast<OSPri>(ctx->r5));
    }
    machine.leave(id);
}
}
extern "C" void tooie_persistent_recv(uint8_t* rdram,recomp_context* ctx){message(rdram,ctx,recv_id,true,false);}
extern "C" void tooie_persistent_send(uint8_t* rdram,recomp_context* ctx){message(rdram,ctx,send_id,false,false);}
extern "C" void tooie_persistent_jam(uint8_t* rdram,recomp_context* ctx){message(rdram,ctx,jam_id,false,true);}
extern "C" void tooie_persistent_start_thread(uint8_t* rdram,recomp_context* ctx){thread_operation(rdram,ctx,start_id);}
extern "C" void tooie_persistent_stop_thread(uint8_t* rdram,recomp_context* ctx){thread_operation(rdram,ctx,stop_id);}
extern "C" void tooie_persistent_set_priority(uint8_t* rdram,recomp_context* ctx){thread_operation(rdram,ctx,priority_id);}
namespace tooie::persistent_state::hle {
std::span<const continuation::Function> functions() {
    static const continuation::Function values[]={
        {recv_id,tooie_persistent_recv,message_pcs},{send_id,tooie_persistent_send,message_pcs},
        {jam_id,tooie_persistent_jam,message_pcs},{start_id,tooie_persistent_start_thread,thread_pcs},
        {stop_id,tooie_persistent_stop_thread,thread_pcs},{priority_id,tooie_persistent_set_priority,thread_pcs}};
    return values;
}
}
