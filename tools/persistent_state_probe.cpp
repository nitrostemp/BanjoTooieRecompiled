// Isolated engineering executable. It cannot load ROMs, gameplay saves or profiles.
#include "persistent_state_continuation.hpp"
#include "persistent_state_probe_generated.hpp"
#include "platform_support.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using tooie::continuation::Machine;
using tooie::continuation::Identity;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void initialize(Machine& machine) {
    auto* rdram=machine.memory.data();
    // Synthetic table data exercises real guest loads/stores without extracting
    // any licensed game data. Keep all initial FPR values finite.
    for(std::size_t i=0;i<machine.memory.size();++i)
        machine.memory[i]=static_cast<std::uint8_t>((i*17u+(i>>9))&255u);
    const auto word=[&](std::uint32_t address,float value) {
        MEM_W(0,static_cast<gpr>(static_cast<std::int32_t>(address)))=
            std::bit_cast<std::int32_t>(value);
    };
    word(0x800416CC,1.0f/360.0f);word(0x800416D0,360.0f);
    word(0x800416D8,1.0f/4096.0f);
    for(std::uint32_t i=0;i<2050;++i)word(0x8003D3A0+i*4,static_cast<float>(i)/2048.0f);
    machine.context.r29=static_cast<gpr>(static_cast<std::int32_t>(0x807FF000));
    machine.context.r31=static_cast<gpr>(static_cast<std::int32_t>(0x80001234));
    machine.context.f12.fl=45.0f;
    machine.context.status_reg=0x34000000;
}
Identity identity() {
    return {continuation_program_sha256,
        tooie::platform::file_sha256(tooie::platform::executable_path()),
        "synthetic-no-ROM-no-EEPROM-v1",{}};
}
std::string baseline() {
    Machine original(continuation_functions);initialize(original);
    original_func_800137D4(original.memory.data(),&original.context);
    auto expected=original.completion_hash();
    Machine lifted(continuation_functions);initialize(lifted);lifted.start(1);
    require(lifted.frames.empty()&&!lifted.suspended(),"lifted baseline did not return");
    require(lifted.completion_hash()==expected,"lifting changed uninterrupted guest execution");
    return expected;
}
std::string receive_baseline() {
    namespace hp=tooie::continuation::hle_probe;
    Machine original(continuation_functions);hp::initialize(original,true);
    original_func_8001A0F0(original.memory.data(),&original.context);
    hp::verify_complete(original);
    auto expected=original.completion_hash();
    Machine lifted(continuation_functions);hp::initialize(lifted,true);lifted.start(3);
    hp::verify_complete(lifted);
    require(lifted.frames.empty()&&!lifted.suspended(),"lifted receive baseline did not return");
    require(lifted.completion_hash()==expected,"HLE continuation changed uninterrupted receive");
    return expected;
}
void verify_receive_checkpoint(const Machine& loaded,bool consumed) {
    namespace hp=tooie::continuation::hle_probe;
    Machine reference(continuation_functions);hp::initialize(reference,false);
    reference.start(3,hp::wait_stage);
    if(consumed) {hp::deliver(reference);reference.resume(hp::consumed_stage);}
    require(loaded.completion_hash()==reference.completion_hash()&&loaded.frames==reference.frames,
        "HLE checkpoint differs from its supported synthetic stage");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"usage: persistent_state_probe create|resume|create-recv|deliver-recv|resume-recv|reject-identity|reject-file path.tstate-probe");
        const std::string mode=argv[1];const std::filesystem::path path=std::filesystem::absolute(argv[2]);
        auto expected=identity();Machine machine(continuation_functions);
        if(mode=="create-recv"||mode=="deliver-recv"||mode=="resume-recv") {
            namespace hp=tooie::continuation::hle_probe;
            expected.input="synthetic-no-ROM-no-EEPROM-receive-v1";
            if(mode=="create-recv") {
                expected.expected_completion=receive_baseline();
                hp::initialize(machine,false);machine.identity=expected;
                machine.start(3,hp::wait_stage);hp::verify_waiting(machine);
                require(machine.frames.size()==2&&machine.frames.back().function==4&&
                    machine.frames.back().pc==hp::wait_stage,"receive wait stage not captured");
                machine.save(path);
                std::cout<<"HLE_WAIT_CHECKPOINT_WRITTEN frames=2 wait_links=1 consumed=0 expected_completion="
                    <<expected.expected_completion<<"\n";return 0;
            }
            std::fill(machine.memory.begin(),machine.memory.end(),0xCD);
            machine.load(path,expected);
            verify_receive_checkpoint(machine,mode=="resume-recv");
            if(mode=="deliver-recv") {
                hp::deliver(machine);machine.resume(hp::consumed_stage);hp::verify_consumed(machine);
                require(machine.suspended()&&machine.frames.size()==2&&
                    machine.frames.back().pc==hp::consumed_stage,"post-consume stage not captured");
                machine.save(path);
                std::cout<<"HLE_DELIVERY_RESUME_PASS wait_links=0 consumed=1 frames=2 checkpoint_replaced=true\n";
                return 0;
            }
            machine.resume();hp::verify_complete(machine);
            auto result=machine.completion_hash();
            require(result==machine.identity.expected_completion&&result==receive_baseline(),
                "fresh-process HLE continuation diverged from uninterrupted receive");
            require(machine.frames.empty()&&!machine.suspended(),"HLE frames did not retire");
            std::cout<<"HLE_FRESH_PROCESS_RESUME_PASS consumed=1 RA_SP_restored=true completion="
                <<result<<" gameplay=false EEPROM=false\n";return 0;
        }
        if(mode=="create") {
            expected.expected_completion=baseline();initialize(machine);machine.identity=expected;
            machine.start(1,continuation_yield_pc);
            require(machine.suspended()&&machine.frames.size()==2,"did not suspend inside nested original call");
            require(machine.frames[0].function==1&&machine.frames[0].pc==1&&
                machine.frames[1].function==2&&machine.frames[1].pc==continuation_yield_pc,
                "wrong nested continuation topology");
            const auto suspended=machine.completion_hash();
            require(suspended!=expected.expected_completion,"checkpoint accidentally equals final execution");
            machine.save(path);
            std::cout<<"CHECKPOINT_WRITTEN frames=2 memory="<<machine.memory.size()
                <<" checkpoint="<<suspended<<" expected_completion="<<expected.expected_completion
                <<" program="<<expected.program<<"\n";
            return 0; // Native stack and process end here; no in-process restore.
        }
        if(mode=="resume") {
            std::fill(machine.memory.begin(),machine.memory.end(),0xCD);
            machine.context.r5=0xBADBADBAD;
            machine.load(path,expected);
            require(machine.context.f_odd==&machine.context.f0.u32h,"FPR pointer was not rebound");
            auto loaded=machine.completion_hash();auto depth=machine.frames.size();
            // This deliberately bounded probe accepts only its exact synthetic
            // checkpoint. Format/checksum validation alone is not a sandbox for
            // arbitrary guest pointer values in generated native memory accesses.
            Machine reference(continuation_functions);initialize(reference);
            reference.start(1,continuation_yield_pc);
            require(loaded==reference.completion_hash()&&machine.frames==reference.frames,
                "checkpoint does not match the supported synthetic execution boundary");
            machine.resume();
            auto result=machine.completion_hash();
            require(result==machine.identity.expected_completion,"fresh-process continuation diverged from original");
            require(result==baseline(),"stored completion disagrees with fresh original execution");
            require(machine.frames.empty()&&!machine.suspended(),"continuations did not retire");
            std::cout<<"FRESH_PROCESS_RESUME_PASS restored_frames="<<depth
                <<" loaded="<<loaded<<" completion="<<result
                <<" gameplay=false EEPROM=false\n";
            return 0;
        }
        if(mode=="reject-identity"||mode=="reject-file") {
            initialize(machine);
            auto before=machine.completion_hash();
            if(mode=="reject-identity")expected.program="incompatible-program";
            bool rejected=false;
            try {machine.load(path,expected);}
            catch(const std::exception& e) {rejected=true;std::cout<<"REJECTED "<<e.what()<<"\n";}
            require(rejected,"invalid checkpoint was accepted");
            require(machine.completion_hash()==before&&machine.frames.empty()&&!machine.suspended(),
                "rejected load changed the machine");
            std::cout<<"REJECTION_PRESERVED_MACHINE\n";return 0;
        }
        throw std::runtime_error("Unknown continuation probe mode");
    } catch(const std::exception& e) {
        std::cerr<<"CONTINUATION_PROBE_FAILED "<<e.what()<<"\n";return 1;
    }
}
