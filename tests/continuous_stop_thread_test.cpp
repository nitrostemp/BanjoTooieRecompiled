#include "runtime_lifecycle.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern void run_next_thread(uint8_t*);
extern std::atomic_bool exited;
namespace {
constexpr int32_t target = int32_t(0x80001000u), controller = int32_t(0x80001100u), idle = int32_t(0x80001200u);
constexpr int32_t entry = int32_t(0x80400000u), queue = int32_t(0x80002000u), buffer = int32_t(0x80002100u);
std::string mode;
std::atomic_int entered{0}, resumed{0}, controlled{0};
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void worker(uint8_t* rdram, recomp_context*) {
    const int id = osGetThreadId(rdram, 0);
    if (id == 1) {
        ++entered;
        if (mode == "blocked") {
            require(osRecvMesg(rdram, queue, int32_t(0x80002200u), OS_MESG_BLOCK) == 0, "resumed receive failed");
            require(*TO_PTR(OSMesg, int32_t(0x80002200u)) == 0x51, "resumed receive payload mismatch");
        } else if (mode == "self-null" || mode == "self-explicit") {
            osStopThread(rdram, mode == "self-null" ? NULLPTR : target);
        }
        ++resumed;
        return;
    }
    if (id == 2) {
        OSThread* t = TO_PTR(OSThread, target);
        UltraThreadContext* original_context = t->context;
        if (mode == "blocked") {
            require(TO_PTR(OSMesgQueue, queue)->blocked_on_recv == target, "target did not block on receive");
            osStopThread(rdram, target);
            require(TO_PTR(OSMesgQueue, queue)->blocked_on_recv == NULLPTR, "stopped target remains in receive queue");
            require(osSendMesg(rdram, queue, 0x51, OS_MESG_NOBLOCK) == 0, "completion injection failed");
            require(resumed == 0, "stop unexpectedly woke blocked target");
        } else if (mode == "ready") {
            require(entered == 0, "ready target ran before controller");
            osStopThread(rdram, target);
            require(ultramodern::thread_queue_peek(rdram, ultramodern::running_queue) == idle, "stopped target remains runnable");
        } else {
            require(entered == 1 && resumed == 0, "self-stop did not suspend its native frame");
        }
        require(t->state == OSThreadState::STOPPED && t->context == original_context && original_context != nullptr,
                "stop changed native context or failed STOPPED state");
        osStopThread(rdram, target); // Already STOPPED is a no-op in original libultra.
        osStartThread(rdram, target);
        if (mode != "ready") require(resumed == 1, "higher-priority restart did not return to the suspended native frame");
        ++controlled;
        return;
    }
    while (true) { tooie::lifecycle::poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    mode = argv[1];
    if (mode != "blocked" && mode != "ready" && mode != "self-null" && mode != "self-explicit") return 2;
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    auto* rdram = memory.data();
    recomp::register_game({.rom_hash=0,.internal_name="TEST",.display_name="Stop/restart contract",.game_id=u8"continuous.stop"});
    recomp::start_game(u8"continuous.stop", "");
    recomp::overlays::add_loaded_function(entry, worker);
    tooie::lifecycle::enable();
    ultramodern::init_thread_cleanup();
    osCreateMesgQueue(rdram, queue, buffer, 1);
    osCreateThread(rdram, target, 1, entry, 0, int32_t(0x80006000u), 40);
    osCreateThread(rdram, controller, 2, entry, 0, int32_t(0x80007000u), mode == "ready" ? 60 : 30);
    osCreateThread(rdram, idle, 3, entry, 0, int32_t(0x80008000u), 1);
    for (auto address : {target, controller, idle}) ultramodern::schedule_running_thread(rdram, address);
    run_next_thread(rdram);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!tooie::lifecycle::stopping() && (resumed != 1 || controlled != 1) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tooie::lifecycle::request_stop();
    if (!tooie::lifecycle::wait_for_producers(std::chrono::seconds(3))) std::_Exit(70);
    exited.store(true);
    ultramodern::join_thread_cleaner_thread();
    const auto counts = tooie::lifecycle::counts();
    ultramodern::quit();
    if (entered != 1 || resumed != 1 || controlled != 1 || counts.created != 3 || counts.enqueued != 3 || counts.deleted != 3
        || counts.producers || tooie::lifecycle::failure()) {
        if (auto error = tooie::lifecycle::failure()) try { std::rethrow_exception(error); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        std::cerr << "FAIL stop/restart " << mode << '\n'; return 1;
    }
    std::cout << "PASS stop/restart " << mode << ": contexts preserved; created=enqueued=deleted=3\n";
}
