#include "rt64_matrix_trace.hpp"

#include "hle/rt64_workload_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>

namespace RT64 {
namespace {

std::atomic<uint32_t> screenXTraceArm{0};
std::filesystem::path screenXTraceDirectory;
std::ofstream screenXTraceFile;
size_t screenXTraceBytes = 0;
constexpr size_t screenXTraceByteCap = 4U << 20;

float maxAbsDifference(const hlslpp::float4x4 &a, const hlslpp::float4x4 &b,
    int firstColumn = 0) noexcept {
    float result = 0.0f;
    for (int row = 0; row < 4; ++row) {
        for (int column = firstColumn; column < 4; ++column) {
            result = std::max(result, std::fabs(a[row][column] - b[row][column]));
        }
    }
    return result;
}

struct ProjectionSample {
    uint32_t workload = 0;
    uint32_t index = 0;
    uint32_t group = 0;
    uint32_t previous = UINT32_MAX;
    float rawX = 0.0f, rawY = 0.0f, rawZ = 0.0f;
    float finalX = 0.0f, finalY = 0.0f, finalZ = 0.0f;
    bool present = false;
};

struct WorldSample {
    uint32_t workload = 0;
    uint32_t index = 0;
    uint32_t group = 0;
    uint32_t previous = UINT32_MAX;
    float rawX = 0.0f, rawY = 0.0f, rawZ = 0.0f;
    float finalX = 0.0f, finalY = 0.0f, finalZ = 0.0f;
};

struct TraceSummary {
    uint32_t projections = 0, mappedProjections = 0, projectionEndpointChecks = 0;
    uint32_t worlds = 0, mappedWorlds = 0, missingWorldFinals = 0;
    float projectionEndpointMax = 0.0f, projectionPriorEndpointMax = 0.0f;
    float projectionSourceDeltaMax = 0.0f, projectionSplitGapMax = 0.0f;
    float projectionFinalDeltaMax = 0.0f;
    float worldCurrentEndpointMax = 0.0f, worldPriorEndpointMax = 0.0f;
    float worldSourceDeltaMax = 0.0f, worldFinalDeltaMax = 0.0f;
    WorldSample worstWorldEndpoint{};
    ProjectionSample firstProjection{};
};

struct TriangleSummary {
    uint32_t checked = 0;
    uint32_t skipped = 0;
    uint32_t workload = 0;
    uint32_t triangle = 0;
    uint32_t firstVertex = 0, secondVertex = 0;
    uint32_t firstMatrix = 0, secondMatrix = 0;
    uint32_t firstGroup = 0, secondGroup = 0;
    float maxRatio = 1.0f;
    float maxGrowthAny = 0.0f;
    float rawLength = 0.0f, finalLength = 0.0f, previousSameLocalLength = 0.0f;
    bool edgeVelocityUsed = false;
    bool previousValid = false;
    bool previousLocalReliable = false;
};

float pointDistance(const hlslpp::float4 &a, const hlslpp::float4 &b) noexcept {
    const float dx = float(a.x) - float(b.x);
    const float dy = float(a.y) - float(b.y);
    const float dz = float(a.z) - float(b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint32_t worldGroupId(const DrawData &data, uint32_t matrixIndex) noexcept {
    if (matrixIndex >= data.worldTransformGroups.size()) return 0;
    const uint32_t groupIndex = data.worldTransformGroups[matrixIndex];
    return groupIndex < data.transformGroups.size() ? data.transformGroups[groupIndex].matrixId : 0;
}

void traceScreenX(const WorkloadQueue &queue, const GameFrame &frame,
    uint32_t call, float weight, bool projectionsProcessed,
    bool transformsProcessed) noexcept {
    if (!screenXTraceFile || call >= 420 || call % 7 != 0) return;

    // Compare the same current vertices under all four matrix combinations.
    // This distinguishes a world-history mismatch from a camera-history
    // mismatch without assuming that an AUTO matrix identifies an object.
    for (uint32_t w : frame.workloads) {
        if (w >= queue.workloads.size()) continue;
        const Workload &workload = queue.workloads[w];
        const DrawData &data = workload.drawData;
        const auto *workloadMap = w < frame.frameMap.workloads.size()
            ? &frame.frameMap.workloads[w] : nullptr;
        const uint64_t previousWorkloadId = workloadMap && workloadMap->mapped &&
            workloadMap->prevWorkloadIndex < queue.workloads.size()
            ? queue.workloads[workloadMap->prevWorkloadIndex].workloadId : 0;
        const uint32_t matrixLimit = uint32_t(std::min<size_t>(
            data.worldTransforms.size(), 256));
        for (uint32_t m = 0; m < matrixLimit; ++m) {
            if (m >= data.worldTransformVertexIndices.size()) continue;
            const size_t first = data.worldTransformVertexIndices[m];
            const size_t count = data.worldTransformVertexCount(m);
            if (!count || first > data.vertexCount() || count > data.vertexCount() - first)
                continue;
            const auto &rawWorld = data.worldTransforms[m];
            const auto &finalWorld = transformsProcessed && m < data.lerpWorldTransforms.size()
                ? data.lerpWorldTransforms[m] : rawWorld;
            double sumX[4]{}, sumY[4]{};
            uint32_t samples = 0;
            uint32_t projection = UINT32_MAX;
            bool mixedProjection = false;
            const size_t stride = std::max<size_t>(1, count / 32);
            for (size_t v = first; v < first + count; v += stride) {
                if (v >= data.worldIndices.size() || data.worldIndices[v] != m ||
                    v >= data.viewProjIndices.size() || v * 3 + 2 >= data.posFloats.size())
                    continue;
                const uint32_t p = data.viewProjIndices[v];
                if (p >= data.viewProjTransforms.size() || p >= data.rspViewports.size())
                    continue;
                if (projection != UINT32_MAX && p != projection) {
                    mixedProjection = true;
                    continue;
                }
                const auto &rawProjection = data.viewProjTransforms[p];
                const auto &finalProjection = projectionsProcessed &&
                    p < data.modViewProjTransforms.size()
                    ? data.modViewProjTransforms[p] : rawProjection;
                const auto &viewport = data.rspViewports[p];
                const size_t offset = v * 3;
                const hlslpp::float4 rawLocal(data.posFloats[offset],
                    data.posFloats[offset + 1], data.posFloats[offset + 2], 1.0f);
                auto finalLocal = rawLocal;
                if (offset + 2 < data.velFloats.size())
                    finalLocal -= hlslpp::float4(data.velFloats[offset],
                        data.velFloats[offset + 1], data.velFloats[offset + 2], 0.0f)
                        * (1.0f - weight);
                const hlslpp::float4 points[4] = {
                    hlslpp::mul(hlslpp::mul(rawLocal, rawWorld), rawProjection),
                    hlslpp::mul(hlslpp::mul(finalLocal, finalWorld), finalProjection),
                    hlslpp::mul(hlslpp::mul(finalLocal, finalWorld), rawProjection),
                    hlslpp::mul(hlslpp::mul(rawLocal, rawWorld), finalProjection)
                };
                bool valid = true;
                for (const auto &point : points)
                    valid &= std::isfinite(float(point.w)) &&
                        std::fabs(float(point.w)) > 0.0001f;
                if (!valid) continue;
                double xs[4]{}, ys[4]{};
                for (uint32_t kind = 0; kind < 4; ++kind) {
                    xs[kind] = double(viewport.translate.x) +
                        double(viewport.scale.x) * double(points[kind].x) /
                            double(points[kind].w);
                    ys[kind] = double(viewport.translate.y) -
                        double(viewport.scale.y) * double(points[kind].y) /
                            double(points[kind].w);
                    if (!std::isfinite(xs[kind]) || !std::isfinite(ys[kind])) {
                        valid = false; break;
                    }
                }
                if (!valid) continue;
                for (uint32_t kind = 0; kind < 4; ++kind) {
                    sumX[kind] += xs[kind];
                    sumY[kind] += ys[kind];
                }
                projection = p;
                ++samples;
            }
            if (!samples) continue;
            const auto *map = workloadMap && workloadMap->mapped &&
                m < workloadMap->transforms.size()
                ? &workloadMap->transforms[m] : nullptr;
            const uint32_t physical = m < data.worldTransformPhysicalAddresses.size()
                ? data.worldTransformPhysicalAddresses[m] : 0;
            const auto *projectionMap = workloadMap && workloadMap->mapped &&
                projection < workloadMap->viewProjections.size()
                ? &workloadMap->viewProjections[projection] : nullptr;
            const uint32_t projectionGroup = projection < data.viewProjTransformGroups.size()
                ? data.viewProjTransformGroups[projection] : UINT32_MAX;
            const uint32_t projectionGroupId = projectionGroup < data.transformGroups.size()
                ? data.transformGroups[projectionGroup].matrixId : UINT32_MAX;
            char line[512];
            const int length = std::snprintf(line, sizeof(line),
                "%u,%.5f,%llu,%llu,%u,%u,%u,%08x,%08x,%08x,%zu,%u,%u,%u,%u,%u,%u,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4g,%.4g,%.4g\n",
                call, weight, static_cast<unsigned long long>(workload.workloadId),
                static_cast<unsigned long long>(previousWorkloadId), w, m, projection,
                worldGroupId(data, m), projectionGroupId, physical,
                count, samples, unsigned(mixedProjection),
                unsigned(map && map->mapped),
                map && map->mapped ? map->prevTransformIndex : UINT32_MAX,
                unsigned(projectionMap && projectionMap->mapped),
                projectionMap && projectionMap->mapped
                    ? projectionMap->prevTransformIndex : UINT32_MAX,
                sumX[0] / samples, sumY[0] / samples,
                sumX[1] / samples, sumY[1] / samples,
                sumX[2] / samples, sumY[2] / samples,
                sumX[3] / samples, sumY[3] / samples,
                float(rawWorld[3][0]), float(rawWorld[3][1]),
                float(rawWorld[3][2]));
            if (length <= 0 || size_t(length) >= sizeof(line) ||
                size_t(length) > screenXTraceByteCap - screenXTraceBytes) {
                screenXTraceFile.close();
                return;
            }
            screenXTraceFile.write(line, length);
            screenXTraceBytes += size_t(length);
        }
    }
}

void traceTriangleEdges(const WorkloadQueue &queue, const GameFrame &frame,
    uint32_t sampleOrdinal, float weight, bool processed, TriangleSummary &summary) noexcept {
    const uint32_t workloadCount = uint32_t(std::max<size_t>(1, frame.workloads.size()));
    const uint32_t budget = 2048u / workloadCount;
    if (budget == 0) return;
    for (uint32_t w : frame.workloads) {
        if (w >= queue.workloads.size()) continue;
        const DrawData &data = queue.workloads[w].drawData;
        const GameFrameMap::WorkloadMap *workloadMap = w < frame.frameMap.workloads.size()
            ? &frame.frameMap.workloads[w] : nullptr;
        const DrawData *previousData = workloadMap && workloadMap->mapped &&
            workloadMap->prevWorkloadIndex < queue.workloads.size()
            ? &queue.workloads[workloadMap->prevWorkloadIndex].drawData : nullptr;
        const uint32_t triangleCount = uint32_t(data.faceIndices.size() / 3);
        if (triangleCount == 0) continue;
        const uint32_t count = std::min(budget, triangleCount);
        const uint32_t first = uint32_t((uint64_t(sampleOrdinal) * budget) % triangleCount);
        for (uint32_t t = 0; t < count; ++t) {
            const uint32_t triangle = (first + t) % triangleCount;
            uint32_t vertices[3]{};
            uint32_t matrices[3]{};
            hlslpp::float4 rawPoints[3];
            hlslpp::float4 finalPoints[3];
            hlslpp::float4 previousPoints[3];
            bool vertexVelocities[3]{};
            bool vertexLocalReliable[3]{};
            bool valid = true;
            bool previousValid = previousData != nullptr;
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = data.faceIndices[size_t(triangle) * 3 + corner];
                if (vertex >= data.worldIndices.size() ||
                    size_t(vertex) * 3 + 2 >= data.posFloats.size()) {
                    valid = false;
                    break;
                }
                const uint32_t matrix = data.worldIndices[vertex];
                if (matrix >= data.worldTransforms.size()) {
                    valid = false;
                    break;
                }
                vertices[corner] = vertex;
                matrices[corner] = matrix;
                const size_t p = size_t(vertex) * 3;
                const auto &rawMatrix = data.worldTransforms[matrix];
                const auto &finalMatrix = (processed && matrix < data.lerpWorldTransforms.size())
                    ? data.lerpWorldTransforms[matrix] : rawMatrix;
                rawPoints[corner] = hlslpp::mul(hlslpp::float4(
                    data.posFloats[p], data.posFloats[p + 1], data.posFloats[p + 2], 1.0f), rawMatrix);
                const bool velocityAvailable = p + 2 < data.velFloats.size();
                const float vx = velocityAvailable ? data.velFloats[p] : 0.0f;
                const float vy = velocityAvailable ? data.velFloats[p + 1] : 0.0f;
                const float vz = velocityAvailable ? data.velFloats[p + 2] : 0.0f;
                vertexVelocities[corner] = vx != 0.0f || vy != 0.0f || vz != 0.0f;
                finalPoints[corner] = hlslpp::mul(hlslpp::float4(
                    data.posFloats[p] - vx * (1.0f - weight),
                    data.posFloats[p + 1] - vy * (1.0f - weight),
                    data.posFloats[p + 2] - vz * (1.0f - weight), 1.0f), finalMatrix);
                if (!std::isfinite(float(rawPoints[corner].w)) ||
                    !std::isfinite(float(finalPoints[corner].w)) ||
                    std::fabs(float(rawPoints[corner].w) - 1.0f) > 0.0001f ||
                    std::fabs(float(finalPoints[corner].w) - 1.0f) > 0.0001f) {
                    valid = false;
                    break;
                }
                if (previousValid) {
                    if (matrix >= workloadMap->transforms.size() ||
                        !workloadMap->transforms[matrix].mapped ||
                        workloadMap->transforms[matrix].prevTransformIndex >= previousData->worldTransforms.size()) {
                        previousValid = false;
                    }
                    else {
                        const auto &previousMatrix = previousData->worldTransforms[
                            workloadMap->transforms[matrix].prevTransformIndex];
                        previousPoints[corner] = hlslpp::mul(hlslpp::float4(
                            data.posFloats[p] - vx, data.posFloats[p + 1] - vy,
                            data.posFloats[p + 2] - vz, 1.0f), previousMatrix);
                        if (!std::isfinite(float(previousPoints[corner].w)) ||
                            std::fabs(float(previousPoints[corner].w) - 1.0f) > 0.0001f)
                            previousValid = false;
                    }
                }
                vertexLocalReliable[corner] = matrix < data.worldTransformGroups.size() &&
                    data.worldTransformGroups[matrix] < data.transformGroups.size() &&
                    data.transformGroups[data.worldTransformGroups[matrix]].vertexInterpolation != G_EX_COMPONENT_SKIP;
            }
            if (!valid) {
                ++summary.skipped;
                continue;
            }
            ++summary.checked;
            for (uint32_t edge = 0; edge < 3; ++edge) {
                const uint32_t next = (edge + 1) % 3;
                const float rawLength = pointDistance(rawPoints[edge], rawPoints[next]);
                const float finalLength = pointDistance(finalPoints[edge], finalPoints[next]);
                if (!std::isfinite(rawLength) || !std::isfinite(finalLength) || rawLength < 1.0f) continue;
                const float ratio = finalLength / rawLength;
                summary.maxGrowthAny = std::max(summary.maxGrowthAny, finalLength - rawLength);
                if (ratio > summary.maxRatio) {
                    summary.maxRatio = ratio;
                    summary.workload = w;
                    summary.triangle = triangle;
                    summary.firstVertex = vertices[edge];
                    summary.secondVertex = vertices[next];
                    summary.firstMatrix = matrices[edge];
                    summary.secondMatrix = matrices[next];
                    summary.firstGroup = worldGroupId(data, matrices[edge]);
                    summary.secondGroup = worldGroupId(data, matrices[next]);
                    summary.rawLength = rawLength;
                    summary.finalLength = finalLength;
                    summary.previousValid = previousValid;
                    summary.previousLocalReliable = previousValid &&
                        vertexLocalReliable[edge] && vertexLocalReliable[next];
                    summary.previousSameLocalLength = previousValid
                        ? pointDistance(previousPoints[edge], previousPoints[next]) : 0.0f;
                    summary.edgeVelocityUsed = vertexVelocities[edge] || vertexVelocities[next];
                }
            }
        }
    }
}

void traceProjection(const WorkloadQueue &queue, const GameFrame &curFrame,
    const GameScene &scene, float curWeight, bool processed,
    TraceSummary &summary) noexcept {
    for (const auto &indices : scene.projections) {
        if (indices.workloadIndex >= queue.workloads.size()) continue;
        const Workload &workload = queue.workloads[indices.workloadIndex];
        if (indices.fbPairIndex >= workload.fbPairs.size()) continue;
        const auto &fbPair = workload.fbPairs[indices.fbPairIndex];
        if (indices.projectionIndex >= fbPair.projections.size()) continue;
        const Projection &projection = fbPair.projections[indices.projectionIndex];
        if (projection.scissorRect.isNull()) continue;
        const DrawData &data = workload.drawData;
        const uint32_t index = projection.transformsIndex;
        if (index >= data.viewProjTransforms.size()) continue;
        const auto &raw = data.viewProjTransforms[index];
        const auto &final = (processed && index < data.modViewProjTransforms.size())
            ? data.modViewProjTransforms[index] : raw;
        ++summary.projections;
        summary.projectionFinalDeltaMax = std::max(summary.projectionFinalDeltaMax,
            maxAbsDifference(final, raw));
        if (index < data.viewTransforms.size() && index < data.projTransforms.size()) {
            summary.projectionSplitGapMax = std::max(summary.projectionSplitGapMax,
                maxAbsDifference(hlslpp::mul(data.viewTransforms[index], data.projTransforms[index]), raw, 1));
        }
        const GameFrameMap::WorkloadMap *workloadMap =
            indices.workloadIndex < curFrame.frameMap.workloads.size()
            ? &curFrame.frameMap.workloads[indices.workloadIndex] : nullptr;
        const GameFrameMap::ViewProjectionMap *projectionMap = nullptr;
        const DrawData *previousData = nullptr;
        if (workloadMap && workloadMap->mapped &&
            workloadMap->prevWorkloadIndex < queue.workloads.size() &&
            index < workloadMap->viewProjections.size()) {
            const auto &candidate = workloadMap->viewProjections[index];
            const auto &prevData = queue.workloads[workloadMap->prevWorkloadIndex].drawData;
            if (candidate.mapped && candidate.prevTransformIndex < prevData.viewProjTransforms.size()) {
                projectionMap = &candidate;
                previousData = &prevData;
                ++summary.mappedProjections;
                summary.projectionSourceDeltaMax = std::max(summary.projectionSourceDeltaMax,
                    maxAbsDifference(raw, prevData.viewProjTransforms[candidate.prevTransformIndex], 1));
            }
        }
        if (processed && std::fabs(curWeight - 1.0f) < 0.00001f) {
            ++summary.projectionEndpointChecks;
            // Horizontal aspect adjustment changes column zero only.
            summary.projectionEndpointMax = std::max(summary.projectionEndpointMax,
                maxAbsDifference(final, raw, 1));
        }
        if (processed && previousData && projectionMap &&
            std::fabs(curWeight) < 0.00001f &&
            (projectionMap->rigidBody.lerpTranslation || projectionMap->rigidBody.lerpRotation)) {
            summary.projectionPriorEndpointMax = std::max(summary.projectionPriorEndpointMax,
                maxAbsDifference(final,
                    previousData->viewProjTransforms[projectionMap->prevTransformIndex], 1));
        }
        if (!summary.firstProjection.present) {
            auto &sample = summary.firstProjection;
            sample.present = true;
            sample.workload = indices.workloadIndex;
            sample.index = index;
            if (index < data.viewProjTransformGroups.size()) {
                const uint32_t groupIndex = data.viewProjTransformGroups[index];
                if (groupIndex < data.transformGroups.size())
                    sample.group = data.transformGroups[groupIndex].matrixId;
            }
            if (projectionMap) sample.previous = projectionMap->prevTransformIndex;
            sample.rawX = raw[3][0]; sample.rawY = raw[3][1]; sample.rawZ = raw[3][2];
            sample.finalX = final[3][0]; sample.finalY = final[3][1]; sample.finalZ = final[3][2];
        }
    }
}

void traceWorldDetails(const WorkloadQueue &queue, const GameFrame &curFrame,
    uint32_t call, bool processed) noexcept {
    uint32_t emitted = 0;
    for (uint32_t w : curFrame.workloads) {
        if (w >= queue.workloads.size()) continue;
        const DrawData &data = queue.workloads[w].drawData;
        const GameFrameMap::WorkloadMap *workloadMap = w < curFrame.frameMap.workloads.size()
            ? &curFrame.frameMap.workloads[w] : nullptr;
        const DrawData *previousData = nullptr;
        if (workloadMap && workloadMap->mapped &&
            workloadMap->prevWorkloadIndex < queue.workloads.size())
            previousData = &queue.workloads[workloadMap->prevWorkloadIndex].drawData;
        for (uint32_t index = 0; index < data.worldTransforms.size(); ++index) {
            const auto &raw = data.worldTransforms[index];
            const auto &final = (processed && index < data.lerpWorldTransforms.size())
                ? data.lerpWorldTransforms[index] : raw;
            const GameFrameMap::TransformMap *map = nullptr;
            const interop::float4x4 *previous = nullptr;
            if (previousData && index < workloadMap->transforms.size()) {
                const auto &candidate = workloadMap->transforms[index];
                if (candidate.mapped && candidate.prevTransformIndex < previousData->worldTransforms.size()) {
                    map = &candidate;
                    previous = &previousData->worldTransforms[candidate.prevTransformIndex];
                }
            }
            const uint32_t groupIndex = index < data.worldTransformGroups.size()
                ? data.worldTransformGroups[index] : UINT32_MAX;
            const TransformGroup *group = groupIndex < data.transformGroups.size()
                ? &data.transformGroups[groupIndex] : nullptr;
            const uint32_t vertices = index < data.worldTransformVertexIndices.size()
                ? data.worldTransformVertexCount(index) : 0;
            // In large attract scenes, retain early scene context and every
            // unmatched drawable part, without emitting hundreds of bones.
            if (index >= 32 && (map || vertices == 0)) continue;
            if (emitted >= 64) return;
            ++emitted;
            uint32_t nearestIndex = UINT32_MAX;
            float nearestMatrixDelta = INFINITY;
            for (uint32_t other = 0; other < data.worldTransforms.size(); ++other) {
                if (other == index) continue;
                const float delta = maxAbsDifference(raw, data.worldTransforms[other]);
                if (delta < nearestMatrixDelta) {
                    nearestMatrixDelta = delta;
                    nearestIndex = other;
                }
            }
            uint32_t priorSameIndexVertices = 0;
            float priorSameIndexVertexDelta = -1.0f;
            if (previousData && index < previousData->worldTransformVertexIndices.size() &&
                index < data.worldTransformVertexIndices.size()) {
                priorSameIndexVertices = previousData->worldTransformVertexCount(index);
                const size_t curFirst = data.worldTransformVertexIndices[index];
                const size_t prevFirst = previousData->worldTransformVertexIndices[index];
                if (vertices == priorSameIndexVertices &&
                    curFirst <= data.posFloats.size() / 3 &&
                    vertices <= data.posFloats.size() / 3 - curFirst &&
                    prevFirst <= previousData->posFloats.size() / 3 &&
                    vertices <= previousData->posFloats.size() / 3 - prevFirst) {
                    priorSameIndexVertexDelta = 0.0f;
                    for (size_t v = 0; v < size_t(vertices) * 3; ++v)
                        priorSameIndexVertexDelta = std::max(priorSameIndexVertexDelta,
                            std::fabs(data.posFloats[curFirst * 3 + v] -
                                previousData->posFloats[prevFirst * 3 + v]));
                }
            }
            std::fprintf(stderr,
                "TOOIE_MATRIX_DETAIL call=%u w=%u i=%u mapped=%u prev_w=%u prev_i=%u "
                "gid=%08x addr=%08x phys=%08x verts=%u near_i=%u near_matrix=%.6g "
                "prev_same_verts=%u prev_same_posmax=%.6g decomp=%u flags=%u%u%u%u%u "
                "raw=%.5g,%.5g,%.5g prev=%.5g,%.5g,%.5g final=%.5g,%.5g,%.5g "
                "src_delta=%.6g final_delta=%.6g prior_err=%.6g\n",
                call, w, index, unsigned(map != nullptr),
                previous ? workloadMap->prevWorkloadIndex : UINT32_MAX,
                map ? map->prevTransformIndex : UINT32_MAX,
                group ? group->matrixId : 0,
                index < data.worldTransformSegmentedAddresses.size()
                    ? data.worldTransformSegmentedAddresses[index] : 0,
                index < data.worldTransformPhysicalAddresses.size()
                    ? data.worldTransformPhysicalAddresses[index] : 0,
                vertices, nearestIndex, nearestMatrixDelta,
                priorSameIndexVertices, priorSameIndexVertexDelta,
                map ? unsigned(map->rigidBody.lerpDecompose) : 0,
                map ? unsigned(map->rigidBody.lerpTranslation) : 0,
                map ? unsigned(map->rigidBody.lerpRotation) : 0,
                map ? unsigned(map->rigidBody.lerpScale) : 0,
                map ? unsigned(map->rigidBody.lerpSkew) : 0,
                map ? unsigned(map->rigidBody.lerpPerspective) : 0,
                raw[3][0], raw[3][1], raw[3][2],
                previous ? (*previous)[3][0] : 0.0f,
                previous ? (*previous)[3][1] : 0.0f,
                previous ? (*previous)[3][2] : 0.0f,
                final[3][0], final[3][1], final[3][2],
                previous ? maxAbsDifference(raw, *previous) : 0.0f,
                maxAbsDifference(final, raw),
                previous ? maxAbsDifference(map->rigidBody.lerp(0.0f, *previous, raw, true), *previous) : 0.0f);
        }
    }
}

} // namespace

void tooieScreenXTraceSetLogDirectory(const std::filesystem::path &directory) noexcept {
    try { screenXTraceDirectory = directory; }
    catch (...) { screenXTraceDirectory.clear(); }
}

void tooieScreenXTraceArm() noexcept {
    screenXTraceArm.fetch_add(1, std::memory_order_release);
}

void tooieMatrixTraceFrame(const WorkloadQueue &queue, const GameFrame &curFrame,
    const GameFrame &prevFrame, float curFrameWeight, float prevFrameWeight,
    bool projectionsProcessed, bool transformsProcessed) noexcept {
    static uint32_t screenArmSeen = 0;
    static uint32_t screenCalls = 420;
    const uint32_t screenArm = screenXTraceArm.load(std::memory_order_acquire);
    if (screenArm != screenArmSeen) {
        screenArmSeen = screenArm;
        screenCalls = 0;
        screenXTraceFile.close();
        screenXTraceBytes = 0;
        if (screenArm <= 4 && !screenXTraceDirectory.empty()) {
            try {
                screenXTraceFile.open(screenXTraceDirectory /
                    ("screen-xy-trace-" + std::to_string(screenArm) + ".csv"),
                    std::ios::out | std::ios::trunc);
                if (screenXTraceFile) {
                    constexpr char header[] = "sample,weight,workload_id,previous_workload_id,slot,"
                        "world,projection,world_group,projection_group,physical,vertices,sampled,"
                        "mixed_projection,world_mapped,previous_world,projection_mapped,previous_projection,"
                        "raw_x,raw_y,final_x,final_y,model_x,model_y,camera_x,camera_y,"
                        "world_tx,world_ty,world_tz\n";
                    screenXTraceFile.write(header, sizeof(header) - 1);
                    screenXTraceBytes = sizeof(header) - 1;
                }
            } catch (...) { screenXTraceFile.close(); }
        }
    }
    if (screenCalls < 420)
        traceScreenX(queue, curFrame, screenCalls++, curFrameWeight,
            projectionsProcessed, transformsProcessed);
    if (screenCalls == 420 && screenXTraceFile)
        screenXTraceFile.close();
    static const bool logoTrace = [] {
        const char* value = std::getenv("TOOIE_LOGO_TRACE");
        return value && std::string_view(value) == "1";
    }();
    static uint32_t logoCalls = 0;
    if (logoTrace && logoCalls++ < 3600) {
        // Dense observations of the small startup workloads only. This follows
        // local position -> model -> projection -> native viewport Y and
        // separates camera interpolation from model interpolation. It does not
        // assume a particular model address identifies a logo or change draws.
        for (uint32_t w : curFrame.workloads) {
            if (w >= queue.workloads.size()) continue;
            const Workload& workload = queue.workloads[w];
            const DrawData& data = workload.drawData;
            if (data.worldTransforms.size() > 8 || data.vertexCount() > 4096) continue;
            struct Bounds {
                unsigned count = 0;
                double sum = 0;
                float low = INFINITY, high = -INFINITY;
                void add(float y) {
                    ++count; sum += y; low = std::min(low, y); high = std::max(high, y);
                }
            };
            std::array<std::array<Bounds, 4>, 8> bounds{};
            std::array<uint32_t, 8> projectionIds{};
            for (uint32_t v = 0; v < data.vertexCount(); ++v) {
                if (v >= data.viewProjIndices.size() || size_t(v) * 3 + 2 >= data.posFloats.size()) continue;
                const uint32_t m = data.worldIndices[v], p = data.viewProjIndices[v];
                if (m >= data.worldTransforms.size() || p >= data.viewProjTransforms.size() || p >= data.rspViewports.size()) continue;
                projectionIds[m] = p;
                const auto& rawWorld = data.worldTransforms[m];
                const auto& finalWorld = transformsProcessed && m < data.lerpWorldTransforms.size()
                    ? data.lerpWorldTransforms[m] : rawWorld;
                const auto& rawProjection = data.viewProjTransforms[p];
                const auto& finalProjection = projectionsProcessed && p < data.modViewProjTransforms.size()
                    ? data.modViewProjTransforms[p] : rawProjection;
                const auto& viewport = data.rspViewports[p];
                const size_t offset = size_t(v) * 3;
                const hlslpp::float4 rawLocal(data.posFloats[offset], data.posFloats[offset + 1], data.posFloats[offset + 2], 1.0f);
                auto finalLocal = rawLocal;
                if (offset + 2 < data.velFloats.size()) {
                    finalLocal -= hlslpp::float4(data.velFloats[offset], data.velFloats[offset + 1], data.velFloats[offset + 2], 0.0f) * (1.0f - curFrameWeight);
                }
                const hlslpp::float4 points[4] = {
                    hlslpp::mul(hlslpp::mul(rawLocal, rawWorld), rawProjection),
                    hlslpp::mul(hlslpp::mul(finalLocal, finalWorld), finalProjection),
                    hlslpp::mul(hlslpp::mul(finalLocal, finalWorld), rawProjection),
                    hlslpp::mul(hlslpp::mul(rawLocal, rawWorld), finalProjection)
                };
                for (unsigned kind = 0; kind < 4; ++kind) {
                    if (std::fabs(float(points[kind].w)) < 0.0001f) continue;
                    const float y = float(viewport.translate.y) - float(viewport.scale.y) * float(points[kind].y) / float(points[kind].w);
                    if (std::isfinite(y)) bounds[m][kind].add(y);
                }
            }
            for (uint32_t m = 0; m < data.worldTransforms.size(); ++m) {
                if (!bounds[m][0].count || !bounds[m][1].count || !bounds[m][2].count || !bounds[m][3].count) continue;
                const auto& raw = bounds[m][0]; const auto& final = bounds[m][1];
                const auto& model = bounds[m][2]; const auto& camera = bounds[m][3];
                const auto& viewport = data.rspViewports[projectionIds[m]];
                std::fprintf(stderr,
                    "TOOIE_LOGO_TRACE call=%u workload=%llu fb=%08x w=%u m=%u p=%u cw=%.6f gid=%08x verts=%u "
                    "raw_y=%.6f final_y=%.6f model_y=%.6f camera_y=%.6f raw_bounds=%.6f,%.6f final_bounds=%.6f,%.6f viewport_y=%.6f,%.6f\n",
                    logoCalls - 1, static_cast<unsigned long long>(workload.workloadId),
                    workload.fbPairCount ? workload.fbPairs[workload.fbPairCount - 1].colorImage.address : 0,
                    w, m, projectionIds[m],
                    curFrameWeight, worldGroupId(data, m), raw.count,
                    raw.sum / raw.count, final.sum / final.count, model.sum / model.count, camera.sum / camera.count,
                    raw.low, raw.high, final.low, final.high, float(viewport.scale.y), float(viewport.translate.y));
            }
        }
    }
    static const bool enabled = [] {
        const char *value = std::getenv("TOOIE_MATRIX_TRACE");
        return value && std::string_view(value) == "1";
    }();
    if (!enabled) return;
    static const uint32_t start = [] {
        const char *value = std::getenv("TOOIE_MATRIX_TRACE_START");
        if (!value || !*value) return uint32_t{0};
        uint32_t parsed = 0;
        for (const char *digit = value; *digit; ++digit) {
            if (*digit < '0' || *digit > '9' || parsed > (1000000u - uint32_t(*digit - '0')) / 10u)
                return uint32_t{0};
            parsed = parsed * 10u + uint32_t(*digit - '0');
        }
        return parsed;
    }();
    static std::atomic<uint32_t> renderCalls{0};
    const uint32_t call = renderCalls.fetch_add(1, std::memory_order_relaxed);
    if (call < start) return;
    const uint32_t sampleCall = call - start;
    // Eleven is coprime to common 6/9 presentation-to-guest ratios, so the
    // bounded samples traverse interpolation phases instead of aliasing one.
    if (sampleCall >= 3300 || sampleCall % 11 != 0) return;

    TraceSummary summary{};
    for (const GameScene &scene : curFrame.perspectiveScenes)
        traceProjection(queue, curFrame, scene, curFrameWeight, projectionsProcessed, summary);
    for (const GameScene &scene : curFrame.orthographicScenes)
        traceProjection(queue, curFrame, scene, curFrameWeight, projectionsProcessed, summary);

    for (uint32_t w : curFrame.workloads) {
        if (w >= queue.workloads.size()) continue;
        const DrawData &data = queue.workloads[w].drawData;
        const GameFrameMap::WorkloadMap *workloadMap = w < curFrame.frameMap.workloads.size()
            ? &curFrame.frameMap.workloads[w] : nullptr;
        const DrawData *previousData = nullptr;
        if (workloadMap && workloadMap->mapped &&
            workloadMap->prevWorkloadIndex < queue.workloads.size())
            previousData = &queue.workloads[workloadMap->prevWorkloadIndex].drawData;
        for (uint32_t index = 0; index < data.worldTransforms.size(); ++index) {
            ++summary.worlds;
            const auto &raw = data.worldTransforms[index];
            const bool finalAvailable = transformsProcessed && index < data.lerpWorldTransforms.size();
            if (transformsProcessed && !finalAvailable) ++summary.missingWorldFinals;
            const auto &final = finalAvailable ? data.lerpWorldTransforms[index] : raw;
            summary.worldFinalDeltaMax = std::max(summary.worldFinalDeltaMax,
                maxAbsDifference(final, raw));
            if (!previousData || index >= workloadMap->transforms.size()) continue;
            const auto &map = workloadMap->transforms[index];
            if (!map.mapped || map.prevTransformIndex >= previousData->worldTransforms.size()) continue;
            ++summary.mappedWorlds;
            const auto &previous = previousData->worldTransforms[map.prevTransformIndex];
            summary.worldSourceDeltaMax = std::max(summary.worldSourceDeltaMax,
                maxAbsDifference(raw, previous));
            const auto endpoint = map.rigidBody.lerp(1.0f, previous, raw, true);
            const auto priorEndpoint = map.rigidBody.lerp(0.0f, previous, raw, true);
            const float endpointError = maxAbsDifference(endpoint, raw);
            summary.worldPriorEndpointMax = std::max(summary.worldPriorEndpointMax,
                maxAbsDifference(priorEndpoint, previous));
            if (endpointError >= summary.worldCurrentEndpointMax) {
                summary.worldCurrentEndpointMax = endpointError;
                auto &sample = summary.worstWorldEndpoint;
                sample.workload = w; sample.index = index;
                sample.previous = map.prevTransformIndex;
                if (index < data.worldTransformGroups.size()) {
                    const uint32_t groupIndex = data.worldTransformGroups[index];
                    if (groupIndex < data.transformGroups.size())
                        sample.group = data.transformGroups[groupIndex].matrixId;
                }
                sample.rawX = raw[3][0]; sample.rawY = raw[3][1]; sample.rawZ = raw[3][2];
                sample.finalX = endpoint[3][0]; sample.finalY = endpoint[3][1]; sample.finalZ = endpoint[3][2];
            }
        }
    }

    // One bounded line of guest-derived matrix summaries per sampled presentation.
    // Redirect stderr in a disposable startup run; no images or frame dumps are emitted.
    std::fprintf(stderr,
        "TOOIE_MATRIX_TRACE call=%u cw=%.5f pw=%.5f prev_frame_match=%u proj_process=%u world_process=%u "
        "p=%u pm=%u pe=%u pe_max=%.6g pprev_err=%.6g psplit=%.6g psrc_delta=%.6g pfinal_delta=%.6g "
        "p0=%u:%u gid=%08x prev=%u raw_t=%.4g,%.4g,%.4g final_t=%.4g,%.4g,%.4g "
        "w=%u wm=%u wmissing=%u wend_max=%.6g wprev_err=%.6g wsrc_delta=%.6g wfinal_delta=%.6g "
        "wend=%u:%u gid=%08x prev=%u raw_t=%.4g,%.4g,%.4g end_t=%.4g,%.4g,%.4g\n",
        call, curFrameWeight, prevFrameWeight, unsigned(prevFrame.matched),
        unsigned(projectionsProcessed), unsigned(transformsProcessed),
        summary.projections, summary.mappedProjections, summary.projectionEndpointChecks,
        summary.projectionEndpointMax, summary.projectionPriorEndpointMax,
        summary.projectionSplitGapMax, summary.projectionSourceDeltaMax,
        summary.projectionFinalDeltaMax,
        summary.firstProjection.workload, summary.firstProjection.index,
        summary.firstProjection.group, summary.firstProjection.previous,
        summary.firstProjection.rawX, summary.firstProjection.rawY,
        summary.firstProjection.rawZ, summary.firstProjection.finalX,
        summary.firstProjection.finalY, summary.firstProjection.finalZ,
        summary.worlds, summary.mappedWorlds, summary.missingWorldFinals,
        summary.worldCurrentEndpointMax, summary.worldPriorEndpointMax,
        summary.worldSourceDeltaMax, summary.worldFinalDeltaMax,
        summary.worstWorldEndpoint.workload, summary.worstWorldEndpoint.index,
        summary.worstWorldEndpoint.group, summary.worstWorldEndpoint.previous,
        summary.worstWorldEndpoint.rawX, summary.worstWorldEndpoint.rawY,
        summary.worstWorldEndpoint.rawZ, summary.worstWorldEndpoint.finalX,
        summary.worstWorldEndpoint.finalY, summary.worstWorldEndpoint.finalZ);
    TriangleSummary triangles{};
    traceTriangleEdges(queue, curFrame, sampleCall / 11, curFrameWeight,
        transformsProcessed, triangles);
    std::fprintf(stderr,
        "TOOIE_TRI_TRACE call=%u checked=%u skipped=%u max_ratio=%.6g max_growth_any=%.6g "
        "w=%u tri=%u v=%u,%u m=%u,%u gid=%08x,%08x raw_len=%.6g final_len=%.6g "
        "ratio_edge_growth=%.6g prev_same_local_len=%.6g prev_valid=%u prev_local_reliable=%u vel_edge=%u\n",
        call, triangles.checked, triangles.skipped, triangles.maxRatio,
        triangles.maxGrowthAny, triangles.workload, triangles.triangle,
        triangles.firstVertex, triangles.secondVertex,
        triangles.firstMatrix, triangles.secondMatrix,
        triangles.firstGroup, triangles.secondGroup,
        triangles.rawLength, triangles.finalLength,
        triangles.finalLength - triangles.rawLength,
        triangles.previousSameLocalLength,
        unsigned(triangles.previousValid),
        unsigned(triangles.previousLocalReliable),
        unsigned(triangles.edgeVelocityUsed));
    // The first startup logos draw only 3-5 world matrices. Detail at a
    // coarser stride gives all parts and their map provenance within 155 rows.
    if (sampleCall <= 990 && sampleCall % 33 == 0)
        traceWorldDetails(queue, curFrame, call, transformsProcessed);
}

} // namespace RT64
