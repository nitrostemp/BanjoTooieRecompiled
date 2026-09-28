#include "runtime_lifecycle.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The test supplies only guest-entry dispatch and process exit state. Native
// scheduling, queues, blocking, priority changes, stop/restart and joins are real.
std::atomic_bool exited{false};
extern void run_next_thread(uint8_t*);
namespace {
constexpr int count=7, controller=0, idle=6;
constexpr int32_t object(int id) { return int32_t(0x80001000u+id*0x100u); }
constexpr int32_t shared=int32_t(0x80003000u), shared_data=int32_t(0x80003100u);
constexpr int32_t done=int32_t(0x80003200u), done_data=int32_t(0x80003300u);
constexpr int32_t payload(int id) { return int32_t(0x80003400u+id*4u); }
std::string mode;
std::vector<int> entered, completed, delivered, arrivals, blocked_snapshot;
std::atomic_bool finished{false};
bool removal_checked=false;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
bool receiving() { return mode.starts_with("recv"); }
bool sending() { return mode.starts_with("send"); }
bool removing() { return mode.ends_with("remove"); }
void verify_queue(uint8_t* rdram,int32_t head) {
    int seen=0;
    for (int32_t cur=head;cur;cur=TO_PTR(OSThread,cur)->next) {
        require(++seen<=5,"queue cycle or unexpected node");
        blocked_snapshot.push_back(TO_PTR(OSThread,cur)->id);
    }
    require(seen==5,"queue lost a worker");
}
void guest(uint8_t* rdram) {
    const int id=osGetThreadId(rdram,0);
    if (id==idle) {
        while (true) { tooie::lifecycle::poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    if (id!=controller) {
        entered.push_back(id);
        if (receiving()) {
            arrivals.push_back(id);
            require(osRecvMesg(rdram,shared,payload(id),OS_MESG_BLOCK)==0,"worker receive failed");
            require(*TO_PTR(OSMesg,payload(id))==0x51,"worker receive payload corrupted");
        } else if (sending()) {
            arrivals.push_back(id);
            require(osSendMesg(rdram,shared,id,OS_MESG_BLOCK)==0,"worker send failed");
        }
        completed.push_back(id);
        require(osSendMesg(rdram,done,id,OS_MESG_BLOCK)==0,"completion send failed");
        return;
    }
    // Controller starts at100. For wait tests it temporarily becomes5, so every
    // new worker runs immediately through its real blocking operation before
    // osStartThread returns. This fixes arrival order independently of tie policy.
    if (receiving() || sending()) osSetThreadPri(rdram,0,5);
    for (int peer:{1,2,3,4,5}) osStartThread(rdram,object(peer));
    if (receiving() || sending()) {
        require(arrivals==std::vector<int>({1,2,3,4,5}),"wait arrival order was not controlled");
    }
    if (removing()) {
        auto* peer=TO_PTR(OSThread,object(2));
        auto* context=peer->context;
        osStopThread(rdram,object(2));
        require(peer->state==OSThreadState::STOPPED && peer->context==context && context,
                "stop failed to preserve native frame");
        osStartThread(rdram,object(2));
        require(peer->context==context,"restart replaced native frame");
        removal_checked=true;
    }
    if (receiving() || sending()) {
        osSetThreadPri(rdram,0,100);
        auto* q=TO_PTR(OSMesgQueue,shared);
        verify_queue(rdram,receiving()?q->blocked_on_recv:q->blocked_on_send);
    }
    for (int i=0;i<5;i++) {
        if (receiving()) require(osSendMesg(rdram,shared,0x51,OS_MESG_NOBLOCK)==0,"controller wake failed");
        if (sending()) {
            require(osRecvMesg(rdram,shared,payload(controller),OS_MESG_NOBLOCK)==0,"controller drain failed");
            const int value=*TO_PTR(OSMesg,payload(controller));
            if (i==0) require(value==0x7E,"initial full queue payload changed");
            else delivered.push_back(value);
        }
        require(osRecvMesg(rdram,done,payload(controller),OS_MESG_BLOCK)==0,"controller completion wait failed");
    }
    if (sending()) {
        require(osRecvMesg(rdram,shared,payload(controller),OS_MESG_NOBLOCK)==0,"last drain failed");
        delivered.push_back(*TO_PTR(OSMesg,payload(controller)));
    }
    finished.store(true,std::memory_order_release);
}
void print_order(const char* label,const std::vector<int>& values) {
    std::cout<<label<<'='; for (int v:values) std::cout<<v; std::cout<<' ';
}
}
void run_thread_function(uint8_t* rdram,uint64_t,uint64_t,uint64_t) { guest(rdram); }
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    mode=argv[1];
    if (mode!="running" && mode!="running-remove" && mode!="recv" && mode!="recv-remove"
        && mode!="send" && mode!="send-remove") return 2;
    std::vector<uint8_t> memory(8*1024*1024); auto* rdram=memory.data();
    tooie::lifecycle::enable(); ultramodern::init_thread_cleanup();
    osCreateMesgQueue(rdram,shared,shared_data,1);
    osCreateMesgQueue(rdram,done,done_data,8);
    // A full queue is initialized before guest execution; no scheduler mutation.
    if (sending()) { auto* q=TO_PTR(OSMesgQueue,shared); q->validCount=1; *TO_PTR(OSMesg,shared_data)=0x7E; }
    const int priorities[count]={100,30,30,30,60,10,1};
    for (int id=0;id<count;id++)
        osCreateThread(rdram,object(id),id,int32_t(0x80400000u),0,int32_t(0x80010000u+id*0x1000u),priorities[id]);
    ultramodern::schedule_running_thread(rdram,object(controller));
    ultramodern::schedule_running_thread(rdram,object(idle));
    run_next_thread(rdram);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!finished.load(std::memory_order_acquire) && !tooie::lifecycle::stopping()
           && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tooie::lifecycle::request_stop();
    if (!tooie::lifecycle::wait_for_producers(std::chrono::seconds(3))) {
        std::cerr<<"FAIL producer shutdown deadline; retaining RDRAM to process exit\n";std::_Exit(70);
    }
    exited.store(true); ultramodern::join_thread_cleaner_thread();
    auto counts=tooie::lifecycle::counts();
    const std::vector<int> expected=removing()?std::vector<int>{4,1,3,2,5}:std::vector<int>{4,1,2,3,5};
    bool pass=finished && completed==expected && (receiving() || sending() || entered==expected)
      && (!sending() || delivered==expected) && (!(receiving() || sending()) || blocked_snapshot==expected)
      && (!removing() || removal_checked) && counts.created==count && counts.enqueued==count
      && counts.deleted==count && counts.producers==0 && !tooie::lifecycle::failure();
    if (auto error=tooie::lifecycle::failure()) try {std::rethrow_exception(error);}
      catch(const std::exception& e) {std::cerr<<"guest failure: "<<e.what()<<'\n';}
    std::cout<<(pass?"PASS ":"FAIL ")<<mode<<' ';
    print_order("expected",expected);print_order("entered",entered);print_order("completed",completed);
    if (receiving() || sending()) print_order("waitqueue",blocked_snapshot);
    if (sending()) print_order("delivered",delivered);
    std::cout<<"created="<<counts.created<<" enqueued="<<counts.enqueued<<" joined_deleted="<<counts.deleted
             <<" producers="<<counts.producers<<" preserved_restart="<<removal_checked<<'\n';
    return pass?0:1;
}
