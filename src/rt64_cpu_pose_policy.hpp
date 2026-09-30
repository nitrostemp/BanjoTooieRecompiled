#pragma once

#include "hle/rt64_workload.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace RT64 {

namespace {

struct TooiePoseSpan {
    std::size_t first = 0;
    std::size_t count = 0;
};

inline constexpr std::uint32_t tooieFirstCpuRootId = 0x60000000U;
inline constexpr std::uint32_t tooieLastCpuRootId = 0x7FFFFFFEU;

inline bool tooieUniqueCpuRootId(const DrawData &data,
    std::uint32_t id) noexcept {
    unsigned matches = 0;
    for (const auto groupIndex : data.worldTransformGroups) {
        if (groupIndex >= data.transformGroups.size()) return false;
        if (data.transformGroups[groupIndex].matrixId == id && ++matches > 1)
            return false;
    }
    return matches == 1;
}

inline bool tooiePoseRootSpan(const DrawData &data, std::uint32_t world,
    TooiePoseSpan &span) noexcept {
    if (world >= data.worldTransformVertexIndices.size() ||
        world >= data.worldTransformGroups.size()) return false;
    const auto first = std::size_t(data.worldTransformVertexIndices[world]);
    const auto end = world + 1U < data.worldTransformVertexIndices.size()
        ? std::size_t(data.worldTransformVertexIndices[world + 1U])
        : data.worldIndices.size();
    if (first > end || end > data.worldIndices.size() ||
        end - first == 0 || end - first > 4096U ||
        end > data.posFloats.size() / 3U ||
        end > data.tcFloats.size() / 2U) return false;
    for (auto i = first; i < end; ++i)
        if (data.worldIndices[i] != world) return false;
    span = {first, end - first};
    return true;
}

enum class TooiePoseFaceResult { found, end, invalid };

inline TooiePoseFaceResult tooieNextPoseFace(const DrawData &data,
    std::uint32_t world, TooiePoseSpan span, std::size_t &cursor,
    std::array<std::uint32_t, 3> &relative) noexcept {
    while (cursor < data.faceIndices.size()) {
        unsigned owned = 0;
        for (unsigned corner = 0; corner < 3; ++corner) {
            const auto index = data.faceIndices[cursor + corner];
            if (index >= data.worldIndices.size()) return TooiePoseFaceResult::invalid;
            if (data.worldIndices[index] == world) {
                ++owned;
                if (index < span.first || index >= span.first + span.count)
                    return TooiePoseFaceResult::invalid;
                relative[corner] = std::uint32_t(index - span.first);
            }
        }
        cursor += 3;
        if (owned != 0 && owned != 3) return TooiePoseFaceResult::invalid;
        if (owned == 3) return TooiePoseFaceResult::found;
    }
    return TooiePoseFaceResult::end;
}

} // namespace

// Conservative pair-only eligibility for RT64's ordered CPU vertex velocity.
// False retains the existing original-pose vertex fallback. Matching an actor
// root ID alone does not prove that local vertex indices still correspond.
inline bool tooieCpuPoseCorrespondence(const DrawData &current,
    const DrawData &previous, std::uint32_t curWorld,
    std::uint32_t prevWorld) noexcept {
    TooiePoseSpan curSpan{}, prevSpan{};
    if (!tooiePoseRootSpan(current, curWorld, curSpan) ||
        !tooiePoseRootSpan(previous, prevWorld, prevSpan) ||
        curSpan.count != prevSpan.count ||
        curSpan.first + curSpan.count > current.velFloats.size() / 3U)
        return false;
    const auto curGroupIndex = current.worldTransformGroups[curWorld];
    const auto prevGroupIndex = previous.worldTransformGroups[prevWorld];
    if (curGroupIndex >= current.transformGroups.size() ||
        prevGroupIndex >= previous.transformGroups.size()) return false;
    const auto &curGroup = current.transformGroups[curGroupIndex];
    const auto &prevGroup = previous.transformGroups[prevGroupIndex];
    if (curGroup.matrixId < tooieFirstCpuRootId ||
        curGroup.matrixId > tooieLastCpuRootId ||
        curGroup.matrixId != prevGroup.matrixId ||
        curGroup.ordering != G_EX_ORDER_LINEAR ||
        prevGroup.ordering != G_EX_ORDER_LINEAR ||
        !tooieUniqueCpuRootId(current, curGroup.matrixId) ||
        !tooieUniqueCpuRootId(previous, prevGroup.matrixId)) return false;

    for (std::size_t i = 0; i < curSpan.count; ++i) {
        const auto curVertex = curSpan.first + i;
        const auto prevVertex = prevSpan.first + i;
        double distanceSquared = 0.0;
        for (std::size_t component = 0; component < 3; ++component) {
            const float cur = current.posFloats[curVertex * 3U + component];
            const float prev = previous.posFloats[prevVertex * 3U + component];
            if (!std::isfinite(cur) || !std::isfinite(prev)) return false;
            const double delta = double(cur) - double(prev);
            distanceSquared += delta * delta;
        }
        if (distanceSquared > 256.0 * 256.0) return false;
        for (std::size_t component = 0; component < 2; ++component) {
            const float cur = current.tcFloats[curVertex * 2U + component];
            const float prev = previous.tcFloats[prevVertex * 2U + component];
            if (!std::isfinite(cur) || !std::isfinite(prev) || cur != prev)
                return false;
        }
    }

    constexpr std::size_t maxFaceIndices = 262144U;
    if (current.faceIndices.size() > maxFaceIndices ||
        previous.faceIndices.size() > maxFaceIndices ||
        current.faceIndices.size() % 3U != 0 ||
        previous.faceIndices.size() % 3U != 0) return false;
    std::size_t curCursor = 0, prevCursor = 0, matchedFaces = 0;
    for (;;) {
        std::array<std::uint32_t, 3> curFace{}, prevFace{};
        const auto curResult = tooieNextPoseFace(current, curWorld,
            curSpan, curCursor, curFace);
        const auto prevResult = tooieNextPoseFace(previous, prevWorld,
            prevSpan, prevCursor, prevFace);
        if (curResult == TooiePoseFaceResult::invalid ||
            prevResult == TooiePoseFaceResult::invalid ||
            curResult != prevResult) return false;
        if (curResult == TooiePoseFaceResult::end) return matchedFaces != 0;
        if (curFace != prevFace) return false;
        ++matchedFaces;
    }
}

// Remove velocities left by earlier matching before RT64 generates a CPU pose
// pair. This runs even when the new pair is ineligible or has identical poses.
// It only touches complete velocity triples owned by this world transform.
inline bool tooieClearCpuPoseVelocity(DrawData &current,
    std::uint32_t world) noexcept {
    if (world >= current.worldTransformVertexIndices.size() ||
        world >= current.worldTransformGroups.size()) return false;
    const auto groupIndex = current.worldTransformGroups[world];
    if (groupIndex >= current.transformGroups.size()) return false;
    const auto id = current.transformGroups[groupIndex].matrixId;
    if (id < tooieFirstCpuRootId || id > tooieLastCpuRootId) return false;

    const auto first = std::size_t(current.worldTransformVertexIndices[world]);
    const auto end = world + 1U < current.worldTransformVertexIndices.size()
        ? std::size_t(current.worldTransformVertexIndices[world + 1U])
        : current.worldIndices.size();
    if (first > end || end > current.worldIndices.size()) return false;
    const auto available = current.velFloats.size() / 3U;
    bool changed = false;
    for (auto vertex = first; vertex < end && vertex < available; ++vertex) {
        if (current.worldIndices[vertex] != world) continue;
        for (std::size_t component = 0; component < 3; ++component) {
            auto &velocity = current.velFloats[vertex * 3U + component];
            if (velocity != 0.0f) {
                velocity = 0.0f;
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace RT64
