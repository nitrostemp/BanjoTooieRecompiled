// Explicit continuation adaptation of the pinned N64ModernRuntime receive
// semantics in ultramodern/src/mesgqueue.cpp and threadqueue.cpp. This isolated
// proof supports one receiver and no send waiters; it is not a runtime replacement.
#include "persistent_state_hle_probe.hpp"
#include "ultramodern/ultra64.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace {
namespace hp=tooie::continuation::hle_probe;
using tooie::continuation::Machine;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T* at(uint8_t* rdram,std::uint32_t address) {
    check(address>=0x80000400&&std::uint64_t(address)+sizeof(T)<=0x80800000&&address%alignof(T)==0,
        "HLE proof guest object outside RDRAM/alignment");
    return reinterpret_cast<T*>(rdram+(address-0x80000000));
}
template<class T>const T* at(const uint8_t* rdram,std::uint32_t address) {
    return at<T>(const_cast<uint8_t*>(rdram),address);
}
OSMesgQueue* checked_queue(uint8_t* rdram,std::uint32_t address) {
    check(address==hp::queue,"HLE proof only supports its explicitly owned queue");
    auto* mq=at<OSMesgQueue>(rdram,address);
    check(mq->msg==static_cast<std::int32_t>(hp::buffer)&&mq->msgCount==2&&
        mq->first>=0&&mq->first<2&&mq->validCount>=0&&mq->validCount<=2&&
        mq->blocked_on_send==0,"unsupported/corrupt HLE proof queue");
    return mq;
}
void verify_operands(uint8_t* rdram,std::uint32_t queue,std::uint32_t destination,std::uint32_t flags) {
    checked_queue(rdram,queue);
    check(destination==0&&flags==OS_MESG_BLOCK,"unsupported HLE receive operands");
}
void consume(OSMesgQueue* mq) {
    check(mq->validCount>0,"cannot consume an empty queue");
    // Destination is explicitly NULL in the real generated wrapper. The pinned
    // do_recv discards that message, advances first and decrements validCount.
    mq->first=(mq->first+1)%mq->msgCount;--mq->validCount;
}
}

namespace tooie::continuation::hle_probe {
void initialize(Machine& machine,bool message_present) {
    std::fill(machine.memory.begin(),machine.memory.end(),0);
    auto* rdram=machine.memory.data();
    auto* mq=at<OSMesgQueue>(rdram,queue);auto* self=at<OSThread>(rdram,thread);
    mq->msg=static_cast<std::int32_t>(buffer);mq->msgCount=2;mq->validCount=message_present?1:0;
    *at<OSMesg>(rdram,buffer)=static_cast<OSMesg>(message);
    self->priority=10;self->id=1;self->context=nullptr;self->state=OSThreadState::RUNNING;
    machine.context.r29=static_cast<gpr>(static_cast<std::int32_t>(0x807FF000));
    machine.context.r31=static_cast<gpr>(static_cast<std::int32_t>(0x80001234));
    machine.context.status_reg=0x34000000;
}
void verify_waiting(const Machine& machine) {
    const auto* mq=at<OSMesgQueue>(machine.memory.data(),queue);
    const auto* self=at<OSThread>(machine.memory.data(),thread);
    check(mq->validCount==0&&mq->first==0&&mq->blocked_on_recv==static_cast<std::int32_t>(thread)&&
        self->queue==static_cast<std::int32_t>(queue+offsetof(OSMesgQueue,blocked_on_recv))&&
        self->next==0&&self->context==nullptr,"blocked receive linkage was not captured exactly once");
}
void deliver(Machine& machine) {
    verify_waiting(machine);
    auto* rdram=machine.memory.data();auto* mq=checked_queue(rdram,queue);
    auto* self=at<OSThread>(rdram,thread);
    const auto last=(mq->first+mq->validCount)%mq->msgCount;
    *at<OSMesg>(rdram,buffer+last*sizeof(OSMesg))=static_cast<OSMesg>(message);
    ++mq->validCount;
    // Pinned do_send wakes one receiver via thread_queue_pop: remove its link
    // and clear queue. This one-thread proof dispatches it directly, so there
    // is no additional host/native running queue to serialize.
    mq->blocked_on_recv=self->next;self->queue=0;self->state=OSThreadState::RUNNING;
}
void verify_consumed(const Machine& machine) {
    const auto* mq=at<OSMesgQueue>(machine.memory.data(),queue);
    const auto* self=at<OSThread>(machine.memory.data(),thread);
    check(mq->validCount==0&&mq->first==1&&mq->blocked_on_recv==0&&self->queue==0&&self->context==nullptr,
        "receive did not consume exactly one delivered message");
}
void verify_complete(const Machine& machine) {
    verify_consumed(machine);
    check(machine.context.r2==0&&machine.context.r29==static_cast<gpr>(static_cast<std::int32_t>(0x807FF000))&&
        machine.context.r31==static_cast<gpr>(static_cast<std::int32_t>(0x80001234)),
        "guest wrapper result/RA/SP did not resume correctly");
}
}

void original_osRecvMesg_recomp(uint8_t* rdram,recomp_context* ctx) {
    verify_operands(rdram,static_cast<std::uint32_t>(ctx->r4),static_cast<std::uint32_t>(ctx->r5),
        static_cast<std::uint32_t>(ctx->r6));
    auto* mq=checked_queue(rdram,static_cast<std::uint32_t>(ctx->r4));
    check(mq->blocked_on_recv==0,"uninterrupted HLE baseline must already be runnable");
    consume(mq);ctx->r2=0;
}
void lifted_osRecvMesg_recomp(uint8_t* rdram,recomp_context* ctx) {
    auto& machine=Machine::current();auto& frame=machine.enter(4);
    if(frame.pc==0) {
        frame.operands={static_cast<std::uint32_t>(ctx->r4),static_cast<std::uint32_t>(ctx->r5),
            static_cast<std::uint32_t>(ctx->r6),0};
        verify_operands(rdram,static_cast<std::uint32_t>(frame.operands[0]),
            static_cast<std::uint32_t>(frame.operands[1]),static_cast<std::uint32_t>(frame.operands[2]));
        auto* mq=checked_queue(rdram,static_cast<std::uint32_t>(frame.operands[0]));
        if(mq->validCount==0) {
            auto* self=at<OSThread>(rdram,hp::thread);
            check(mq->blocked_on_recv==0&&self->queue==0,"receiver already queued");
            // This phase ends AFTER the real HLE's wait-list insertion. A
            // restored frame must not execute the insertion a second time.
            self->next=mq->blocked_on_recv;
            self->queue=static_cast<std::int32_t>(hp::queue+offsetof(OSMesgQueue,blocked_on_recv));
            mq->blocked_on_recv=static_cast<std::int32_t>(hp::thread);
            frame.pc=hp::wait_stage;
            check(machine.checkpoint(hp::wait_stage),"blocked receive requires a scheduler suspension");
            return;
        }
        frame.pc=hp::wait_stage;
    }
    if(frame.pc==hp::wait_stage) {
        verify_operands(rdram,static_cast<std::uint32_t>(frame.operands[0]),
            static_cast<std::uint32_t>(frame.operands[1]),static_cast<std::uint32_t>(frame.operands[2]));
        auto* mq=checked_queue(rdram,static_cast<std::uint32_t>(frame.operands[0]));
        check(mq->blocked_on_recv==0,"scheduler resumed a receiver before removing its wait link");
        consume(mq);frame.operands[3]=0;
        // The real osRecvMesg then calls check_running_queue, which can preempt
        // after consumption but before the translation wrapper assigns v0.
        frame.pc=hp::consumed_stage;
        if(machine.checkpoint(hp::consumed_stage))return;
    }
    check(frame.pc==hp::consumed_stage,"unknown HLE receive phase");
    ctx->r2=frame.operands[3];machine.leave(4);
}
