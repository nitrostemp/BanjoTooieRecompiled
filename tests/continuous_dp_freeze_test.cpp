// Real materialized event threads and runtime DP imports; only rendering is stubbed.
#ifndef TOOIE_EVENTS_RUNTIME_SOURCE
#error Define TOOIE_EVENTS_RUNTIME_SOURCE
#endif
#include TOOIE_EVENTS_RUNTIME_SOURCE
#include "recomp.h"
#include "librecomp/game.hpp"
#include <iostream>
#include <string>
#include <vector>
extern void dequeue_external_messages(uint8_t*);
extern "C" void osDpSetStatus_recomp(uint8_t*,recomp_context*);
extern "C" void osDpGetStatus_recomp(uint8_t*,recomp_context*);
namespace {
std::string mode;
std::atomic_int calls{0},vi_ticks{0},shutdowns{0};
std::thread::id parser_thread;
std::vector<int32_t> parsed_order;
moodycamel::LightweightSemaphore release_parser;
void set_status(uint32_t bits){recomp_context c{};c.r4=bits;osDpSetStatus_recomp(nullptr,&c);}
uint32_t get_status(){recomp_context c{};osDpGetStatus_recomp(nullptr,&c);return uint32_t(c.r2);}
class Renderer final:public ultramodern::renderer::RendererContext {
public:
    Renderer(){setup_result=ultramodern::renderer::SetupResult::Success;}
    bool valid() override{return true;}
    bool update_config(const ultramodern::renderer::GraphicsConfig&,const ultramodern::renderer::GraphicsConfig&) override{return true;}
    void enable_instant_present() override{}
    void send_dl(const OSTask* task) override{
        parser_thread=std::this_thread::get_id();
        parsed_order.push_back(task->t.data_ptr);
        if(mode=="mid" || mode=="stop-mid")set_status(8);
        ++calls;
        if(mode=="refreeze")release_parser.wait();
        if(mode=="parser-failure")throw std::runtime_error("injected parser failure");
    }
    void send_dummy_workload(uint32_t) override{}
    void update_screen() override{}
    void shutdown() override{++shutdowns;}
    uint32_t get_display_framerate() const override{return 60;}
    float get_resolution_scale() const override{return 1;}
};
std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(uint8_t*,ultramodern::renderer::WindowHandle,bool){return std::make_unique<Renderer>();}
void vi_callback(){++vi_ticks;}
template<class F>bool await(F f){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!f() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return f();
}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    mode=argv[1];
    const bool disabled=mode=="disabled",stop=mode=="stop-pre" || mode=="stop-mid";
    const bool pre=mode=="pre" || mode=="stop-pre" || mode=="two" || mode=="vi-unfreeze" || mode=="refreeze" || disabled;
    if(!pre && !stop && mode!="mid" && mode!="parser-failure" && mode!="normal" && mode!="status")return 2;
    if(!disabled)tooie::lifecycle::enable(false);
    if(mode=="vi-unfreeze"){
        recomp::register_game({.rom_hash=0,.internal_name="TEST",.display_name="DP freeze VI test",.game_id=u8"dp.freeze.test"});
        recomp::start_game(u8"dp.freeze.test","");
    }
    if(mode=="status"){
        set_status(8|2|32); // Freeze, XBUS, Flush.
        const auto full=get_status();
        set_status(4|8); // Both clear and set is a no-op, per pinned runtime.
        const bool unchanged=get_status()==full;
        set_status(4);
        const bool preserved=(get_status()&7)==5;
        std::cout<<"status unchanged="<<unchanged<<" unrelated_bits="<<preserved<<'\n';
        return unchanged && preserved ? 0:1;
    }
    std::vector<uint8_t> memory(8*1024*1024);auto* rdram=memory.data();
    constexpr int32_t sp=int32_t(0x80001000u),dp=int32_t(0x80001100u),parsed=int32_t(0x80001200u),completed=int32_t(0x80001300u);
    for(auto mq:{sp,dp,parsed,completed})osCreateMesgQueue(rdram,mq,mq+0x40,8);
    osSetEventMesg(rdram,OS_EVENT_SP,sp,0x51);osSetEventMesg(rdram,OS_EVENT_DP,dp,0xd1);
    for(auto address:{int32_t(0x80003000u),int32_t(0x80004000u)}){
        osExQueueDisplaylistEvent(parsed,1,address,OS_EX_DISPLAYLIST_EVENT_PARSED);
        osExQueueDisplaylistEvent(completed,2,address,OS_EX_DISPLAYLIST_EVENT_COMPLETED);
    }
    ultramodern::renderer::set_callbacks({create_renderer});
    ultramodern::events::set_callbacks({vi_callback,nullptr});
    ultramodern::init_events(rdram,{});
    if(mode=="vi-unfreeze")osViSwapBuffer(rdram,int32_t(0x803bc480u));
    const auto gfx_id=events_context.sp.gfx_thread.get_id();
    const auto count=[&](int32_t mq){dequeue_external_messages(rdram);return TO_PTR(OSMesgQueue,mq)->validCount;};
    if(pre)set_status(8);
    auto* task=TO_PTR(OSTask,int32_t(0x80002000u));task->t.type=M_GFXTASK;task->t.data_ptr=int32_t(0x80003000u);
    ultramodern::submit_rsp_task(rdram,int32_t(0x80002000u));
    if(mode=="two"){task->t.data_ptr=int32_t(0x80004000u);ultramodern::submit_rsp_task(rdram,int32_t(0x80002000u));}
    bool ok=await([&]{return count(sp)==1;});
    bool held=true,vi_progress=true;
    if((pre && !disabled) || mode=="mid" || mode=="stop-mid"){
        if(!pre)ok=await([&]{return calls==1;}) && ok;
        const auto before_vi=vi_ticks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
        held=count(dp)==0 && count(parsed)==0 && count(completed)==0 && calls==(pre?0:1);
        vi_progress=vi_ticks>before_vi;
        ok=held && vi_progress && ok;
        if(mode=="vi-unfreeze"){
            // Real VI worker latches framebuffer while gfx worker is held,
            // despite queued ScreenUpdateActions on the blocked gfx worker.
            osViSwapBuffer(rdram,int32_t(0x803e1c80u));
            ok=await([]{std::lock_guard lock(events_context.message_mutex);return uint32_t(osViGetCurrentFramebuffer())==0x803e1c80u;}) && ok;
            ok=calls==0 && count(dp)==0 && ok;
        }
        if(stop)tooie::lifecycle::request_stop();else set_status(4);
        if(mode=="refreeze"){
            ok=await([]{return calls==1;}) && ok;
            set_status(8);release_parser.signal();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            ok=count(dp)==0 && count(parsed)==0 && count(completed)==0 && ok;
            set_status(4);
        }
    }
    const int expected=mode=="two"?2:1;
    if(mode=="parser-failure")ok=await([]{return bool(tooie::lifecycle::failure());}) && ok;
    else if(!stop)ok=await([&]{return count(dp)==expected && count(completed)==expected;}) && ok;
    exited.store(true);graphics_shutdown_ready.signal();ultramodern::join_event_threads();
    const int final_dp=stop || mode=="parser-failure"?0:expected;
    ok=count(sp)==(stop?1:expected) && count(dp)==final_dp && count(parsed)==final_dp && count(completed)==final_dp && shutdowns==1 && ok;
    ok=(bool(tooie::lifecycle::failure())==(mode=="parser-failure")) && ok;
    if(calls>0)ok=parser_thread==gfx_id && ok;
    if(mode=="two")ok=parsed_order==std::vector<int32_t>{int32_t(0x80003000u),int32_t(0x80004000u)} && ok;
    std::cout<<(ok?"PASS":"FAIL")<<" mode="<<mode<<" held="<<held<<" vi_progress="<<vi_progress<<" SP="<<count(sp)<<" DP="<<count(dp)<<" parsed="<<calls<<" shutdown="<<shutdowns<<'\n';
    return ok?0:1;
}
