#include "virtual_clock.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(1);
}

std::chrono::nanoseconds wait_elapsed(std::chrono::milliseconds delay) {
    const auto before = tooie::timing::virtual_elapsed();
    std::this_thread::sleep_for(delay);
    return tooie::timing::virtual_elapsed() - before;
}
}

int main() {
    tooie::timing::reset_virtual_clock();
    const auto normal = wait_elapsed(20ms);
    if (normal < 10ms || normal > 100ms) fail("normal virtual clock rate is implausible");

    const auto before_fast = tooie::timing::virtual_elapsed();
    if (!tooie::timing::set_rate(2)) fail("rate transition was not accepted");
    const auto after_fast = tooie::timing::virtual_elapsed();
    if (after_fast < before_fast || after_fast - before_fast > 5ms) fail("rate change jumped guest time");
    const auto fast = wait_elapsed(20ms);
    if (fast < 25ms || fast > 160ms) fail("2x virtual clock did not advance proportionally");

    const auto target = tooie::timing::virtual_elapsed() + 60ms;
    const auto deadline = tooie::timing::host_deadline_for_virtual(target);
    const auto host_remaining = deadline - std::chrono::steady_clock::now();
    if (host_remaining < 20ms || host_remaining > 60ms) fail("virtual deadline was not rebased to host rate");

    const auto before_normal = tooie::timing::virtual_elapsed();
    if (!tooie::timing::set_rate(1)) fail("normal-rate transition was not accepted");
    const auto after_normal = tooie::timing::virtual_elapsed();
    if (after_normal < before_normal || after_normal - before_normal > 5ms) fail("restoring rate jumped guest time");
    const auto restored = wait_elapsed(20ms);
    if (restored < 10ms || restored > 100ms) fail("restored virtual clock rate is implausible");
}