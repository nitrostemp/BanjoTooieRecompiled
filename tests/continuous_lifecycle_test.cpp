#include "runtime_lifecycle.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
extern std::atomic_bool exited;

namespace {
constexpr int workers=40;
constexpr uint32_t object_base=0x80001000,entry=0x80400000;
std::atomic_int entries{0};
bool inject_failure=false;
void guest_entry(uint8_t* rdram,recomp_context*) {
    entries.fetch_add(1);
    if (inject_failure) throw std::runtime_error("Injected continuous guest failure");
    if (osGetThreadId(rdram,0)==workers-1) {
        while (true) {
            tooie::lifecycle::poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    std::string mode=argv[1];
    if (mode!="normal" && mode!="parked" && mode!="failure" && mode!="create-stop" && mode!="timer-empty" && mode!="timer-long") return 2;
    inject_failure=mode=="failure";
    std::vector<uint8_t> memory(8*1024*1024);
    auto* rdram=memory.data();
    if (mode=="timer-empty" || mode=="timer-long") {
        tooie::lifecycle::enable();
        ultramodern::init_timers(rdram);
        if (mode=="timer-long") {
            osCreateMesgQueue(rdram,int32_t(0x80003000),int32_t(0x80003100),4);
            osSetTimer(rdram,int32_t(0x80002000),100000000000ULL,0,int32_t(0x80003000),123);
        }
        // Let the producer enter its empty wait or multi-minute timed wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        auto before=std::chrono::steady_clock::now();
        tooie::lifecycle::request_stop();
        tooie::lifecycle::stop_and_join_timer();
        auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before);
        require(elapsed<std::chrono::seconds(1),"Timer stop failed to interrupt native wait");
        std::cout<<"PASS continuous lifecycle "<<mode<<": producer joined in "<<elapsed.count()<<"ms\n";
        return 0;
    }
    recomp::register_game({.rom_hash=0,.internal_name="TEST",.display_name="Continuous lifecycle test",.game_id=u8"continuous.test"});
    recomp::start_game(u8"continuous.test","");
    recomp::overlays::add_loaded_function(int32_t(entry),guest_entry);
    tooie::lifecycle::enable();
    ultramodern::init_thread_cleanup();
    std::exception_ptr test_failure;
    try {
    for (int i=0;i<workers;i++)
        osCreateThread(rdram,int32_t(object_base+i*0x80),i,int32_t(entry),0,int32_t(0x80010000+i*0x100),1);
    if (mode=="normal") {
        // Original libultra preserves insertion order among equal priorities.
        // Start worker0 directly; the final queued worker39 is the poller.
        for (int i=1;i<workers;i++) ultramodern::schedule_running_thread(rdram,int32_t(object_base+i*0x80));
        osStartThread(rdram,int32_t(object_base));
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (entries.load()!=workers && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(entries.load()==workers,"Natural scheduler chain did not run every worker");
    } else if (inject_failure) {
        osStartThread(rdram,int32_t(object_base));
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (!tooie::lifecycle::stopping() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(bool(tooie::lifecycle::failure()),"Guest exception escaped or was lost");
    }
    tooie::lifecycle::request_stop();
    if (mode=="create-stop") {
        bool rejected=false;
        try { osCreateThread(rdram,int32_t(object_base+workers*0x80),workers,int32_t(entry),0,int32_t(0x80020000),1); }
        catch (const ultramodern::thread_terminated&) { rejected=true; }
        require(rejected,"Worker creation after shutdown admission close was accepted");
    }
    } catch (...) { test_failure=std::current_exception(); }
    // Even a failed assertion must close the real native resources it created.
    tooie::lifecycle::request_stop();
    if (!tooie::lifecycle::wait_for_producers(std::chrono::seconds(3))) {
        std::cerr<<"FAIL: Guest producers did not unwind; memory retained until process exit\n";
        std::_Exit(70);
    }
    exited.store(true);
    ultramodern::join_thread_cleaner_thread();
    bool active_retained=recomp::current_game_id()==u8"continuous.test";
    ultramodern::quit();
    auto counts=tooie::lifecycle::counts();
    if (test_failure) {
        try { std::rethrow_exception(test_failure); }
        catch (const std::exception& error) { std::cerr<<"FAIL after safe cleanup: "<<error.what()<<"\n"; }
        return 1;
    }
    require(active_retained,"Active game reset before native worker joins");
    require(counts.created==workers && counts.producers==0 && counts.enqueued==workers && counts.deleted==workers,
            "Scalable lifecycle did not reclaim every worker exactly once");
    require(inject_failure==bool(tooie::lifecycle::failure()),"Unexpected exception status");
    std::cout<<"PASS continuous lifecycle "<<mode<<": created="<<counts.created<<" enqueued="<<counts.enqueued
             <<" deleted="<<counts.deleted<<" entries="<<entries.load()<<"\n";
}
