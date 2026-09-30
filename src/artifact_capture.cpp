#include "artifact_capture.hpp"
#include "rt64_matrix_trace.hpp"
#include "model_interpolation.hpp"

#include "hle/rt64_state.h"
#include "hle/rt64_workload_queue.h"
#include "xxHash/xxh3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace tooie::artifact_capture {
namespace {

constexpr uint32_t kTasksPerRequest = 2;
constexpr size_t kMaxManifestCalls = 768;
constexpr size_t kMaxDetailedCalls = 48;
constexpr size_t kMaxBytes = 128 * 1024;

struct CallRef {
    size_t framebuffer;
    size_t projection;
    size_t call;
    size_t nonpositive_w;
    size_t positive_w;
};

// The source clip buffer is finalized at full sync. Count signs for every
// indexed call so a later mixed-W draw can be selected for detailed capture.
std::array<size_t, 2> clip_w_counts(const RT64::DrawData& data,
    const RT64::GameCall& game_call) {
    const size_t first = game_call.meshDesc.faceIndicesStart;
    if (first >= data.faceIndices.size()) return {};
    const size_t requested = size_t(game_call.callDesc.triangleCount) * 3;
    const size_t count = std::min({requested, data.faceIndices.size() - first, size_t(4096)});
    std::array<size_t, 2> result{};
    for (size_t i = 0; i < count; ++i) {
        const size_t vertex = data.faceIndices[first + i];
        if (vertex >= data.posTransformed.size()) continue;
        const float w = float(data.posTransformed[vertex][3]);
        if (std::isfinite(w)) ++result[w > 0.0f];
    }
    return result;
}

// Source geometry only: do not read render-thread lerp/previous/GPU buffers.
// A bounded sample plus screen-space bounds distinguishes a bad source vertex
// from a material or later interpolation problem without dumping the ROM.
nlohmann::json source_geometry(const RT64::DrawData& data, const RT64::GameCall& game_call) {
    const auto& call = game_call.callDesc;
    const size_t first = game_call.meshDesc.faceIndicesStart;
    const size_t requested = size_t(call.triangleCount) * 3;
    if (first > data.faceIndices.size()) return {{"available", false}};
    const size_t count = std::min({requested, data.faceIndices.size() - first, size_t(4096)});
    std::array<float, 3> low, high;
    low.fill(std::numeric_limits<float>::max());
    high.fill(std::numeric_limits<float>::lowest());
    size_t valid = 0, invalid = 0, behind = 0;
    nlohmann::json samples = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        const size_t vertex = data.faceIndices[first + i];
        if (vertex >= data.posScreen.size() || vertex >= data.posTransformed.size() ||
            vertex >= data.worldIndices.size() || vertex >= data.posFloats.size() / 3) {
            ++invalid;
            continue;
        }
        const auto& screen = data.posScreen[vertex];
        const auto& clip = data.posTransformed[vertex];
        if (!std::isfinite(float(screen[0])) || !std::isfinite(float(screen[1])) ||
            !std::isfinite(float(screen[2])) || !std::isfinite(float(clip[3]))) {
            ++invalid;
            continue;
        }
        ++valid;
        behind += float(clip[3]) <= 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], float(screen[axis]));
            high[axis] = std::max(high[axis], float(screen[axis]));
        }
        if (samples.size() < 3) {
            samples.push_back({
                {"vertex", vertex}, {"world_index", data.worldIndices[vertex]},
                {"local_xyz", {data.posFloats[vertex * 3], data.posFloats[vertex * 3 + 1], data.posFloats[vertex * 3 + 2]}},
                {"screen_xyz", {float(screen[0]), float(screen[1]), float(screen[2])}},
                {"clip_xyzw", {float(clip[0]), float(clip[1]), float(clip[2]), float(clip[3])}},
            });
            auto& sample = samples.back();
            if (vertex < data.tcFloats.size() / 2)
                sample["source_uv"] = {data.tcFloats[vertex * 2], data.tcFloats[vertex * 2 + 1]};
            if (vertex < data.normColBytes.size() / 4)
                sample["source_normal_or_color"] = {data.normColBytes[vertex * 4],
                    data.normColBytes[vertex * 4 + 1], data.normColBytes[vertex * 4 + 2],
                    data.normColBytes[vertex * 4 + 3]};
        }
    }
    nlohmann::json result{{"available", true}, {"face_indices_start", first},
        {"references_checked", count}, {"references_requested", requested},
        {"truncated", count != requested}, {"valid", valid}, {"invalid", invalid},
        {"nonpositive_w", behind}, {"samples", std::move(samples)}};
    if (valid) { result["screen_min"] = low; result["screen_max"] = high; }
    return result;
}

std::atomic_uint32_t pending_tasks{0};

const char* projection_type(RT64::Projection::Type type) {
    switch (type) {
    case RT64::Projection::Type::Perspective: return "perspective";
    case RT64::Projection::Type::Orthographic: return "orthographic";
    case RT64::Projection::Type::Rectangle: return "rectangle";
    case RT64::Projection::Type::Triangle: return "triangle";
    case RT64::Projection::Type::None: return "none";
    }
    return "unknown";
}

bool consume_request() noexcept {
    uint32_t remaining = pending_tasks.load(std::memory_order_acquire);
    while (remaining != 0) {
        if (pending_tasks.compare_exchange_weak(remaining, remaining - 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

nlohmann::json profiler_values(const RT64::ProfilingTimer& profiler) {
    if (profiler.size() == 0) return {{"last_ms", 0.0}, {"recent_average_ms", 0.0}, {"samples", 0}};
    const auto* values = profiler.data();
    const double last = values[(profiler.index() + profiler.size() - 1) % profiler.size()];
    double total = 0.0;
    std::size_t samples = 0;
    for (std::size_t i = 0; i < profiler.size(); ++i) {
        if (values[i] > 0.0) {
            total += values[i];
            ++samples;
        }
    }
    return {{"last_ms", last}, {"recent_average_ms", samples != 0 ? total / samples : 0.0},
        {"samples", samples}};
}

bool call_match_hash(const RT64::Workload& workload, const RT64::DrawCall& call,
    uint64_t& hash, uint32_t& matching_flags) noexcept {
    // Keep this byte layout and hash construction in lockstep with the pinned
    // RT64 GameFrame::hashFromCall/buildCallHashMap implementation.
    struct CallMatchKey {
        interop::ColorCombiner colorCombiner;
        interop::OtherMode otherMode;
        uint32_t geometryMode;
        uint32_t triangleCount;
        uint32_t matrixIdHash;
    };
    static_assert(sizeof(CallMatchKey) == 28);

    if (call.minWorldMatrix > call.maxWorldMatrix ||
        call.maxWorldMatrix >= workload.drawData.worldTransformGroups.size()) {
        return false;
    }

    uint32_t matrix_id_hash = 0;
    bool transform_matching = false;
    bool tile_interpolation = false;
    bool tile_matching = false;
    for (uint32_t m = call.minWorldMatrix; m <= call.maxWorldMatrix; ++m) {
        const uint32_t group_index = workload.drawData.worldTransformGroups[m];
        if (group_index >= workload.drawData.transformGroups.size()) {
            return false;
        }
        const RT64::TransformGroup& group = workload.drawData.transformGroups[group_index];
        matrix_id_hash = matrix_id_hash * 33 ^ group.matrixId;
        const bool id_with_auto_ordering = group.matrixId != G_EX_ID_AUTO &&
            group.matrixId != G_EX_ID_IGNORE && group.ordering == G_EX_ORDER_AUTO;
        transform_matching = transform_matching || group.matrixId == G_EX_ID_AUTO || id_with_auto_ordering;
        tile_interpolation = tile_interpolation || group.tileInterpolation != G_EX_COMPONENT_SKIP;
        tile_matching = tile_matching || group.tileInterpolation == G_EX_COMPONENT_AUTO;
    }

    CallMatchKey key;
    key.colorCombiner = call.colorCombiner;
    key.otherMode = call.otherMode;
    key.geometryMode = call.geometryMode;
    key.triangleCount = call.triangleCount;
    key.matrixIdHash = matrix_id_hash;
    hash = XXH3_64bits(&key, sizeof(key));
    matching_flags = uint32_t(transform_matching) | (uint32_t(tile_interpolation) << 1) |
        (uint32_t(tile_matching) << 2);
    return true;
}

} // namespace

void request() noexcept {
    pending_tasks.store(kTasksPerRequest, std::memory_order_release);
    RT64::tooieScreenXTraceArm();
    tooie::model_interpolation::arm_trace();
}

bool try_claim() noexcept {
    return consume_request();
}

void capture_after_display_list(RT64::State& state, uint32_t cursor_before,
    uint32_t cursor_after, const TaskContext& context, const LogSink& log) {
    try {

    // RT64 advances the queue from State::fullSync. `fbPairs`, projections,
    // GameCall::callDesc, and DrawData::callTiles are finalized by that point.
    // The render thread only reads these capture fields (it writes derived
    // lerp/GPU buffers), so an immediate producer-side copy avoids waiting for
    // the intentionally early first-frame workload signal.
    const bool submitted = cursor_before != cursor_after;
    nlohmann::json packet{
        {"graphics_task_sequence", context.sequence},
        {"display_list", context.display_list},
        {"display_list_size", context.display_list_size},
        {"display_list_hash", context.display_list_hash},
        {"display_list_hash_algorithm", context.display_list_hash_algorithm},
        {"display_list_hash_scope", context.display_list_hash_scope},
        {"map", context.map_id},
        {"gbi", context.gbi},
        {"submitted_by_full_sync", submitted},
        // Equal cursors normally mean this task did not submit a workload.
        // A full ring traversal is indistinguishable at this seam, so retain
        // that ambiguity but never dereference an ambiguous queue slot.
        {"same_cursor_unsubmitted_or_ring_wrapped", !submitted},
        {"projection_override", context.projection_override},
        {"camera_interpolated", context.camera_interpolated},
        {"original_cutscene_motion", context.original_cutscene_motion},
        {"free_camera", {
            {"enabled", context.free_camera_enabled},
            {"input_active", context.free_camera_input_active},
            {"offset", context.free_camera_offset},
            {"snapshot_consistency", "best_effort_atomic_fields_may_span_reset"},
        }},
        {"calls", nlohmann::json::array()},
        {"call_manifest", nlohmann::json::array()},
    };
    if (!submitted) {
        packet["workload_index"] = nullptr;
        packet["workload_id"] = nullptr;
        packet["framebuffer_pairs"] = 0;
        packet["recorded_calls"] = 0;
        packet["encountered_calls"] = 0;
        packet["truncated"] = false;
        packet["byte_cap"] = kMaxBytes;
        packet["capture_scope"] = "metadata_only_no_workload_slot_dereferenced";
        log("artifact_draw_capture", std::move(packet));
        return;
    }
    if (!state.ext.workloadQueue || state.ext.workloadQueue->workloads.empty()) {
        packet["capture_scope"] = "metadata_only_missing_workload_queue";
        packet["workload_index"] = nullptr;
        packet["workload_id"] = nullptr;
        log("artifact_draw_capture", std::move(packet));
        return;
    }
    // `cursor_before` is the producer slot that processDisplayLists started
    // filling. It is the first completed workload if an unusual list submits
    // more than once; preserve that fact instead of silently selecting latest.
    const uint32_t workload_index = cursor_before;
    const size_t workload_count = state.ext.workloadQueue->workloads.size();
    if (workload_index >= workload_count) {
        packet["capture_scope"] = "metadata_only_invalid_workload_cursor";
        packet["workload_index"] = workload_index;
        packet["workload_id"] = nullptr;
        log("artifact_draw_capture", std::move(packet));
        return;
    }
    const uint32_t expected_after = uint32_t((cursor_before + 1) % workload_count);
    RT64::Workload& workload = state.ext.workloadQueue->workloads[workload_index];
    packet["workload_index"] = workload_index;
    packet["workload_id"] = workload.workloadId;
    packet["vi_original_rate"] = workload.viOriginalRate;
    packet["multiple_full_sync_suspected"] = cursor_after != expected_after;
    packet["framebuffer_pairs"] = workload.fbPairCount;
    packet["capture_scope"] = "first_submitted_workload_descriptors";
    packet["framebuffer_descriptors"] = nlohmann::json::array();

    size_t recorded = 0;
    size_t encountered = 0;
    std::vector<CallRef> call_refs;
    call_refs.reserve(kMaxManifestCalls);
    std::vector<std::pair<size_t, size_t>> projection_ranges;
    uint64_t encountered_triangles = 0;
    uint64_t encountered_textured_calls = 0;
    uint64_t encountered_tile_references = 0;
    uint64_t perspective_projections = 0;
    uint64_t orthographic_projections = 0;
    uint64_t rectangle_projections = 0;
    uint64_t triangle_projections = 0;
    uint64_t none_projections = 0;
    uint64_t perspective_calls = 0;
    uint64_t orthographic_calls = 0;
    uint64_t rectangle_calls = 0;
    uint64_t triangle_calls = 0;
    uint64_t none_calls = 0;
    uint64_t matching_hashable_calls = 0;
    uint64_t matching_unhashable_calls = 0;
    uint64_t matching_bucket_count = 0;
    uint64_t matching_largest_bucket = 0;
    uint64_t matching_same_hash_pairs = 0;
    uint64_t matching_identical_previous_candidate_pairs = 0;
    uint64_t matching_transform_candidate_pairs = 0;
    uint64_t matching_transform_dedup_candidate_pairs = 0;
    uint64_t matching_tile_candidate_pairs = 0;
    uint64_t matching_tile_dedup_candidate_pairs = 0;
    uint64_t matching_look_at_candidate_pairs = 0;
    uint64_t matching_look_at_dedup_candidate_pairs = 0;
    bool truncated = false;
    const size_t fb_count = std::min<size_t>(workload.fbPairCount, workload.fbPairs.size());
    packet["projection_descriptors"] = nlohmann::json::array();
    for (size_t f = 0; f < fb_count; ++f) {
        const RT64::FramebufferPair& framebuffer = workload.fbPairs[f];
        packet["framebuffer_descriptors"].push_back({
            {"index", f}, {"color_address", framebuffer.colorImage.address},
            {"color_format", framebuffer.colorImage.fmt},
            {"color_size", framebuffer.colorImage.siz},
            {"color_width", framebuffer.colorImage.width},
            {"depth_address", framebuffer.depthImage.address},
            {"depth_read", framebuffer.depthRead}, {"depth_write", framebuffer.depthWrite},
            {"fill_rect_only", framebuffer.fillRectOnly},
            {"clear_depth_only", bool(framebuffer.fastPaths.clearDepthOnly)},
            {"scissor_fixed_quarters", {framebuffer.scissorRect.ulx, framebuffer.scissorRect.uly,
                framebuffer.scissorRect.lrx, framebuffer.scissorRect.lry}},
            {"draw_color_fixed_quarters", {framebuffer.drawColorRect.ulx, framebuffer.drawColorRect.uly,
                framebuffer.drawColorRect.lrx, framebuffer.drawColorRect.lry}},
            {"draw_depth_fixed_quarters", {framebuffer.drawDepthRect.ulx, framebuffer.drawDepthRect.uly,
                framebuffer.drawDepthRect.lrx, framebuffer.drawDepthRect.lry}},
        });
        const size_t projection_count = std::min<size_t>(framebuffer.projectionCount, framebuffer.projections.size());
        for (size_t p = 0; p < projection_count; ++p) {
            const RT64::Projection& projection = framebuffer.projections[p];
            const size_t call_count = std::min<size_t>(projection.gameCallCount, projection.gameCalls.size());
            nlohmann::json projection_descriptor{
                {"framebuffer_pair", f}, {"projection", p},
                {"type", projection_type(projection.type)},
                {"transforms_index", projection.transformsIndex}, {"calls", call_count},
                {"scissor_fixed_quarters", {projection.scissorRect.ulx, projection.scissorRect.uly,
                    projection.scissorRect.lrx, projection.scissorRect.lry}},
            };
            // Keep the original combined matrix alongside its decomposed parts.
            // Aspect-only rendering may recompose them even at the original rate.
            const auto transform_index = projection.transformsIndex;
            const auto& source_draw = workload.drawData;
            if (transform_index < source_draw.viewProjTransforms.size())
                projection_descriptor["source_view_projection"] = source_draw.viewProjTransforms[transform_index].m;
            if (transform_index < source_draw.viewTransforms.size())
                projection_descriptor["source_view"] = source_draw.viewTransforms[transform_index].m;
            if (transform_index < source_draw.projTransforms.size())
                projection_descriptor["source_projection"] = source_draw.projTransforms[transform_index].m;
            if (transform_index < source_draw.viewProjTransformGroups.size() &&
                source_draw.viewProjTransformGroups[transform_index] < source_draw.transformGroups.size()) {
                const auto& group = source_draw.transformGroups[source_draw.viewProjTransformGroups[transform_index]];
                projection_descriptor["aspect_mode"] = group.aspectMode;
            }
            if (projection.transformsIndex < workload.drawData.rspViewports.size()) {
                const auto& viewport = workload.drawData.rspViewports[projection.transformsIndex];
                projection_descriptor["viewport_scale"] = {viewport.scale[0], viewport.scale[1], viewport.scale[2]};
                projection_descriptor["viewport_translate"] = {viewport.translate[0], viewport.translate[1], viewport.translate[2]};
            }
            if (projection.transformsIndex < workload.drawData.viewportClipRatios.size() / 4) {
                const auto index = projection.transformsIndex * 4;
                projection_descriptor["viewport_clip_ratios"] = {
                    workload.drawData.viewportClipRatios[index], workload.drawData.viewportClipRatios[index + 1],
                    workload.drawData.viewportClipRatios[index + 2], workload.drawData.viewportClipRatios[index + 3]};
            }
            packet["projection_descriptors"].push_back(std::move(projection_descriptor));
            const size_t projection_first = call_refs.size();
            const bool matcher_projection = projection.type == RT64::Projection::Type::Perspective ||
                projection.type == RT64::Projection::Type::Orthographic;
            struct BucketEstimate {
                uint64_t occupancy = 0;
                uint64_t transform_eligible = 0;
                uint64_t tile_eligible = 0;
                uint64_t look_at_eligible = 0;
                std::set<std::array<uint32_t, 2>> transform_work;
                std::set<std::array<uint32_t, 3>> tile_work;
                std::set<uint32_t> look_at_indices;
            };
            std::unordered_map<uint64_t, BucketEstimate> matching_buckets;
            if (matcher_projection) {
                matching_buckets.reserve(call_count);
            }
            encountered += call_count;
            switch (projection.type) {
            case RT64::Projection::Type::Perspective:
                ++perspective_projections;
                perspective_calls += call_count;
                break;
            case RT64::Projection::Type::Orthographic:
                ++orthographic_projections;
                orthographic_calls += call_count;
                break;
            case RT64::Projection::Type::Rectangle:
                ++rectangle_projections;
                rectangle_calls += call_count;
                break;
            case RT64::Projection::Type::Triangle:
                ++triangle_projections;
                triangle_calls += call_count;
                break;
            case RT64::Projection::Type::None:
                ++none_projections;
                none_calls += call_count;
                break;
            }
            for (size_t c = 0; c < call_count; ++c) {
                const RT64::DrawCall& call = projection.gameCalls[c].callDesc;
                encountered_triangles += call.triangleCount;
                encountered_textured_calls += call.textureOn != 0;
                encountered_tile_references += call.tileCount;
                if (matcher_projection) {
                    uint64_t hash = 0;
                    uint32_t matching_flags = 0;
                    if (call_match_hash(workload, call, hash, matching_flags)) {
                        ++matching_hashable_calls;
                        BucketEstimate& bucket = matching_buckets[hash];
                        ++bucket.occupancy;
                        if ((matching_flags & 1) != 0) {
                            ++bucket.transform_eligible;
                            bucket.transform_work.emplace(std::array<uint32_t, 2>{
                                call.minWorldMatrix, call.maxWorldMatrix});
                        }
                        if ((matching_flags & 2) != 0) {
                            ++bucket.tile_eligible;
                            bucket.tile_work.emplace(std::array<uint32_t, 3>{
                                call.tileIndex, call.tileCount, (matching_flags >> 2) & 1});
                        }
                        const uint32_t texture_gen_mask = G_LIGHTING | G_TEXTURE_GEN;
                        if ((call.geometryMode & texture_gen_mask) == texture_gen_mask) {
                            const uint32_t face_start = projection.gameCalls[c].meshDesc.faceIndicesStart;
                            if (face_start < workload.drawData.faceIndices.size()) {
                                const uint32_t vertex_index = workload.drawData.faceIndices[face_start];
                                if (vertex_index < workload.drawData.lookAtIndices.size()) {
                                    ++bucket.look_at_eligible;
                                    bucket.look_at_indices.emplace(
                                        workload.drawData.lookAtIndices[vertex_index] >> RSP_LOOKAT_INDEX_SHIFT);
                                }
                            }
                        }
                    }
                    else {
                        ++matching_unhashable_calls;
                    }
                }
                if (call_refs.size() < kMaxManifestCalls) {
                    const auto w = matcher_projection ?
                        clip_w_counts(workload.drawData, projection.gameCalls[c]) : std::array<size_t, 2>{};
                    call_refs.push_back({f, p, c, w[0], w[1]});
                    // Column names are emitted once to keep later calls visible
                    // inside the same bounded JSONL event.
                    packet["call_manifest"].push_back({f, p, c, call.callIndex, call.uid,
                        call.triangleCount, call.geometryMode, call.cullBothMask,
                        call.otherMode.H, call.otherMode.L, call.colorCombiner.H,
                        call.colorCombiner.L, call.textureOn, call.textureTile,
                        call.tileIndex, call.tileCount, call.minWorldMatrix,
                        call.maxWorldMatrix, call.NoN, w[0], w[1]});
                }
                else {
                    truncated = true;
                }
            }
            if (call_refs.size() > projection_first)
                projection_ranges.emplace_back(projection_first, call_refs.size());
            for (const auto& [hash, bucket] : matching_buckets) {
                (void)hash;
                ++matching_bucket_count;
                const uint64_t occupancy = bucket.occupancy;
                matching_largest_bucket = std::max(matching_largest_bucket, occupancy);
                matching_same_hash_pairs += occupancy * (occupancy - 1) / 2;
                matching_identical_previous_candidate_pairs += occupancy * occupancy;
                matching_transform_candidate_pairs += bucket.transform_eligible * bucket.transform_eligible;
                matching_transform_dedup_candidate_pairs += bucket.transform_work.size() * bucket.transform_work.size();
                matching_tile_candidate_pairs += bucket.tile_eligible * bucket.tile_eligible;
                matching_tile_dedup_candidate_pairs += bucket.tile_work.size() * bucket.tile_work.size();
                matching_look_at_candidate_pairs += bucket.look_at_eligible * bucket.look_at_eligible;
                matching_look_at_dedup_candidate_pairs += bucket.look_at_indices.size() * bucket.look_at_indices.size();
            }
        }
    }
    packet["call_manifest_columns"] = {"framebuffer_pair", "projection", "call",
        "call_index", "uid", "triangles", "geometry_mode", "cull_both_mask",
        "other_mode_h", "other_mode_l", "combiner_h", "combiner_l",
        "texture_on", "texture_tile", "tile_index", "tile_count",
        "world_min", "world_max", "no_near_clip_ucode", "nonpositive_w", "positive_w"};
    packet["manifest_cap"] = kMaxManifestCalls;
    packet["manifest_calls"] = call_refs.size();
    packet["manifest_truncated"] = encountered > call_refs.size();
    packet["clip_w_count_scope"] = "up_to_4096_source_face_references_per_indexed_call";
    std::vector<size_t> selected;
    std::vector<const char*> selected_reasons;
    std::vector<bool> chosen(call_refs.size(), false);
    const auto select = [&](size_t index, const char* reason) {
        if (index < call_refs.size() && !chosen[index] && selected.size() < kMaxDetailedCalls) {
            chosen[index] = true;
            selected.push_back(index);
            selected_reasons.push_back(reason);
        }
    };
    // Give every projection an early draw, then a mixed-W draw and a late draw.
    // Fill remaining slots uniformly across the entire workload.
    for (const auto& range : projection_ranges) select(range.first, "projection_first");
    for (const auto& range : projection_ranges) {
        for (size_t i = range.first; i < range.second; ++i) {
            if (call_refs[i].nonpositive_w && call_refs[i].positive_w) {
                select(i, "projection_first_mixed_clip_w");
                break;
            }
        }
    }
    for (const auto& range : projection_ranges) select(range.second - 1, "projection_last");
    if (!call_refs.empty()) {
        for (size_t slot = 0; slot < kMaxDetailedCalls; ++slot)
            select((slot * (call_refs.size() - 1)) / (kMaxDetailedCalls - 1), "uniform_workload");
    }
    for (size_t detail_index = 0; detail_index < selected.size(); ++detail_index) {
                const size_t ordinal = selected[detail_index];
                const CallRef& ref = call_refs[ordinal];
                const RT64::Projection& projection = workload.fbPairs[ref.framebuffer].projections[ref.projection];
                const size_t f = ref.framebuffer, p = ref.projection, c = ref.call;
                const RT64::DrawCall& call = projection.gameCalls[c].callDesc;
                const bool matcher_projection = projection.type == RT64::Projection::Type::Perspective ||
                    projection.type == RT64::Projection::Type::Orthographic;
                nlohmann::json entry{
                    {"manifest_ordinal", ordinal},
                    {"selection_reason", selected_reasons[detail_index]},
                    {"framebuffer_pair", f}, {"projection", p},
                    {"projection_type", projection_type(projection.type)},
                    {"transforms_index", projection.transformsIndex},
                    {"call", c}, {"call_index", call.callIndex}, {"uid", call.uid},
                    {"triangles", call.triangleCount}, {"geometry_mode", call.geometryMode},
                    {"cull_both_mask", call.cullBothMask}, {"texture_on", call.textureOn},
                    {"texture_tile", call.textureTile}, {"tile_index", call.tileIndex},
                    {"tile_count", call.tileCount},
                    {"combiner", {{"L", call.colorCombiner.L}, {"H", call.colorCombiner.H}}},
                    {"other_mode", {{"L", call.otherMode.L}, {"H", call.otherMode.H}}},
                    {"draw_status_changes", call.drawStatusChanges},
                    {"no_near_clip_ucode", call.NoN},
                    {"world_min", call.minWorldMatrix}, {"world_max", call.maxWorldMatrix},
                    {"prim_color", {call.rdpParams.primColor[0], call.rdpParams.primColor[1], call.rdpParams.primColor[2], call.rdpParams.primColor[3]}},
                    {"env_color", {call.rdpParams.envColor[0], call.rdpParams.envColor[1], call.rdpParams.envColor[2], call.rdpParams.envColor[3]}},
                    {"fog_color", {call.rdpParams.fogColor[0], call.rdpParams.fogColor[1], call.rdpParams.fogColor[2], call.rdpParams.fogColor[3]}},
                    {"scissor_fixed_quarters", {call.scissorRect.ulx, call.scissorRect.uly,
                        call.scissorRect.lrx, call.scissorRect.lry}},
                    {"scissor_mode", call.scissorMode},
                };
                if (matcher_projection) {
                    entry["source_geometry"] = source_geometry(workload.drawData, projection.gameCalls[c]);
                    const auto& draw = workload.drawData;
                    if (projection.transformsIndex < draw.viewProjTransforms.size()) {
                        entry["view_projection"] = draw.viewProjTransforms[projection.transformsIndex].m;
                    }
                    if (call.minWorldMatrix < draw.worldTransforms.size()) {
                        const auto index = call.minWorldMatrix;
                        entry["first_world_matrix"] = draw.worldTransforms[index].m;
                        if (index < draw.worldTransformPhysicalAddresses.size())
                            entry["first_world_matrix_address"] = draw.worldTransformPhysicalAddresses[index];
                        if (index < draw.worldTransformGroups.size() &&
                            draw.worldTransformGroups[index] < draw.transformGroups.size()) {
                            const auto& group = draw.transformGroups[draw.worldTransformGroups[index]];
                            entry["first_world_group"] = {{"id", group.matrixId},
                                {"position", group.positionInterpolation}, {"rotation", group.rotationInterpolation},
                                {"scale", group.scaleInterpolation}, {"vertex", group.vertexInterpolation}};
                        }
                    }
                }
                nlohmann::json tiles = nlohmann::json::array();
                const size_t tile_end = std::min<size_t>(call.tileIndex + call.tileCount, workload.drawData.callTiles.size());
                for (size_t t = call.tileIndex; t < tile_end; ++t) {
                    const RT64::DrawCallTile& tile = workload.drawData.callTiles[t];
                    tiles.push_back({
                        {"index", t}, {"tmem_hash", tile.tmemHashOrID}, {"valid", tile.valid},
                        {"sample_width", tile.sampleWidth}, {"sample_height", tile.sampleHeight},
                        {"raw_tmem", tile.rawTMEM}, {"tlut", tile.tlut},
                    });
                }
                entry["tiles"] = std::move(tiles);
                packet["calls"].push_back(std::move(entry));
                ++recorded;
    }
    packet["recorded_calls"] = recorded;
    packet["encountered_calls"] = encountered;
    packet["encountered_triangles"] = encountered_triangles;
    packet["encountered_textured_calls"] = encountered_textured_calls;
    packet["encountered_tile_references"] = encountered_tile_references;
    packet["projection_counts"] = {
        {"perspective", perspective_projections}, {"orthographic", orthographic_projections},
        {"rectangle", rectangle_projections}, {"triangle", triangle_projections}, {"none", none_projections}};
    packet["calls_by_projection_type"] = {
        {"perspective", perspective_calls}, {"orthographic", orthographic_calls},
        {"rectangle", rectangle_calls}, {"triangle", triangle_calls}, {"none", none_calls}};
    packet["matching_hash_estimate"] = {
        {"hashable_calls", matching_hashable_calls},
        {"unhashable_calls", matching_unhashable_calls},
        {"bucket_count_sum_across_projections", matching_bucket_count},
        {"largest_bucket_within_one_projection", matching_largest_bucket},
        {"same_hash_unordered_pairs_within_projections", matching_same_hash_pairs},
        {"candidate_pairs_if_previous_frame_identical", matching_identical_previous_candidate_pairs},
        {"transform_candidate_pairs_if_previous_frame_identical", matching_transform_candidate_pairs},
        {"transform_pairs_after_domain_dedup_if_previous_frame_identical", matching_transform_dedup_candidate_pairs},
        {"tile_candidate_pairs_if_previous_frame_identical", matching_tile_candidate_pairs},
        {"tile_pairs_after_domain_dedup_if_previous_frame_identical", matching_tile_dedup_candidate_pairs},
        {"look_at_candidate_pairs_if_previous_frame_identical", matching_look_at_candidate_pairs},
        {"look_at_pairs_after_domain_dedup_if_previous_frame_identical", matching_look_at_dedup_candidate_pairs},
        {"scope", "exact_pinned_rt64_call_hash_per_perspective_or_orthographic_projection"},
        {"estimate_limit", "candidate count assumes each projection is matched to an identical previous projection; scene merging can add cross_projection_candidates"},
    };
    {
        // RT64 owns these histories on its workload thread. Never wait for that
        // thread from the diagnostic path; copy only when its existing mutex is
        // immediately available, otherwise retain an explicit unavailable row.
        std::unique_lock<std::mutex> profiler_lock(state.ext.workloadQueue->threadMutex, std::try_to_lock);
        if (profiler_lock.owns_lock()) {
            packet["rt64_render_profilers"] = {
                {"available", true},
                {"matching_cpu", profiler_values(state.ext.workloadQueue->matchingProfiler)},
                {"workload_wall", profiler_values(state.ext.workloadQueue->workloadProfiler)},
                {"render_frame_wall_including_gpu_wait", profiler_values(state.ext.workloadQueue->rendererCPUProfiler)},
                {"render_gpu_timestamp", profiler_values(state.ext.workloadQueue->rendererGPUProfiler)},
                {"scope", "pinned_rt64_existing_120_slot_histories_copied_under_thread_mutex"},
            };
        }
        else {
            packet["rt64_render_profilers"] = {
                {"available", false}, {"reason", "workload_thread_busy_no_wait"}};
        }
    }
    packet["detail_selection"] = "first_per_projection_then_first_mixed_w_per_projection_then_last_per_projection_then_uniform";
    packet["detail_cap"] = kMaxDetailedCalls;
    packet["detail_candidates"] = selected.size();
    packet["detail_sampled"] = encountered > selected.size();
    packet["detail_byte_trimmed"] = false;
    packet["truncated"] = truncated || encountered > recorded;
    packet["byte_cap"] = kMaxBytes;
    // Serialize the complete packet once. Each removed array entry has a known
    // serialized cost; avoid dumping the growing packet after every call.
    size_t serialized_bytes = packet.dump().size();
    while (serialized_bytes > kMaxBytes && !packet["calls"].empty()) {
        auto& calls = packet["calls"];
        serialized_bytes -= calls.back().dump().size() + (calls.size() > 1 ? 1 : 0);
        calls.erase(calls.end() - 1);
        --recorded;
        packet["recorded_calls"] = recorded;
        packet["detail_byte_trimmed"] = true;
    }
    // The numeric count and trim flag can alter the final byte length by a
    // handful of bytes. A single final check resolves that boundary case.
    serialized_bytes = packet.dump().size();
    while (serialized_bytes > kMaxBytes && !packet["calls"].empty()) {
        packet["calls"].erase(packet["calls"].end() - 1);
        packet["recorded_calls"] = --recorded;
        packet["detail_byte_trimmed"] = true;
        serialized_bytes = packet.dump().size();
    }
    // Rare workloads can exceed the byte budget with the compact manifest
    // alone. Retain its prefix with explicit counts rather than oversize a log.
    while (serialized_bytes > kMaxBytes && !packet["call_manifest"].empty()) {
        auto& manifest = packet["call_manifest"];
        serialized_bytes -= manifest.back().dump().size() + (manifest.size() > 1 ? 1 : 0);
        manifest.erase(manifest.end() - 1);
        packet["manifest_calls"] = manifest.size();
        packet["manifest_truncated"] = true;
    }
    if (packet["manifest_truncated"] == true) packet["truncated"] = true;
    // Metadata edits above can shift the serialized length by a few bytes.
    // Check the actual final packet, then remove only any remaining boundary
    // rows. If fixed descriptors alone exceed the cap, emit a small failure.
    serialized_bytes = packet.dump().size();
    while (serialized_bytes > kMaxBytes && !packet["call_manifest"].empty()) {
        auto& manifest = packet["call_manifest"];
        manifest.erase(manifest.end() - 1);
        packet["manifest_calls"] = manifest.size();
        packet["manifest_truncated"] = true;
        packet["truncated"] = true;
        serialized_bytes = packet.dump().size();
    }
    if (serialized_bytes > kMaxBytes) {
        log("artifact_draw_capture_failure", {
            {"graphics_task_sequence", context.sequence},
            {"reason", "fixed_descriptors_exceed_byte_cap"},
            {"byte_cap", kMaxBytes},
            {"fixed_descriptor_bytes", serialized_bytes},
            {"encountered_calls", encountered},
        });
        return;
    }
    log("artifact_draw_capture", std::move(packet));
    } catch (...) {
        // F4 diagnostics must never turn allocation, serialization, or a log
        // sink failure into a game failure.
        try {
            log("artifact_draw_capture_failure", {{"graphics_task_sequence", context.sequence}});
        } catch (...) {
        }
    }
}

} // namespace tooie::artifact_capture
