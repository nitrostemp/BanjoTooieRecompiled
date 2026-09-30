#pragma once

#include <array>
#include <cstdint>

namespace tooie::model_interpolation {

// Raw guest words only. The s0/position relation suggests an Actor-shaped
// candidate; it does not establish an actor's identity or field semantics.
struct ActorTraceSnapshot {
    bool actor_valid = false;
    bool linked_valid = false;
    std::array<std::uint32_t, 39> actor_words{};  // 0x9c bytes
    std::array<std::uint32_t, 12> linked_words{}; // 0x30 bytes
};

constexpr bool actor_trace_guest_range(std::uint32_t address,
    std::uint32_t bytes) noexcept {
    const auto segment = address & 0xE0000000U;
    if (segment != 0x80000000U && segment != 0xA0000000U) return false;
    if ((address & 3U) != 0) return false;
    constexpr std::uint32_t rdram_bytes = 0x00800000U;
    const auto offset = address & 0x1FFFFFFFU;
    return bytes <= rdram_bytes && offset <= rdram_bytes - bytes;
}

template <class ReadWord>
ActorTraceSnapshot snapshot_actor_candidate(std::uint32_t s0,
    std::uint32_t position, ReadWord read) noexcept {
    ActorTraceSnapshot result{};
    if (s0 > UINT32_MAX - 4U || position != s0 + 4U ||
        !actor_trace_guest_range(s0, 0x9CU)) return result;

    std::array<std::uint32_t, 39> actor{};
    for (std::uint32_t i = 0; i < actor.size(); ++i)
        if (!read(s0 + i * 4U, actor[i])) return result;
    result.actor_words = actor;
    result.actor_valid = true;

    // A raw linked structure, without assuming its type or identity.
    const auto linked = actor[0];
    if (!actor_trace_guest_range(linked, 0x30U)) return result;
    std::array<std::uint32_t, 12> linked_words{};
    for (std::uint32_t i = 0; i < linked_words.size(); ++i)
        if (!read(linked + i * 4U, linked_words[i])) return result;
    result.linked_words = linked_words;
    result.linked_valid = true;
    return result;
}

} // namespace tooie::model_interpolation
