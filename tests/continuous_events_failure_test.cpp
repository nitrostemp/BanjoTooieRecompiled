// Focused tests of actual materialized event thread bodies and queue delivery.
#ifndef TOOIE_EVENTS_RUNTIME_SOURCE
#error Define TOOIE_EVENTS_RUNTIME_SOURCE to the selected events.cpp
#endif
#include TOOIE_EVENTS_RUNTIME_SOURCE
#include "runtime_lifecycle.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <cerrno>
#include <dlfcn.h>
#endif

extern void dequeue_external_messages(uint8_t*);
namespace {
std::string mode;
std::atomic_int thread_creations{0};
std::atomic_int shutdowns{0}, destructions{0}, calls{0};
moodycamel::LightweightSemaphore release_rsp;
std::atomic_bool premature_shutdown{false};
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
class Renderer final : public ultramodern::renderer::RendererContext {
public:
    Renderer() { setup_result = (mode == "invalid" || mode == "init-invalid") ? ultramodern::renderer::SetupResult::GraphicsDeviceNotFound : ultramodern::renderer::SetupResult::Success; chosen_api=ultramodern::renderer::GraphicsApi::Auto; }
    ~Renderer() { ++destructions; }
    bool valid() override { return mode != "invalid" && mode != "init-invalid"; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&,const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dl(const OSTask*) override { ++calls; if (mode == "gfx-throw") throw std::runtime_error("injected send_dl failure"); }
    void send_dummy_workload(uint32_t) override {}
    void update_screen() override {}
    void shutdown() override { ++shutdowns; if (mode == "shutdown-throw") throw std::runtime_error("injected shutdown failure"); }
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1; }
};
std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(uint8_t*,ultramodern::renderer::WindowHandle,bool) {
    if (mode == "create-throw" || mode == "init-create-throw") throw std::runtime_error("injected renderer construction failure");
    if (mode == "create-null") return nullptr;
    return std::make_unique<Renderer>();
}
void gfx_init() { if (mode == "gfx-init-throw") throw std::runtime_error("injected gfx init failure"); }
void rsp_init() { if (mode == "rsp-init-throw") throw std::runtime_error("injected RSP init failure"); }
void vi_callback() { if (mode == "vi-throw") throw std::runtime_error("injected VI callback failure"); }
bool rsp_task(uint8_t*, const OSTask*) {
    ++calls;
    if (mode == "producer-order") {
        release_rsp.wait();
        premature_shutdown = shutdowns != 0 || destructions != 0;
    }
    if (mode == "rsp-throw") throw std::runtime_error("injected RSP task failure");
    return mode != "rsp-false";
}
}
#ifdef __linux__
// Focused fault injection into std::thread's real creation call. The first
// graphics worker starts, while the second (RSP) creation fails with EAGAIN.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void*(*fn)(void*), void* arg) noexcept {
    using Create = int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
    static auto real_create = reinterpret_cast<Create>(dlsym(RTLD_NEXT,"pthread_create"));
    if (!real_create) std::_Exit(71);
    if (mode == "init-partial" && ++thread_creations == 2) return EAGAIN;
    return real_create(thread,attr,fn,arg);
}
#endif
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    mode = argv[1];
    const bool disabled = mode.starts_with("disabled-");
    if (disabled) mode.erase(0,9);
    const bool rsp = mode == "rsp-false" || mode == "rsp-throw" || mode == "rsp-success";
    const bool startup_failure = mode == "create-throw" || mode == "create-null" || mode == "invalid" || mode == "gfx-init-throw" || mode == "rsp-init-throw";
    const bool task_failure = mode == "gfx-throw" || mode == "rsp-false" || mode == "rsp-throw";
    const bool init_failure = mode == "init-create-throw" || mode == "init-invalid" || mode == "init-partial";
    if (!rsp && !startup_failure && !init_failure && mode != "producer-order" && mode != "vi-throw" && mode != "gfx-throw" && mode != "gfx-success" && mode != "shutdown-throw") return 2;
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    auto* rdram=memory.data();
    constexpr int32_t sp_mq=int32_t(0x80001000u),dp_mq=int32_t(0x80001100u),parsed_mq=int32_t(0x80001200u),completed_mq=int32_t(0x80001300u);
    for (auto mq : {sp_mq,dp_mq,parsed_mq,completed_mq}) osCreateMesgQueue(rdram,mq,mq+0x40,8);
    osSetEventMesg(rdram,OS_EVENT_SP,sp_mq,0x51);
    osSetEventMesg(rdram,OS_EVENT_DP,dp_mq,0xD1);
    osExQueueDisplaylistEvent(parsed_mq,1,int32_t(0x80003000u),OS_EX_DISPLAYLIST_EVENT_PARSED);
    osExQueueDisplaylistEvent(completed_mq,2,int32_t(0x80003000u),OS_EX_DISPLAYLIST_EVENT_COMPLETED);
    if (!disabled) tooie::lifecycle::enable(false);
    ultramodern::renderer::set_callbacks({create_renderer});
    ultramodern::rsp::set_callbacks({rsp_init,rsp_task});
    ultramodern::events::set_callbacks({vi_callback,gfx_init});
    events_context.rdram=rdram;
    if (mode == "producer-order") {
        ultramodern::init_events(rdram,{});
        auto* task=TO_PTR(OSTask,int32_t(0x80002000u));
        task->t.type=M_AUDTASK;
        ultramodern::submit_rsp_task(rdram,int32_t(0x80002000u));
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(calls==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        require(calls==1,"RSP producer did not start");
        exited.store(true);
        graphics_shutdown_ready.signal();
        std::thread joiner{ultramodern::join_event_threads};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const bool held=shutdowns==0 && destructions==0;
        release_rsp.signal();
        joiner.join();
        require(held && !premature_shutdown,"renderer resources released with RSP producer still running");
        require(shutdowns==1 && destructions==1,"renderer resource cleanup count mismatch");
        std::cout<<"PASS event boundary producer-order: RSP joined before renderer shutdown=1\n";
        return 0;
    }
    if (init_failure || mode == "vi-throw") {
        bool caught = false;
        if (init_failure) {
            try { ultramodern::init_events(rdram,{}); }
            catch (...) { caught = true; }
        } else {
            set_dummy_vi(false);
            events_context.vi.thread = std::thread{vi_thread_func};
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (!tooie::lifecycle::failure() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        }
        const bool retained = destructions==0;
        exited.store(true);
        graphics_shutdown_ready.signal();
        ultramodern::join_event_threads();
        require(!init_failure || caught,"init_events did not propagate failure");
        require(bool(tooie::lifecycle::failure()),"partial initialization/VI lost failure");
        require(retained,"partial initialization destroyed renderer before shutdown release");
        require(!events_context.sp.gfx_thread.joinable() && !events_context.sp.task_thread.joinable() && !events_context.vi.thread.joinable(),"partial initialized worker remained joinable");
        if (mode=="init-invalid" || mode=="init-partial") require(shutdowns==1 && destructions==1,"partial init lost renderer cleanup");
        std::cout<<"PASS event boundary "<<mode<<": propagated=1 joined=1 retained_until_release=1\n";
        return 0;
    }
    moodycamel::LightweightSemaphore ready;
    if (rsp) events_context.sp.task_thread = std::thread{task_thread_func,rdram,&ready};
    else events_context.sp.gfx_thread = std::thread{gfx_thread_func,rdram,&ready,ultramodern::renderer::WindowHandle{}};
    if (!ready.wait(2000000)) { std::cerr<<"FAIL missing ready notification\n";std::_Exit(70); }
    if (!startup_failure && mode != "shutdown-throw") {
        OSTask* task=TO_PTR(OSTask,int32_t(0x80002000u));
        task->t.type=rsp?M_AUDTASK:M_GFXTASK;
        task->t.data_ptr=int32_t(0x80003000u);
        ultramodern::submit_rsp_task(rdram,int32_t(0x80002000u));
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    if (task_failure || startup_failure) {
        while (!tooie::lifecycle::failure() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    } else if (mode != "shutdown-throw") {
        while (calls==0 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    }
    bool retained_before_join = !rsp && mode != "create-throw" && mode != "create-null" ? destructions==0 : true;
    exited.store(true);
    graphics_shutdown_ready.signal();
    ultramodern::join_event_threads();
    dequeue_external_messages(rdram);
    try {
        require(!ready.tryWait(),"ready was signalled more than once");
        require(bool(tooie::lifecycle::failure())==(startup_failure||task_failure||mode=="shutdown-throw"),"failure status mismatch");
        if (startup_failure) require(renderer_setup_result.load()!=ultramodern::renderer::SetupResult::Success,"failed renderer initialization published Success");
        require(retained_before_join,"renderer destroyed before shutdown release");
        const auto count=[&](int32_t mq){return TO_PTR(OSMesgQueue,mq)->validCount;};
        const int expected_sp=(mode=="gfx-success"||mode=="gfx-throw"||mode=="rsp-success")?1:0;
        require(count(sp_mq)==expected_sp,"wrong SP event count");
        const int expected_dp=mode=="gfx-success"?1:0;
        require(count(dp_mq)==expected_dp && count(parsed_mq)==expected_dp && count(completed_mq)==expected_dp,"failed task fabricated DP/parsed/completed event");
        if (!rsp && mode!="create-throw" && mode!="create-null") require(shutdowns==1 && destructions==1,"renderer shutdown/destruction not exactly once");
        std::cout<<"PASS event boundary "<<mode<<": SP="<<count(sp_mq)<<" DP="<<count(dp_mq)<<" parsed="<<count(parsed_mq)<<" completed="<<count(completed_mq)<<" ready_once=1\n";
    } catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
}
