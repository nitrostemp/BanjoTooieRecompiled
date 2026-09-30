#pragma once
#include <cstdint>

namespace tooie::rt64_match {
template <typename Workload>
bool same_projection_prefix(const Workload& current, const Workload& previous,
    uint32_t pair, uint32_t projection) noexcept {
    if (pair >= current.fbPairCount || pair >= previous.fbPairCount ||
        current.fbPairCount > current.fbPairs.size() ||
        previous.fbPairCount > previous.fbPairs.size()) return false;
    for (uint32_t f = 0; f <= pair; ++f) {
        const auto& a = current.fbPairs[f];
        const auto& b = previous.fbPairs[f];
        if (a.projectionCount > a.projections.size() ||
            b.projectionCount > b.projections.size()) return false;
        if (f < pair && a.projectionCount != b.projectionCount) return false;
        if (f == pair && (projection >= a.projectionCount ||
            projection >= b.projectionCount)) return false;
        const uint32_t count = f < pair ? a.projectionCount : projection + 1;
        for (uint32_t p = 0; p < count; ++p)
            if (a.projections[p].type != b.projections[p].type) return false;
    }
    return true;
}

inline bool allow_scene_pass_pair(bool stable_pass, uint32_t current_pair,
    uint32_t current_projection, uint32_t previous_pair, uint32_t previous_projection) noexcept {
    // Preserve a pass when its prefix is stable on either side of a candidate.
    // Later passes may appear/disappear without reopening cross-camera matches.
    // Reserving both sides also prevents an unstable view from stealing a
    // stable view's previous history during greedy candidate selection.
    return !stable_pass || (current_pair == previous_pair &&
        current_projection == previous_projection);
}
} // namespace tooie::rt64_match
