#include "trace.hpp"
#include "game.hpp"
#include "overlay_call_trace.hpp"
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cstdio>
namespace tooie {
static std::ofstream output;
static TraceTransport transport; // Destroyed before the stream; explicit stop is required.
static std::atomic_bool trace_active{false};
static bool sink_failure_reported=false; // Protected by trace_mutex; never recursively trace it.
static std::filesystem::path directory;
static std::deque<Json> recent;
static uint64_t sequence;
static std::mutex trace_mutex, watchdog_mutex;
static std::condition_variable watchdog_cv;
static std::thread watchdog;
static bool watchdog_done;
static Json stable_state={{"state","guest not initialized"}};
std::string hex32(uint32_t value) { std::ostringstream s; s<<"0x"<<std::uppercase<<std::hex<<std::setw(8)<<std::setfill('0')<<value; return s.str(); }
void start_trace(const std::filesystem::path& path, TraceTransportOptions options) {
    std::lock_guard lock(trace_mutex);
    if(trace_active.load(std::memory_order_acquire))throw std::logic_error("Trace already started");
    directory=std::filesystem::absolute(path); std::filesystem::create_directories(directory);
    output.clear();
    output.open(directory/"events.jsonl",std::ios::trunc);
    if(!output) throw std::runtime_error("Cannot create trace");
    try{transport.start(output,options);}catch(...){output.close();throw;}
    trace_active.store(true,std::memory_order_release);
    sink_failure_reported=false;
    recent.clear(); sequence=0;
}
bool trace_enabled() noexcept { return trace_active.load(std::memory_order_acquire); }
void check_trace_health() {
    std::lock_guard lock(trace_mutex);
    // Normal frontend play intentionally has no mission trace transport.
    if (!trace_active.load(std::memory_order_acquire)) return;
    check_overlay_call_observation_health();
    transport.check_health();
}
void stop_trace() {
    std::lock_guard lock(trace_mutex);
    if(!trace_active.load(std::memory_order_acquire)){transport.stop();return;}
    std::exception_ptr error;
    try{transport.stop();}catch(...){error=std::current_exception();}
    output.close();trace_active.store(false,std::memory_order_release);
    if(!output&&!error)error=std::make_exception_ptr(std::runtime_error("Trace close failed"));
    if(error)std::rethrow_exception(error);
}
void trace(const char* event,const char* checkpoint,const char* outcome,uint32_t pc,const recomp_context* ctx,Json extra) {
    std::lock_guard lock(trace_mutex);
    Json row={{"run_id",directory.filename().string()},{"sequence",++sequence},
        {"host_monotonic_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()},
        {"guest_thread_id",nullptr},{"event",event},{"checkpoint",checkpoint},{"outcome",outcome},{"guest_pc",hex32(pc)},
        {"sp",ctx?Json(hex32(ctx->r29)):Json(nullptr)},{"ra",ctx?Json(hex32(ctx->r31)):Json(nullptr)}};
    if(ctx) row["registers"]={{"a0",hex32(ctx->r4)},{"a1",hex32(ctx->r5)},{"a2",hex32(ctx->r6)},{"a3",hex32(ctx->r7)},{"v0",hex32(ctx->r2)},{"v1",hex32(ctx->r3)}};
    row.update(extra); recent.push_back(row); if(recent.size()>256)recent.pop_front();
    // All events except ordinary overlay entry/return are immediate barriers.
    const bool ordinary_overlay=row["event"]=="overlay_lifecycle" &&
        (row["outcome"]=="entry"||row["outcome"]=="return");
    // Preserve recent fault history even if the output sink failed. Throwing a
    // sink error through cleanup logging would skip device/thread/RDRAM cleanup.
    // Host-loop health and checked final stop still make the run fail visibly.
    // Runtime-only diagnostics may collect recent rows without an active file.
    // Do not turn before-start/after-stop observations into sink failures.
    if(trace_active.load(std::memory_order_acquire) && !sink_failure_reported) {
        const auto serialized=row.dump(); // Serialization failures retain their original exception path.
        try{transport.append(serialized,!ordinary_overlay);}
        catch(const std::exception& e){sink_failure_reported=true;std::fprintf(stderr,"Trace sink failed; output incomplete: %s\n",e.what());}
    }
}
void capture_guest_state(uint8_t*rdram,const recomp_context*ctx) {
    // Called on the guest thread at a stable boundary. Watchdog never races guest memory.
    Json state=registry_snapshot();
    state["capture_kind"]="last completed diagnostic boundary; not a live instruction trace";
    state["sp"]=hex32(ctx->r29);state["ra"]=hex32(ctx->r31);
    state["memory_words"]=Json::object();
    for(uint32_t base:{0x80000300u,0x800044D0u,0x800064D0u,0x80008460u,0x80012000u,0x80012030u,0x80200000u}) {
        Json words=Json::array();for(unsigned i=0;i<8;i++)words.push_back(hex32(MEM_W(i*4,(gpr)(int32_t)base)));
        state["memory_words"][hex32(base)]=words;
    }
    std::lock_guard lock(trace_mutex);stable_state=std::move(state);
}
void fault_packet() {
    // Copy numeric history before acquiring trace_mutex; never reverse lock order.
    auto overlay_history=overlay_call_observation_snapshot();
    std::lock_guard lock(trace_mutex);
    if(directory.empty())return;
    std::exception_ptr error;
    if(trace_active.load(std::memory_order_acquire))try{transport.flush();}catch(...){error=std::current_exception();}
    std::ofstream f(directory/"fault.json");
    Json packet={{"events",recent},{"guest_state",stable_state}};
    if(!overlay_history.is_null())packet["overlay_call_observation"]=std::move(overlay_history);
    f<<packet.dump(2)<<'\n';
    f.flush();if(!f&&!error)error=std::make_exception_ptr(std::runtime_error("Fault packet write failed"));
    if(error)std::rethrow_exception(error);
}
void start_watchdog(unsigned seconds) {
    watchdog_done=false;
    watchdog=std::thread([seconds] {
        std::unique_lock lock(watchdog_mutex);
        if(watchdog_cv.wait_for(lock,std::chrono::seconds(seconds),[]{return watchdog_done;}))return;
        lock.unlock();
        try{trace("watchdog_timeout","none","failure",0,nullptr,{{"timeout_seconds",seconds},{"watchdog_kind","fixed execution deadline"},{"state_precision","last stable boundary"}});}catch(const std::exception& e){std::fprintf(stderr,"Watchdog trace failure: %s\n",e.what());}
        try{fault_packet();}catch(const std::exception& e){std::fprintf(stderr,"Watchdog fault packet failure: %s\n",e.what());}
        std::_Exit(124);
    });
}
void stop_watchdog() {
    {std::lock_guard lock(watchdog_mutex);watchdog_done=true;}watchdog_cv.notify_all();if(watchdog.joinable())watchdog.join();
}
}
