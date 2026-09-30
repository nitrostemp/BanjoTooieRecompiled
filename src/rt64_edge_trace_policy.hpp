#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tooie::rt64_edge_trace {

inline constexpr std::uint32_t presentation_calls = 420;
inline constexpr std::uint32_t sample_stride = 7;
inline constexpr std::uint32_t triangles_per_sample = 1024;
inline constexpr std::uint32_t max_rows_per_sample = 8;
inline constexpr std::size_t max_file_bytes = 1U << 20;

inline constexpr bool sample_enabled(std::uint32_t call) noexcept {
    return call < presentation_calls && call % sample_stride == 0;
}

inline constexpr std::uint32_t scan_count(std::uint32_t triangles,
    std::uint32_t workloads) noexcept {
    const std::uint32_t budget = triangles_per_sample / (workloads ? workloads : 1);
    return triangles < budget ? triangles : budget;
}

inline constexpr std::uint32_t scan_first(std::uint32_t sample,
    std::uint32_t count, std::uint32_t triangles) noexcept {
    return triangles ? std::uint32_t((std::uint64_t(sample) * count) % triangles) : 0;
}

template <std::size_t N, class T> struct TopEdges {
    struct Entry {
        float score = 0.0f;
        T value{};
    };
    std::array<Entry, N> values{};
    std::size_t count = 0;

    void add(float score, const T &value) noexcept {
        if (!std::isfinite(score) || score < 0.0f ||
            (count == N && score <= values[N - 1].score)) return;
        std::size_t at = 0;
        while (at < count && values[at].score >= score) ++at;
        const std::size_t last = count < N ? count++ : N - 1;
        for (std::size_t i = last; i > at; --i) values[i] = values[i - 1];
        values[at] = {score, value};
    }
};

} // namespace tooie::rt64_edge_trace
