#pragma once
#include "runtime_lifecycle.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

// App-owned continuous RDP FREEZE admission. This is not an RDP cycle model:
// an admitted parser runs to completion, then admission is checked again for DP.
namespace tooie::dp {
inline std::atomic<uint32_t> status{0x80}; // Pinned runtime BufferReady initial state.
inline std::mutex admission_mutex;
enum class Stage { parse, completion };
inline std::atomic<uint64_t> parse_waits{0},completion_waits{0},released{0},aborted{0};
inline std::atomic<uint64_t> freeze_sets{0},freeze_clears{0};
struct Counts { uint64_t parse_waits,completion_waits,released,aborted,freeze_sets,freeze_clears; uint32_t status; };
inline Counts counts() noexcept {
    return {parse_waits.load(),completion_waits.load(),released.load(),aborted.load(),freeze_sets.load(),freeze_clears.load(),status.load()};
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
inline uint32_t persistent_status() noexcept {
    std::lock_guard lock(admission_mutex);
    return status.load(std::memory_order_acquire);
}
inline void persistent_restore_status(uint32_t value) noexcept {
    std::lock_guard lock(admission_mutex);
    status.store(value, std::memory_order_release);
}
#endif
// The action must be short (empty parser admission or DP-message enqueue).
// Status writes use the same lock, so DP admission/enqueue has a defined order
// relative to FREEZE set/clear. No lock is held while waiting or parsing.
template<class Stop,class Action> bool admit(Stage stage,Stop&& stop,Action&& action) {
    if(!lifecycle::enabled()){action();return true;}
    bool waited=false;
    for(;;){
        {
            std::lock_guard lock(admission_mutex);
            if(!(status.load(std::memory_order_acquire)&2u)){
                if(waited && stop()){++aborted;return false;}
                action();
                if(waited)++released;
                return true;
            }
        }
        if(!waited){
            waited=true;
            if(stage==Stage::parse)++parse_waits;else ++completion_waits;
        }
        if(stop()){++aborted;return false;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace tooie::dp
