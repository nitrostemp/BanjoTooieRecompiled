#pragma once
#include "persistent_state_continuation.hpp"

// Isolated one-thread/message-queue slice. These addresses are synthetic proof
// inputs; the queue address is the real generated wrapper's original constant.
namespace tooie::continuation::hle_probe {
inline constexpr std::uint32_t queue=0x8007BEE0, thread=0x80070000, buffer=0x80070100;
inline constexpr std::uint32_t wait_stage=0xFFFF0001, consumed_stage=0xFFFF0002;
inline constexpr std::uint32_t message=0x13572468;
void initialize(Machine& machine,bool message_present);
void deliver(Machine& machine);
void verify_waiting(const Machine& machine);
void verify_consumed(const Machine& machine);
void verify_complete(const Machine& machine);
}
inline constexpr std::uint32_t continuation_receive_pcs[]={
    tooie::continuation::hle_probe::wait_stage,tooie::continuation::hle_probe::consumed_stage};
void original_osRecvMesg_recomp(uint8_t*,recomp_context*);
void lifted_osRecvMesg_recomp(uint8_t*,recomp_context*);
