#include "model_actor_trace.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using tooie::model_interpolation::snapshot_actor_candidate;
    constexpr std::uint32_t actor = 0x80010000U;
    constexpr std::uint32_t marker = 0xA0020000U;
    unsigned reads = 0;
    auto read = [&](std::uint32_t address, std::uint32_t &word) {
        ++reads;
        if (address >= actor && address < actor + 0x9CU) {
            word = address == actor ? marker : 0x11110000U + (address - actor) / 4U;
            return true;
        }
        if (address >= marker && address < marker + 0x30U) {
            word = 0x22220000U + (address - marker) / 4U;
            return true;
        }
        return false;
    };
    auto result = snapshot_actor_candidate(actor, actor + 4U, read);
    assert(result.actor_valid && result.linked_valid);
    assert(reads == 39 + 12);
    assert(result.actor_words[0] == marker);
    assert(result.actor_words[38] == 0x11110026U);
    assert(result.linked_words[0] == 0x22220000U);
    assert(result.linked_words[11] == 0x2222000BU);

    constexpr std::uint32_t last_actor = 0xA07FFF64U;
    reads = 0;
    auto at_end = [&](std::uint32_t address, std::uint32_t &word) {
        ++reads;
        word = address == last_actor ? 0xA07FFFD0U : address;
        return true;
    };
    result = snapshot_actor_candidate(last_actor, last_actor + 4U, at_end);
    assert(result.actor_valid && result.linked_valid && reads == 39 + 12);
    assert(result.actor_words[38] == last_actor + 0x98U);
    assert(result.linked_words[11] == 0xA07FFFFCU);

    auto reject = [&](std::uint32_t base, std::uint32_t position) {
        reads = 0;
        result = snapshot_actor_candidate(base, position, read);
        assert(!result.actor_valid && !result.linked_valid && reads == 0);
        for (auto word : result.actor_words) assert(word == 0);
        for (auto word : result.linked_words) assert(word == 0);
    };
    reject(actor, actor + 8U);                // Mere proximity is not provenance.
    reject(0xFFFFFFFCU, 0U);                 // No wraparound on s0 + 4.
    reject(actor + 1U, actor + 5U);          // Whole actor must be word aligned.
    reject(0x00010000U, 0x00010004U);       // Not a KSEG0/1 pointer.
    reject(0xC0010000U, 0xC0010004U);
    reject(0x807FFF68U, 0x807FFF6CU);       // Last word crosses 8 MiB.
    reject(0xA07FFF68U, 0xA07FFF6CU);

    reads = 0;
    auto partial_actor = [&](std::uint32_t address, std::uint32_t &word) {
        if (address == actor + 20U) return false;
        return read(address, word);
    };
    result = snapshot_actor_candidate(actor, actor + 4U, partial_actor);
    assert(!result.actor_valid && !result.linked_valid && reads == 5);
    for (auto word : result.actor_words) assert(word == 0);
    for (auto word : result.linked_words) assert(word == 0);

    auto partial_marker = [&](std::uint32_t address, std::uint32_t &word) {
        if (address == marker + 12U) return false;
        return read(address, word);
    };
    reads = 0;
    result = snapshot_actor_candidate(actor, actor + 4U, partial_marker);
    assert(result.actor_valid && !result.linked_valid && reads == 39 + 3);
    assert(result.actor_words[0] == marker);
    for (auto word : result.linked_words) assert(word == 0);

    auto bad_marker = [&](std::uint32_t address, std::uint32_t &word) {
        if (address == actor) { ++reads; word = 0xA07FFFD4U; return true; }
        return read(address, word);
    };
    reads = 0;
    result = snapshot_actor_candidate(actor, actor + 4U, bad_marker);
    assert(result.actor_valid && !result.linked_valid && reads == 39);
    for (auto word : result.linked_words) assert(word == 0);
}
