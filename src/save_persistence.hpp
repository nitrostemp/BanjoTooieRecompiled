#pragma once
#include <atomic>
#include <cstdint>

namespace tooie::save_persistence {
// Updated under the runtime's save-buffer lock. A receipt covers exactly the
// snapshot written and successfully finalized, not merely a queued write.
inline std::atomic_uint64_t written{0}, persisted{0}, failed{0};
inline void buffer_changed() noexcept { written.fetch_add(1, std::memory_order_release); }
inline std::uint64_t generation() noexcept { return written.load(std::memory_order_acquire); }
inline void acknowledge(std::uint64_t generation, bool success) noexcept {
    (success ? persisted : failed).store(generation, std::memory_order_release);
}
inline bool complete(std::uint64_t generation) noexcept {
    return generation != 0 && persisted.load(std::memory_order_acquire) >= generation;
}
inline bool unsuccessful(std::uint64_t generation) noexcept {
    return generation != 0 && failed.load(std::memory_order_acquire) >= generation;
}
}
