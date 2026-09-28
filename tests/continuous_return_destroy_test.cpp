#include "runtime_lifecycle.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

extern void run_next_thread(uint8_t*);
extern std::atomic_bool exited;
namespace {
constexpr int32_t a = int32_t(0x80001000u), b = int32_t(0x80001100u), c = int32_t(0x80001200u);
constexpr int32_t entry = int32_t(0x80400000u);
std::atomic_int entered{0}, replaced{0};
void worker(uint8_t* rdram, recomp_context*) {
    const int id = osGetThreadId(rdram, 0);
    ++entered;
    if (id == 1 || id == 4) return;
    if (id == 2) {
        // Worker A was popped, ran and returned normally. Destroy then reuse
        // its guest OSThread while its old native cleanup may still be pending.
        osDestroyThread(rdram, a);
        osCreateThread(rdram, a, 4, entry, 0, int32_t(0x80009000u), 3);
        osStartThread(rdram, a);
        ++replaced;
        return;
    }
    while (true) {
        tooie::lifecycle::poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}
int main() {
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    auto* rdram = memory.data();
    recomp::register_game({.rom_hash=0,.internal_name="TEST",.display_name="Return/destroy replacement",.game_id=u8"continuous.return"});
    recomp::start_game(u8"continuous.return", "");
    recomp::overlays::add_loaded_function(entry, worker);
    tooie::lifecycle::enable();
    ultramodern::init_thread_cleanup();
    osCreateThread(rdram, a, 1, entry, 0, int32_t(0x80006000u), 3);
    osCreateThread(rdram, b, 2, entry, 0, int32_t(0x80007000u), 2);
    osCreateThread(rdram, c, 3, entry, 0, int32_t(0x80008000u), 1);
    for (auto address : {a, b, c}) ultramodern::schedule_running_thread(rdram, address);
    run_next_thread(rdram);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!tooie::lifecycle::stopping() && entered.load() != 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tooie::lifecycle::request_stop();
    if (!tooie::lifecycle::wait_for_producers(std::chrono::seconds(3))) std::_Exit(70);
    exited.store(true);
    ultramodern::join_thread_cleaner_thread();
    const auto counts = tooie::lifecycle::counts();
    const bool retained = recomp::current_game_id() == u8"continuous.return";
    ultramodern::quit();
    if (entered != 4 || replaced != 1 || counts.created != 4 || counts.enqueued != 4 || counts.deleted != 4
        || counts.producers || !retained || tooie::lifecycle::failure()) {
        std::cerr << "FAIL return/destroy/replace ownership contract\n"; return 1;
    }
    std::cout << "PASS three worker slots: natural return -> destroy -> replacement; created=enqueued=deleted=4\n";
}
