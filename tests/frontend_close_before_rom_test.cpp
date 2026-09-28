#include "continuous_host.hpp"
#include "runtime_lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
        ("tooie-frontend-close-before-rom-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::atomic_bool stop_sent{false};
    std::jthread stopper([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!tooie::lifecycle::enabled() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (tooie::lifecycle::enabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            tooie::lifecycle::request_stop();
            stop_sent.store(true, std::memory_order_release);
        }
    });
    try {
        // The frontend has no selected ROM. This is the launcher close path
        // without SDL, so it exercises the real runtime startup wait and joins.
        tooie::ContinuousOptions options;
        options.runtime_directory = directory;
        options.observation_window = std::chrono::milliseconds(0);
        options.frontend = true;
        const bool completed = tooie::run_continuous_host(options);
        stopper.join();
        fs::remove_all(directory);
        if (!completed || !stop_sent.load(std::memory_order_acquire)) {
            std::cerr << "Frontend pre-ROM stop did not complete\n";
            return 1;
        }
        std::cout << "PASS frontend close before ROM: startup waiter and runtime resources joined\n";
        return 0;
    } catch (const std::exception& error) {
        stopper.join();
        fs::remove_all(directory);
        std::cerr << "Frontend pre-ROM stop failed: " << error.what() << '\n';
        return 1;
    }
}
