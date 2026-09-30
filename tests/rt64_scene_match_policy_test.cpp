#include "rt64_scene_match_policy.hpp"
#include <cassert>
#include <algorithm>
#include <array>
#include <vector>

struct Projection { int type; };
struct Pair { uint32_t projectionCount; std::vector<Projection> projections; };
struct Workload { uint32_t fbPairCount; std::vector<Pair> fbPairs; };

int main() {
    using namespace tooie::rt64_match;
    // Gloomy Caverns: main pass pair1/slot1 and secondary pair3/slot2
    // were swapped for thirteen guest frames despite stable draw-pass order.
    Workload current{4, {{1, {{0}}}, {2, {{0}, {1}}},
        {2, {{0}, {1}}}, {5, {{0}, {1}, {1}, {1}, {1}}}}};
    auto previous = current;
    assert(same_projection_prefix(current, previous, 3, 4));
    assert(allow_scene_pass_pair(true, 1, 1, 1, 1));
    assert(allow_scene_pass_pair(true, 3, 2, 3, 2));
    assert(!allow_scene_pass_pair(true, 1, 1, 3, 2));
    assert(!allow_scene_pass_pair(true, 3, 2, 1, 1));
    assert(!allow_scene_pass_pair(true, 3, 2, 3, 3));
    // Replay the observed ranking crossover: nearest-matrix candidates would
    // greedily choose 1->4 and 4->1. Pass provenance must preserve 1->1/4->4.
    struct Candidate { float score; uint32_t current, previous; };
    std::array<Candidate, 4> candidates{{{0.1f, 0, 1}, {0.2f, 1, 0},
        {1.0f, 0, 0}, {1.1f, 1, 1}}};
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.score < b.score; });
    constexpr uint32_t pairs[]{1, 3}, slots[]{1, 2}, transforms[]{1, 4};
    const auto select = [&](bool stable) {
        std::array<uint32_t, 2> mapped{0, 0};
        std::array<bool, 2> used{};
        for (const auto& c : candidates) {
            if (mapped[c.current] || used[c.previous] ||
                !allow_scene_pass_pair(stable, pairs[c.current], slots[c.current],
                    pairs[c.previous], slots[c.previous])) continue;
            mapped[c.current] = transforms[c.previous];
            used[c.previous] = true;
        }
        return mapped;
    };
    assert((select(true) == std::array<uint32_t, 2>{1, 4}));
    assert((select(false) == std::array<uint32_t, 2>{4, 1}));
    // Captured workload3721 keeps main pair1/slot1 but drops later pair3
    // projections. That tail change must not allow main -> old secondary.
    previous = current;
    previous.fbPairs[3].projectionCount = 2;
    assert(!allow_scene_pass_pair(same_projection_prefix(current, previous, 1, 1), 1, 1, 3, 2));
    // The reverse candidate must also reserve the proven main-view history.
    assert(same_projection_prefix(current, previous, 1, 1));
    assert(!same_projection_prefix(current, previous, 3, 2));
    assert(!allow_scene_pass_pair(
        same_projection_prefix(current, previous, 3, 2) ||
        same_projection_prefix(current, previous, 1, 1), 3, 2, 1, 1));
    // A missing target cannot be treated as an established pass.
    assert(!same_projection_prefix(current, previous, 3, 4));
    // Changes after the target within its own pair are harmless too.
    previous = current;
    previous.fbPairs[1].projections.push_back({1});
    ++previous.fbPairs[1].projectionCount;
    assert(same_projection_prefix(current, previous, 1, 1));
    previous = current;
    // Changed pass layout retains the existing distance-based fallback.
    previous.fbPairs[1].projectionCount = 1;
    assert(!same_projection_prefix(current, previous, 3, 4));
    assert(allow_scene_pass_pair(false, 1, 1, 3, 2));
    previous = current;
    previous.fbPairs[1].projections[1].type = 2;
    assert(!same_projection_prefix(current, previous, 3, 4));
    previous = current;
    previous.fbPairCount = 3;
    assert(!same_projection_prefix(current, previous, 3, 4));
    previous = current;
    // RT64 retains spare vector capacity: inactive slots aren't topology.
    previous.fbPairs.push_back({0, {}});
    previous.fbPairs[1].projections.push_back({2});
    assert(same_projection_prefix(current, previous, 3, 4));
}
