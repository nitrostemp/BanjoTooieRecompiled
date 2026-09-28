// Test actual event body with a controlled future wall-clock deadline. All waits
// remain real native waits; the seam only records when VI enters its wait.
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <string>
#include <array>
#include "ultramodern/ultramodern.hpp"
#include "runtime_lifecycle.hpp"
#include "lightweightsemaphore.h"
namespace {
moodycamel::LightweightSemaphore entered_wait;
std::atomic_int callbacks{0};
std::atomic_bool wait_recorded{false};
std::string mode;
std::chrono::high_resolution_clock::time_point anchor;
std::array<std::chrono::steady_clock::time_point,6> callback_times;
std::array<uint64_t,6> callback_counts;
void record_wait() { if (!wait_recorded.exchange(true)) entered_wait.signal(); }
}
namespace ultramodern {
std::chrono::high_resolution_clock::time_point test_get_start() {
    if (mode == "backwards" || mode == "disabled-backwards") return anchor-std::chrono::seconds(1);
    if (mode == "cadence" || mode == "disabled-cadence") return anchor;
    return anchor;
}
std::chrono::high_resolution_clock::duration test_time_since_start() {
    if (mode == "backwards" || mode == "disabled-backwards") return -std::chrono::seconds(1);
    if (mode == "rollback" && callbacks.load() != 0) return -std::chrono::seconds(1);
    return std::chrono::high_resolution_clock::now()-anchor;
}
void test_sleep_until(const std::chrono::high_resolution_clock::time_point& target) {
    record_wait();
    sleep_until(target);
}
}
namespace std::this_thread {
template<class Rep,class Period>
void test_sleep_for(const std::chrono::duration<Rep,Period>& duration) {
    record_wait();
    sleep_for(duration);
}
}
#define get_start test_get_start
#define time_since_start test_time_since_start
#define sleep_until test_sleep_until
#define sleep_for test_sleep_for
#include TOOIE_EVENTS_RUNTIME_SOURCE
#undef get_start
#undef time_since_start
#undef sleep_until
#undef sleep_for
int main(int argc,char** argv) {
    mode=argc==2?argv[1]:"stop";
    const bool stop=mode=="stop";
    const bool cadence=mode=="cadence" || mode=="disabled-cadence";
    const bool backwards=mode=="backwards" || mode=="disabled-backwards";
    const bool rollback=mode=="rollback";
    const bool disabled=mode.starts_with("disabled-");
    if (!stop && !cadence && !backwards && !rollback) return 2;
    if (!disabled) tooie::lifecycle::enable(false);
    ultramodern::events::set_callbacks({[]{
        const auto count=++callbacks;
        if(count<=6) { callback_times[count-1]=std::chrono::steady_clock::now(); callback_counts[count-1]=total_vis; }
        if (mode=="backwards") { if(count==4) exited.store(true); }
        else if(mode=="rollback") { if(count==6) exited.store(true); }
        else if (mode!="stop") exited.store(true);
    },nullptr});
    set_dummy_vi(false);
    anchor=std::chrono::high_resolution_clock::now();
    const auto launched=std::chrono::steady_clock::now();
    if (cadence) total_vis=3; // Original 60-Hz deadline is exactly start+50ms.
    if (stop) total_vis=30; // Keep VI inside a real wait when stop is issued.
    events_context.vi.thread=std::thread{vi_thread_func};
    if (stop && !entered_wait.wait(2000000)) { std::cerr<<"FAIL VI never entered wait\n"; std::_Exit(70); }
    const auto start=std::chrono::steady_clock::now();
    if (stop) { exited.store(true); tooie::lifecycle::request_stop(); }
    ultramodern::join_event_threads();
    const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-launched).count();
    std::cout<<"VI "<<mode<<" join_ms="<<milliseconds<<" callbacks="<<callbacks<<" total_vis="<<total_vis<<" elapsed_ms="<<elapsed<<'\n';
    if (stop && (milliseconds>=100 || callbacks!=0 || total_vis!=30)) return 1;
    if (backwards && disabled && (callbacks!=1 || total_vis!=uint64_t(-59))) return 1;
    if (backwards && !disabled && (callbacks!=4 || elapsed<40 || elapsed>=200 || total_vis<4 || total_vis>12)) return 1;
    if (cadence && (callbacks!=1 || elapsed<40 || elapsed>=200 || total_vis<4 || total_vis>12)) return 1;
    if (rollback) {
        if(callbacks!=6 || elapsed<70 || elapsed>=250) return 1;
        for(int i=1;i<6;++i) {
            const auto gap=std::chrono::duration_cast<std::chrono::microseconds>(callback_times[i]-callback_times[i-1]).count();
            std::cout<<"rollback callback="<<i+1<<" gap_us="<<gap<<" total_vis="<<callback_counts[i]<<'\n';
            if(callback_counts[i]<=callback_counts[i-1] || gap<5000) return 1;
        }
    }
    std::cout<<"PASS VI "<<mode<<'\n';
}
