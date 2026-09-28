// Exercise the actual translation wrapper through the real guest-worker failure
// boundary. Standalone probes may select preserved/proposed translation source.
#ifdef TOOIE_TRANSLATION_RUNTIME_SOURCE
#include TOOIE_TRANSLATION_RUNTIME_SOURCE
#endif
#include "runtime_lifecycle.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
extern "C" void osYieldThread_recomp(uint8_t*,recomp_context*);
extern void run_next_thread(uint8_t*);
extern std::atomic_bool exited;
namespace {
constexpr int32_t first=int32_t(0x80001000u),second=int32_t(0x80001200u),entry=int32_t(0x80400000u);
std::atomic_bool after_call{false},context_unchanged{false},peer_started{false},peer_started_at_return{false};
std::atomic_int unwound{0};
struct ContextGuard {
    recomp_context* context;
    recomp_context before;
    explicit ContextGuard(recomp_context* value):context(value),before(*value) {}
    ~ContextGuard() { context_unchanged=std::memcmp(context,&before,sizeof(before))==0; ++unwound; }
};
void worker(uint8_t* rdram,recomp_context* context) {
    if(osGetThreadId(rdram,0)==1) {
        ContextGuard guard(context);
        osYieldThread_recomp(rdram,context);
        peer_started_at_return=peer_started.load();
        after_call=true;
        return;
    }
    // A queued peer returns execution to the caller. This proves a real
    // scheduler handoff rather than a wrapper return.
    peer_started=true;
    osYieldThread_recomp(rdram,context);
    for(;;) { tooie::lifecycle::poll();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::string mode=argv[1];
    std::vector<uint8_t> memory(8*1024*1024);
    if(mode=="disabled") {
        recomp_context context{},before{};
        context.r2=123;before=context;
        osYieldThread_recomp(memory.data(),&context);
        if(std::memcmp(&context,&before,sizeof(context))!=0 || tooie::lifecycle::enabled())return 1;
        std::cout<<"PASS disabled original release return, context unchanged\n";return 0;
    }
    if(mode!="same" && mode!="higher")return 2;
    auto* rdram=memory.data();
    recomp::register_game({.rom_hash=0,.internal_name="TEST",.display_name="Unsupported yield",.game_id=u8"continuous.unsupported-yield"});
    recomp::start_game(u8"continuous.unsupported-yield","");
    recomp::overlays::add_loaded_function(entry,worker);
    tooie::lifecycle::enable(false);
    ultramodern::init_thread_cleanup();
    osCreateThread(rdram,first,1,entry,0,int32_t(0x80006000u),2);
    // Equal priority must hand off FIFO. A lower-priority ready worker must
    // not preempt the caller merely because it called osYieldThread.
    osCreateThread(rdram,second,2,entry,0,int32_t(0x80007000u),mode=="same" ? 2 : 1);
    ultramodern::schedule_running_thread(rdram,first);
    ultramodern::schedule_running_thread(rdram,second);
    run_next_thread(rdram);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!tooie::lifecycle::stopping() && !after_call && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tooie::lifecycle::request_stop();
    if(!tooie::lifecycle::wait_for_producers(std::chrono::seconds(3)))std::_Exit(70);
    exited.store(true);
    ultramodern::join_thread_cleaner_thread();
    const auto counts=tooie::lifecycle::counts();
    std::string error;
    if(auto failure=tooie::lifecycle::failure()) {
        try { std::rethrow_exception(failure); }
        catch(const std::exception& exception) { error=exception.what(); }
    }
    const bool retained=recomp::current_game_id()==u8"continuous.unsupported-yield";
    ultramodern::quit();
    std::cout<<"after_call="<<after_call<<" unwound="<<unwound<<" context_unchanged="<<context_unchanged
             <<" created="<<counts.created<<" enqueued="<<counts.enqueued<<" deleted="<<counts.deleted
             <<" producers="<<counts.producers<<" error="<<error<<'\n';
    const bool selected_correctly=mode=="same" ? peer_started_at_return.load() : !peer_started_at_return.load();
    if(!after_call || !selected_correctly || unwound!=1 || !context_unchanged || counts.created!=2 || counts.enqueued!=2 || counts.deleted!=2
       || counts.producers || !retained || !error.empty())return 1;
    std::cout<<"PASS "<<mode<<" continuous yield scheduling and cleanup\n";
}
