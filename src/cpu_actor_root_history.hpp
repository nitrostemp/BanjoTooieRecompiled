#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tooie::model_interpolation {

// Owner and descriptor identify an Actor root. Source-bracketed attachment
// paths use a fixed role in descriptor and the resolved model as
// source_identity. The transient dbanim vertex-list pointer is deliberately
// not part of this key because it alternates every task.
struct CpuActorRootKey {
    std::uint32_t owner = 0;
    std::uint32_t descriptor = 0;
    std::uint64_t epoch = 0;
    std::uint32_t source_identity = 0;

    constexpr bool operator==(const CpuActorRootKey&) const noexcept = default;
};

struct CpuActorRoot {
    CpuActorRootKey key{};
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Fixed-size task history. An ID is carried only from the immediately prior
// task when its key is unique on both sides and the root moved at most 256.
class CpuActorRootHistory {
public:
    static constexpr std::size_t capacity = 128;
    static constexpr std::uint32_t first_id = 0x60000000U;
    static constexpr std::uint32_t last_id = 0x7FFFFFFEU;

    explicit constexpr CpuActorRootHistory(std::uint32_t initial_id = first_id) noexcept
        : next_id_(initial_id >= first_id && initial_id <= last_id
              ? initial_id : last_id + 1U) {}

    // Call once per submitted graphics task. Output is always zeroed first.
    // Empty or invalid batches erase history; reset never rewinds the ID pool.
    bool assign(std::span<const CpuActorRoot> current,
        std::span<std::uint32_t> ids) noexcept {
        std::fill(ids.begin(), ids.end(), 0U);
        if (current.size() != ids.size() || current.size() > capacity) {
            previous_count_ = 0;
            return false;
        }
        for (const auto &root : current) {
            if (!std::isfinite(root.x) || !std::isfinite(root.y) ||
                !std::isfinite(root.z)) {
                previous_count_ = 0;
                return false;
            }
        }

        std::array<Entry, capacity> next{};
        for (std::size_t i = 0; i < current.size(); ++i) {
            next[i].root = current[i];
            std::size_t current_matches = 0;
            for (const auto &candidate : current)
                current_matches += candidate.key == current[i].key;
            if (current_matches != 1) continue;

            std::size_t previous_matches = 0;
            const Entry *prior = nullptr;
            for (std::size_t j = 0; j < previous_count_; ++j) {
                if (previous_[j].root.key == current[i].key) {
                    ++previous_matches;
                    prior = &previous_[j];
                }
            }
            if (previous_matches > 1) continue;
            if (previous_matches == 1 && prior->id != 0 &&
                nearby(current[i], prior->root)) {
                next[i].id = prior->id;
            } else {
                next[i].id = allocate_id();
            }
            ids[i] = next[i].id;
        }
        previous_ = next;
        previous_count_ = current.size();
        return true;
    }

    void reset() noexcept { previous_count_ = 0; }

private:
    struct Entry {
        CpuActorRoot root{};
        std::uint32_t id = 0;
    };
    static bool nearby(const CpuActorRoot &a, const CpuActorRoot &b) noexcept {
        const double dx = double(a.x) - double(b.x);
        const double dy = double(a.y) - double(b.y);
        const double dz = double(a.z) - double(b.z);
        return dx * dx + dy * dy + dz * dz <= 256.0 * 256.0;
    }
    std::uint32_t allocate_id() noexcept {
        if (next_id_ > last_id) return 0;
        return next_id_++;
    }

    std::array<Entry, capacity> previous_{};
    std::size_t previous_count_ = 0;
    std::uint32_t next_id_ = first_id;
};

} // namespace tooie::model_interpolation
