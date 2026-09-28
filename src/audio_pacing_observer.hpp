#pragma once
#include "audio_pacing_hooks.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace tooie::audio_pacing {
enum class Kind : uint32_t { query=1, queue=2, guest=3, opened=4 };
struct Record {
    Kind kind{};
    uint32_t flags=0;
    std::array<uint64_t,8> times{};
    std::array<uint64_t,12> values{};
};
struct Statistics {
    uint64_t capacity=0, attempted=0, emitted=0, overflow=0, invalid_guest=0;
};
// Main-thread initialization before producers; empty path leaves this disabled.
// Each session requires a fresh/empty directory. Capacity is fixed until finish.
void initialize(const std::filesystem::path& directory, size_t capacity=32768);
bool enabled() noexcept;
uint64_t now_ns() noexcept;
void append(const Record& record) noexcept;
// Explicit size enables focused boundary tests; production wrapper uses8MiB.
void observe_guest(const uint8_t* ram, size_t ram_bytes, const recomp_context* ctx, uint32_t site) noexcept;
// Only after all producer threads are joined. No output I/O occurs before here.
// Throws on output failure/active writer; retains storage instead of freeing it.
Statistics finish();
}
