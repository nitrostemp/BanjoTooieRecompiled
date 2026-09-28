// Positive generic-runtime prerequisite. No original Tooie core1/idle execution.
#include "game.hpp"
#include "context_observer.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <array>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

extern void init(uint8_t*, recomp_context*, gpr);
extern "C" void func_80013678(uint8_t*, recomp_context*);
namespace {
constexpr int32_t entry = int32_t(0x80013678);
constexpr int32_t sentinel = int32_t(0x80700000);
struct Worker {
    std::mutex gate, exit_mutex;
    std::condition_variable cv, exit_cv;
    bool parked = false, release = false, delayed = false, read_grant = false;
    bool native_exit = false;
    tooie::context_observer::Token token{};
    int32_t object = 0;
};
std::array<Worker, 3> workers;
std::thread::id launcher;
thread_local bool callback_seen = false;
bool delayed_case = false;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void event(const char* name, tooie::Json extra = tooie::Json::object()) {
    extra["original_core1_executed"] = false;
    extra["idle_handoff_acceptance"] = false;
    tooie::trace(name, "cleanup-prerequisite", "observed", 0, nullptr, std::move(extra));
}
void callback(uint8_t*, recomp_context* ctx) {
    require(ctx->status_reg == 0 && !ctx->mips3_float_mode && ctx->f_odd == &ctx->f0.u32h,
            "Unexpected native fresh context");
    callback_seen = true;
}
void boundary(uint8_t* rdram, recomp_context* ctx) {
    auto index = static_cast<unsigned>(ctx->r4);
    require(index < workers.size(), "Bad diagnostic worker argument");
    auto& w = workers[index];
    require(callback_seen && std::this_thread::get_id() != launcher,
            "Diagnostic boundary did not use native dispatcher");
    require(ultramodern::this_thread() == w.object && osGetThreadId(rdram, 0) == int(index + 1),
            "Wrong native guest worker identity");
    event("worker_dispatched", {{"worker", index}, {"native_dispatch_callback", true},
          {"current_thread", tooie::hex32(ultramodern::this_thread())}});
    {
        std::unique_lock lock(w.gate);
        w.parked = true;
        w.cv.notify_all();
        w.cv.wait(lock, [&] { return w.release; });
        if (delayed_case) {
            w.delayed = true;
            w.cv.notify_all();
            w.cv.wait(lock, [&] { return w.read_grant; });
            require(MEM_W(0, sentinel) == int32_t(0x1234abcd), "RDRAM not retained for delayed worker");
            event("delayed_worker_rdram_read", {{"rdram_retained", true}, {"worker", index}});
        }
    }
    // The mutex remains locked until native exit, after the runtime wrapper's
    // cleanup enqueue. The controller cannot mistake boundary return for exit.
    std::unique_lock lock(w.exit_mutex);
    w.native_exit = true;
    std::notify_all_at_thread_exit(w.exit_cv, std::move(lock));
}
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::string scenario = argv[3];
    bool ordinary = scenario == "ordinary" || scenario == "already-consumed";
    delayed_case = scenario == "delayed-worker";
    unsigned count = scenario == "empty" ? 0 : scenario == "multiple-queued" ? 3 : 1;
    require(ordinary || delayed_case || scenario == "late-cleaner" || scenario == "multiple-queued" || scenario == "empty",
            "Unknown regression scenario");
    tooie::start_trace(argv[2]);
    tooie::start_watchdog(15);
    launcher = std::this_thread::get_id();
    auto identity = tooie::validate_and_install_rom(argv[1]);
    auto game = tooie::game_entry(identity.xxh3);
    game.thread_create_callback = callback;
    recomp::register_game(game);
    recomp::start_game(game.game_id, "");
    require(recomp::current_game_id() == game.game_id, "Active game missing");
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    uint8_t* rdram = memory.data();
    recomp_context ctx{};
    tooie::register_overlays();
    init(rdram, &ctx, (gpr)int32_t(0x80000400));
    MEM_W(0, sentinel) = int32_t(0x1234abcd);
    recomp::overlays::add_loaded_function(entry, boundary);
    event("scenario_started", {{"scenario", scenario}, {"workers", count}, {"correctness_sleeps", false}});
    if (ordinary) {
        ultramodern::init_thread_cleanup();
        event("cleaner_started_before_workers");
    }
    for (unsigned i = 0; i < count; ++i) {
        auto& w = workers[i];
        w.object = int32_t(0x80600000 + i * 0x400);
        osCreateThread(rdram, w.object, int(i + 1), entry, int32_t(i), int32_t(0x80500000 + i * 0x1000), 0);
        w.token = tooie::context_observer::watch_context(TO_PTR(OSThread, w.object)->context);
        osStartThread(rdram, w.object);
        std::unique_lock lock(w.gate);
        w.cv.wait(lock, [&] { return w.parked; });
    }
    event("new_workers_closed", {{"workers", count}});
    for (unsigned i = 0; i < count; ++i) {
        auto& w = workers[i];
        {
            std::unique_lock lock(w.gate);
            require(tooie::context_observer::deletion_count(w.token) == 0, "Parked context prematurely deleted");
            osDestroyThread(rdram, w.object);
            require(TO_PTR(OSThread, w.object)->context == nullptr, "External destroy did not detach context");
            event("worker_destroyed_while_parked", {{"worker", i}, {"rdram_retained", true}});
            w.release = true;
            w.cv.notify_all();
            if (delayed_case) {
                w.cv.wait(lock, [&] { return w.delayed; });
                require(tooie::context_observer::deletion_count(w.token) == 0, "Delayed context prematurely deleted");
                {
                    std::lock_guard exit_lock(w.exit_mutex);
                    require(!w.native_exit, "Delayed worker exited before memory gate");
                }
                event("release_deferred_for_delayed_worker", {{"worker", i}, {"native_exit", false},
                      {"rdram_retained", memory.data() == rdram}, {"mapping_retained", get_function(entry) == boundary}});
                w.read_grant = true;
                w.cv.notify_all();
            }
        }
        {
            std::unique_lock lock(w.exit_mutex);
            w.exit_cv.wait(lock, [&] { return w.native_exit; });
        }
        event("native_worker_exited", {{"worker", i}, {"notification", "std::notify_all_at_thread_exit"},
              {"cleanup_already_enqueued", true}});
    }
    event("all_cleanup_producers_quiescent", {{"workers", count}, {"no_future_producers", true}});
    if (ordinary) {
        for (unsigned i = 0; i < count; ++i) tooie::context_observer::wait_for_deletion(workers[i].token);
        event("queue_consumed_before_quit", {{"workers", count}});
    } else {
        for (unsigned i = 0; i < count; ++i)
            require(tooie::context_observer::deletion_count(workers[i].token) == 0, "Late queue unexpectedly consumed");
        event("completed_contexts_queued", {{"workers", count}});
    }
    ultramodern::quit();
    event("quit_after_native_exit");
    if (!ordinary) {
        ultramodern::init_thread_cleanup();
        event("cleaner_started_after_quit");
    }
    ultramodern::join_thread_cleaner_thread();
    event("cleaner_join_and_drain_returned");
    for (unsigned i = 0; i < count; ++i) {
        unsigned deletes = tooie::context_observer::deletion_count(workers[i].token);
        require(deletes == 1, "Expected exactly one context deletion; no salvage permitted");
        event("context_deleted_exactly_once", {{"worker", i}, {"deletion_count", deletes},
              {"join_proof", "context destruction completed with non-joinable std::thread"}});
    }
    recomp::overlays::add_loaded_function(entry, func_80013678);
    require(get_function(entry) == func_80013678, "Native function mapping restoration failed");
    event("native_mapping_restored");
    std::vector<uint8_t>().swap(memory);
    event("rdram_released_after_cleanup");
    event("cleanup_regression_pass", {{"scenario", scenario}, {"workers", count},
          {"salvage_used", false}, {"exactly_once_context_deletions", count}});
    tooie::stop_watchdog();
    return 0;
}
