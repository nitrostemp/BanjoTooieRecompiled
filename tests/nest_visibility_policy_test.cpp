#include "nest_visibility_policy.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <map>

int main() {
    using tooie::nest_visibility::expanded_radius;

    // The child can extend beyond the body, and negative actor scale mirrors
    // it without reducing the required culling sphere.
    assert(expanded_radius(10.0f, 16.0f, -2.0f) == 32.0f);
    // A body sphere already large enough is retained.
    assert(expanded_radius(40.0f, 16.0f, 2.0f) == 40.0f);

    // Cache/memory validation failure must leave the original common-renderer
    // culling input untouched.
    assert(expanded_radius(10.0f, 0.0f, 2.0f) == 10.0f);
    assert(expanded_radius(10.0f, -1.0f, 2.0f) == 10.0f);
    assert(expanded_radius(10.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f) == 10.0f);
    assert(expanded_radius(10.0f, 16.0f, std::numeric_limits<float>::infinity()) == 10.0f);
    assert(expanded_radius(10.0f, std::numeric_limits<float>::max(), 2.0f) == 10.0f);
    assert(expanded_radius(-1.0f, 16.0f, 2.0f) == -1.0f);
    assert(std::isnan(expanded_radius(std::numeric_limits<float>::quiet_NaN(), 16.0f, 2.0f)));
    assert(expanded_radius(std::numeric_limits<float>::infinity(), 16.0f, 2.0f) ==
        std::numeric_limits<float>::infinity());

    std::map<std::uint32_t, std::uint32_t> words;
    std::map<std::uint32_t, std::uint16_t> halves;
    constexpr std::uint32_t linked = 0x80001000U;
    constexpr std::uint32_t vector = 0x80002000U;
    constexpr std::uint32_t begin = 0x80003000U;
    constexpr std::uint32_t metadata = begin + 0x80U;
    words[0x80136EE0U] = vector;
    words[vector] = 0x80U;
    words[vector + 4U] = begin;
    words[vector + 8U] = begin + 0x100U;
    words[vector + 0xCU] = begin + 0x100U;
    halves[linked + 0x1AU] = 0x20U; // index one after >> 5
    words[metadata] = linked;
    halves[metadata + 0x68U] = 1U;
    halves[metadata + 0x6AU] = 77U;
    const auto read_word = [&words](std::uint32_t address, std::uint32_t& value) {
        const auto it = words.find(address);
        if (it == words.end()) return false;
        value = it->second;
        return true;
    };
    const auto read_half = [&halves](std::uint32_t address, std::uint16_t& value) {
        const auto it = halves.find(address);
        if (it == halves.end()) return false;
        value = it->second;
        return true;
    };
    float cached = 0.0f;
    assert(tooie::nest_visibility::cached_child_radius(linked, read_word, read_half, cached));
    assert(cached == 77.0f);

    // A zero cache marker requests guest-side initialization, which native code
    // must not perform.  Restore it before each independent malformed case.
    halves[metadata + 0x68U] = 0U;
    assert(!tooie::nest_visibility::cached_child_radius(linked, read_word, read_half, cached));
    halves[metadata + 0x68U] = 1U;
    words[metadata] = linked + 4U;
    assert(!tooie::nest_visibility::cached_child_radius(linked, read_word, read_half, cached));
    words[metadata] = linked;
    // Reading the cached fields is insufficient: a vector's declared element
    // itself must be complete in its live range.
    words[vector + 8U] = metadata + 0x6CU;
    assert(!tooie::nest_visibility::cached_child_radius(linked, read_word, read_half, cached));
}
