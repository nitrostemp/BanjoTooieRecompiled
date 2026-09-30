#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace tooie::nest_visibility {

// The nest's body is culled before its separately submitted child. Preserve the
// caller-provided sphere unless the child metadata supplied a usable radius.
inline float expanded_radius(float existing_scaled_radius, float child_radius,
    float actor_scale) noexcept {
    // This wraps a common renderer input.  A malformed original input remains
    // the renderer's responsibility; do not turn it into a new valid sphere.
    if (!std::isfinite(existing_scaled_radius) || existing_scaled_radius < 0.0f ||
        !std::isfinite(child_radius) || child_radius <= 0.0f ||
        !std::isfinite(actor_scale)) return existing_scaled_radius;
    const float child_scaled_radius = child_radius * std::fabs(actor_scale);
    if (!std::isfinite(child_scaled_radius)) return existing_scaled_radius;
    return std::fmax(existing_scaled_radius, child_scaled_radius);
}

// The child model metadata is cached lazily by guest code.  This native path
// only reads an already initialized cache and rejects every malformed address
// or vector layout, so it never changes the guest's model/vector state.
template <typename ReadWord, typename ReadHalf>
inline bool cached_child_radius(std::uint32_t linked, ReadWord&& read_word,
    ReadHalf&& read_half, float& radius) noexcept {
    constexpr std::uint32_t vector_address = 0x80136EE0U;
    constexpr std::uint32_t metadata_size = 0x6CU;
    constexpr std::uint32_t initialized_offset = 0x68U;
    constexpr std::uint32_t radius_offset = 0x6AU;
    if (linked == 0 || linked > std::numeric_limits<std::uint32_t>::max() - 0x1AU)
        return false;

    std::uint16_t child_index_word = 0;
    std::uint32_t vector = 0;
    if (!read_half(linked + 0x1AU, child_index_word) ||
        !read_word(vector_address, vector) || vector == 0) return false;
    if (vector > std::numeric_limits<std::uint32_t>::max() - 0xCU) return false;

    std::uint32_t stride = 0, begin = 0, end = 0, memend = 0;
    if (!read_word(vector, stride) || !read_word(vector + 4U, begin) ||
        !read_word(vector + 8U, end) || !read_word(vector + 0xCU, memend) ||
        stride < metadata_size || begin > end || end > memend) return false;

    const std::uint64_t offset = std::uint64_t(child_index_word >> 5U) * stride;
    const std::uint64_t metadata = std::uint64_t(begin) + offset;
    if (metadata > std::numeric_limits<std::uint32_t>::max() ||
        metadata + stride > end) return false;
    const auto metadata_address = static_cast<std::uint32_t>(metadata);

    std::uint32_t owner = 0;
    std::uint16_t initialized = 0, cached_radius = 0;
    if (!read_word(metadata_address, owner) || owner != linked ||
        !read_half(metadata_address + initialized_offset, initialized) ||
        initialized == 0 ||
        !read_half(metadata_address + radius_offset, cached_radius) ||
        cached_radius == 0) return false;
    radius = static_cast<float>(cached_radius);
    return true;
}

} // namespace tooie::nest_visibility
