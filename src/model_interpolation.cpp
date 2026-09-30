#include "model_interpolation.hpp"
#include "camera_interpolation.hpp"
#include "scene_observer.hpp"
#include "model_camera_policy.hpp"
#include "replay_timing.hpp"
#include "model_draw_trace_window.hpp"
#include "model_actor_trace.hpp"
#include "cpu_actor_root_history.hpp"
#include "cpu_actor_root_admission.hpp"
#include "nest_visibility_policy.hpp"

#include "recomp.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>

namespace {
using tooie::model_interpolation::MatrixRange;
using tooie::model_interpolation::Stats;
constexpr std::uint32_t rdram_size = 0x00800000U;
constexpr std::uint32_t matrix_size = 0x40U;
constexpr std::size_t max_draw_depth = 8;
constexpr std::size_t max_attachment_depth = 8;
constexpr std::uint32_t backpack_attachment_role = 0x4241434BU;

struct Draw {
    std::uint8_t* rdram = nullptr;
    std::uint32_t buffers = 0;
    std::uint32_t display_list_start = 0;
    std::uint32_t matrix_start = 0;
    std::uint32_t caller_s0 = 0;
    std::uint32_t caller_ra = 0;
    std::uint32_t position = 0;
    std::uint32_t rotation = 0;
    std::uint32_t scale_bits = 0;
    std::uint32_t extra = 0;
    std::uint32_t model = 0;
    std::uint32_t model_identity = 0;
    std::uint32_t model_geometry = 0;
    std::uint32_t backpack_owner = 0;
    float nest_child_radius = 0.0f;
    std::uint64_t trace_index = 0;
    std::size_t depth = 0;
    std::uint64_t epoch = 0;
    bool valid = false;
    bool cpu_skinned = false;
    bool intro_draw = false;
    bool trace = false;
    bool nest_child_radius_valid = false;
};
struct GuestDrawStack {
    std::array<Draw, max_draw_depth> draws{};
    std::size_t depth = 0;
    std::size_t overflow_depth = 0;
};
thread_local GuestDrawStack guest_draws;
thread_local std::size_t intro_draw_depth = 0;
thread_local std::array<std::uint32_t, max_attachment_depth> backpack_owners{};
thread_local std::size_t backpack_depth = 0;
thread_local std::size_t backpack_overflow_depth = 0;
struct NestChildRadius {
    float radius = 0.0f;
    bool valid = false;
};
thread_local NestChildRadius pending_nest_child_radius;
thread_local const std::vector<MatrixRange>* active_task_ranges = nullptr;
std::atomic_uint64_t current_epoch{1};

struct Binding {
    std::uint32_t display_list = 0;
    std::array<MatrixRange, tooie::model_interpolation::kMaxRangesPerTask> ranges{};
    std::size_t count = 0;
};
struct PendingRange {
    MatrixRange matrix{};
    std::uint32_t gfx_begin = 0;
    std::uint32_t gfx_end = 0;
    tooie::model_interpolation::CpuActorRoot root{};
    bool smooth_root = false;
};
tooie::model_interpolation::CpuActorRootHistory root_history;
std::mutex ranges_mutex;
std::array<PendingRange, tooie::model_interpolation::kMaxRangesPerTask> pending{};
std::size_t pending_count = 0;
std::array<Binding, tooie::model_interpolation::kMaxQueuedTasks> bindings{};
std::size_t binding_head = 0;
std::size_t binding_count = 0;
Stats range_stats{};
std::mutex trace_mutex;
std::atomic_uint64_t draw_index{0};
std::atomic_uint32_t draw_trace_arm{0};
std::atomic_uint32_t draw_trace_seen{0};
std::atomic_bool draw_trace_active{false};
std::mutex draw_trace_mutex;
tooie::model_interpolation::DrawTraceWindow draw_trace_window;
std::filesystem::path draw_trace_directory;
std::ofstream draw_trace_file;
std::uint32_t draw_trace_slot = 0;
std::uint64_t draw_trace_index = 0;
std::atomic_uint64_t draw_trace_task{0};
std::uint32_t draw_trace_generation = 0;
std::size_t draw_trace_bytes = 0;
constexpr std::size_t draw_trace_byte_cap = 2U << 20;
struct DrawTraceRecord {
    tooie::model_interpolation::ActorTraceSnapshot actor{};
    std::uint64_t index = 0, epoch = 0;
    std::uint32_t caller_ra = 0, caller_s0 = 0, model = 0;
    std::uint32_t model_geometry = 0, model_identity = 0, backpack_owner = 0;
    std::uint32_t position = 0, rotation = 0, scale_bits = 0, extra = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::uint32_t gfx_begin = 0, gfx_end = 0, mtx_begin = 0, mtx_end = 0;
    bool cpu = false, intro = false, begin_valid = false;
    bool cursors_valid = false, buffers_match = false, physical_valid = false;
};
std::array<DrawTraceRecord, tooie::model_interpolation::DrawTraceWindow::max_draws>
    draw_trace_pending{};
std::size_t draw_trace_pending_count = 0;
bool read_word(std::uint8_t* rdram, std::uint32_t address,
    std::uint32_t& value) noexcept;
bool read_half(std::uint8_t* rdram, std::uint32_t address,
    std::uint16_t& value) noexcept;
std::uint32_t normalize_display_list(std::uint32_t address) noexcept;
bool physical_address(std::uint32_t address, std::uint32_t& physical) noexcept;

struct TraceConfig {
    bool enabled = false;
    std::uint64_t start = 0;
    std::uint64_t count = 200;
};

const TraceConfig& trace_config() noexcept {
    static const TraceConfig config = [] {
        TraceConfig result{};
        const char* enabled = std::getenv("TOOIE_MODEL_TRACE");
        result.enabled = enabled && enabled[0] == '1' && enabled[1] == '\0';
        if (result.enabled) {
            const char* start = std::getenv("TOOIE_MODEL_TRACE_START");
            if (start && start[0] >= '0' && start[0] <= '9') {
                char* end = nullptr;
                const auto value = std::strtoull(start, &end, 10);
                if (end && end != start && *end == '\0') result.start = value;
            }
            const char* count = std::getenv("TOOIE_MODEL_TRACE_COUNT");
            if (count && count[0] >= '0' && count[0] <= '9') {
                char* end = nullptr;
                const auto value = std::strtoull(count, &end, 10);
                if (end && end != count && *end == '\0' &&
                    value >= 1 && value <= 2000) result.count = value;
            }
        }
        return result;
    }();
    return config;
}

float read_float_or_zero(std::uint8_t* rdram, std::uint32_t address) noexcept {
    std::uint32_t bits = 0;
    return read_word(rdram, address, bits) ? std::bit_cast<float>(bits) : 0.0f;
}

void stop_draw_trace_locked() noexcept {
    draw_trace_file.close();
    draw_trace_window.stop();
    draw_trace_pending_count = 0;
    draw_trace_active.store(false, std::memory_order_release);
}

void observe_draw_trace_arm_locked() noexcept {
    const auto generation = draw_trace_arm.load(std::memory_order_acquire);
    const auto slot = draw_trace_window.request(generation);
    draw_trace_seen.store(draw_trace_window.seen, std::memory_order_release);
    if (!slot) return;
    draw_trace_generation = generation;
    draw_trace_file.close();
    draw_trace_slot = slot;
    draw_trace_bytes = 0;
    draw_trace_pending_count = 0;
    if (draw_trace_directory.empty()) { stop_draw_trace_locked(); return; }
    try {
        draw_trace_file.open(draw_trace_directory /
            ("model-draw-" + std::to_string(slot) + ".csv"),
            std::ios::out | std::ios::trunc);
    } catch (...) { stop_draw_trace_locked(); return; }
    if (!draw_trace_file) { stop_draw_trace_locked(); return; }
    constexpr char header[] = "kind,slot,request_generation,task_ordinal,display_list,task_size,draw_index,epoch,"
        "caller_ra,caller_s0,model,position_ptr,pos_x,pos_y,pos_z,rotation_ptr,scale_bits,extra,"
        "cpu,intro,begin_valid,cursors_valid,buffers_match,gfx_begin,gfx_end,"
        "mtx_begin,mtx_end,physical_valid,in_task,actor_candidate_valid,linked_valid,"
        "actor_words_hex,linked_words_hex,model_geometry,model_identity,backpack_owner\n";
    draw_trace_file.write(header, sizeof(header) - 1);
    draw_trace_bytes = sizeof(header) - 1;
    if (!draw_trace_file) { stop_draw_trace_locked(); return; }
    draw_trace_active.store(true, std::memory_order_release);
}

bool write_draw_trace_locked(const char* line, std::size_t length) noexcept {
    if (!draw_trace_file || length > draw_trace_byte_cap - draw_trace_bytes) {
        stop_draw_trace_locked();
        return false;
    }
    draw_trace_file.write(line, length);
    if (!draw_trace_file) { stop_draw_trace_locked(); return false; }
    draw_trace_bytes += length;
    return true;
}

void queue_draw_trace(std::uint8_t* rdram, const Draw& draw,
    std::uint32_t gfx_end, std::uint32_t mtx_end,
    bool buffers_match, bool cursors_valid) noexcept {
    if (!draw_trace_active.load(std::memory_order_acquire) &&
        draw_trace_arm.load(std::memory_order_acquire) ==
            draw_trace_seen.load(std::memory_order_acquire)) return;
    std::lock_guard lock(draw_trace_mutex);
    observe_draw_trace_arm_locked();
    if (!draw_trace_window.record_draw()) return;
    auto &record = draw_trace_pending[draw_trace_pending_count++];
    record = {};
    record.index = ++draw_trace_index;
    record.epoch = draw.epoch;
    record.caller_ra = draw.caller_ra;
    record.caller_s0 = draw.caller_s0;
    record.model = draw.model;
    record.model_geometry = draw.model_geometry;
    record.model_identity = draw.model_identity;
    record.backpack_owner = draw.backpack_owner;
    record.position = draw.position;
    record.rotation = draw.rotation;
    record.scale_bits = draw.scale_bits;
    record.extra = draw.extra;
    record.actor = tooie::model_interpolation::snapshot_actor_candidate(
        draw.caller_s0, draw.position,
        [rdram](std::uint32_t address, std::uint32_t& value) {
            return read_word(rdram, address, value);
        });
    record.x = read_float_or_zero(rdram, draw.position);
    record.y = read_float_or_zero(rdram, draw.position + 4U);
    record.z = read_float_or_zero(rdram, draw.position + 8U);
    record.cpu = draw.cpu_skinned;
    record.intro = draw.intro_draw;
    record.begin_valid = draw.valid;
    record.cursors_valid = cursors_valid;
    record.buffers_match = buffers_match;
    record.physical_valid = draw.valid && cursors_valid && buffers_match &&
        physical_address(draw.display_list_start, record.gfx_begin) &&
        physical_address(gfx_end, record.gfx_end) &&
        physical_address(draw.matrix_start, record.mtx_begin) &&
        physical_address(mtx_end, record.mtx_end) &&
        record.gfx_begin < record.gfx_end && record.mtx_begin < record.mtx_end &&
        (record.gfx_begin % 8U) == 0 && (record.gfx_end % 8U) == 0 &&
        (record.mtx_begin % 8U) == 0 &&
        ((record.mtx_end - record.mtx_begin) % matrix_size) == 0 &&
        ((record.mtx_end - record.mtx_begin) / matrix_size) <= 4096U;
}

void bind_draw_trace(std::uint32_t display_list, std::uint32_t data_size) noexcept {
    const auto task_ordinal = draw_trace_task.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!draw_trace_active.load(std::memory_order_acquire) &&
        draw_trace_arm.load(std::memory_order_acquire) ==
            draw_trace_seen.load(std::memory_order_acquire)) return;
    std::lock_guard lock(draw_trace_mutex);
    observe_draw_trace_arm_locked();
    if (!draw_trace_window.active) return;
    const auto list = normalize_display_list(display_list);
    const bool valid_task = list < rdram_size && data_size <= rdram_size - list &&
        data_size != 0 && (list % 8U) == 0 && (data_size % 8U) == 0;
    char line[1280];
    int length = std::snprintf(line, sizeof(line),
        "task,%u,%u,%llu,%08x,%u,,,,,,,,,,,,,,,,,,,,,,,,,,,\n",
        draw_trace_slot, draw_trace_generation,
        static_cast<unsigned long long>(task_ordinal), list, data_size);
    if (length < 0 || std::size_t(length) >= sizeof(line) ||
        !write_draw_trace_locked(line, std::size_t(length))) return;
    for (std::size_t i = 0; i < draw_trace_pending_count; ++i) {
        const auto &record = draw_trace_pending[i];
        const bool in_task = valid_task && record.physical_valid &&
            record.gfx_begin >= list && record.gfx_end <= list + data_size;
        // Raw big-endian word notation; no inference that the saved register
        // identifies an actor. Empty fields distinguish rejected candidates.
        char actor_hex[39 * 8 + 1]{};
        char linked_hex[12 * 8 + 1]{};
        if (record.actor.actor_valid)
            for (std::size_t word = 0; word < record.actor.actor_words.size(); ++word)
                std::snprintf(actor_hex + word * 8, 9, "%08x", record.actor.actor_words[word]);
        if (record.actor.linked_valid)
            for (std::size_t word = 0; word < record.actor.linked_words.size(); ++word)
                std::snprintf(linked_hex + word * 8, 9, "%08x", record.actor.linked_words[word]);
        length = std::snprintf(line, sizeof(line),
            "draw,%u,%u,%llu,%08x,%u,%llu,%llu,%08x,%08x,%08x,%08x,%.7g,%.7g,%.7g,"
            "%08x,%08x,%08x,%u,%u,%u,%u,%u,%08x,%08x,%08x,%08x,%u,%u,%u,%u,%s,%s,%08x,%08x,%08x\n",
            draw_trace_slot, draw_trace_generation,
            static_cast<unsigned long long>(task_ordinal), list,
            data_size, static_cast<unsigned long long>(record.index),
            static_cast<unsigned long long>(record.epoch), record.caller_ra,
            record.caller_s0, record.model, record.position,
            double(record.x), double(record.y), double(record.z),
            record.rotation, record.scale_bits, record.extra,
            unsigned(record.cpu), unsigned(record.intro), unsigned(record.begin_valid),
            unsigned(record.cursors_valid), unsigned(record.buffers_match),
            record.gfx_begin, record.gfx_end, record.mtx_begin, record.mtx_end,
            unsigned(record.physical_valid), unsigned(in_task),
            unsigned(record.actor.actor_valid), unsigned(record.actor.linked_valid),
            actor_hex, linked_hex, record.model_geometry, record.model_identity,
            record.backpack_owner);
        if (length < 0 || std::size_t(length) >= sizeof(line) ||
            !write_draw_trace_locked(line, std::size_t(length))) return;
    }
    draw_trace_pending_count = 0;
    if (draw_trace_window.record_task()) stop_draw_trace_locked();
}

void trace_draw(std::uint8_t* rdram, const Draw& draw,
    std::uint32_t display_list_end, std::uint32_t matrix_end,
    bool saved_buffers_match) noexcept {
    if (!draw.trace) return;
    const float x = read_float_or_zero(rdram, draw.position);
    const float y = read_float_or_zero(rdram, draw.position + 4U);
    const float z = read_float_or_zero(rdram, draw.position + 8U);
    std::lock_guard lock(trace_mutex);
    std::fprintf(stderr,
        "TOOIE_MODEL_DRAW n=%llu depth=%zu caller_s0=%08x buffers=%08x "
        "gfx=%08x..%08x mtx=%08x..%08x cpu=%d valid=%d "
        "saved_buffers_match=%d posptr=%08x pos=%.4g,%.4g,%.4g "
        "rotptr=%08x scale_bits=%08x extra=%08x model=%08x\n",
        static_cast<unsigned long long>(draw.trace_index), draw.depth,
        draw.caller_s0, draw.buffers, draw.display_list_start,
        display_list_end, draw.matrix_start, matrix_end,
        draw.cpu_skinned ? 1 : 0, draw.valid ? 1 : 0,
        saved_buffers_match ? 1 : 0, draw.position,
        static_cast<double>(x), static_cast<double>(y), static_cast<double>(z),
        draw.rotation, draw.scale_bits, draw.extra, draw.model);
}

std::uint32_t normalize_display_list(std::uint32_t address) noexcept {
    return address & 0x03FFFFFFU;
}

bool physical_address(std::uint32_t address, std::uint32_t& physical) noexcept {
    const auto region = address & 0xE0000000U;
    if (region != 0x80000000U && region != 0xA0000000U) return false;
    physical = address & 0x1FFFFFFFU;
    return physical < rdram_size;
}

bool read_word(std::uint8_t* rdram, std::uint32_t address,
    std::uint32_t& value) noexcept {
    std::uint32_t physical = 0;
    if (!rdram || !physical_address(address, physical) ||
        (physical & 3U) != 0 || physical > rdram_size - 4U) return false;
    value = static_cast<std::uint32_t>(MEM_W(0, static_cast<gpr>(
        static_cast<std::int32_t>(address))));
    return true;
}

bool read_half(std::uint8_t* rdram, std::uint32_t address,
    std::uint16_t& value) noexcept {
    std::uint32_t physical = 0;
    if (!rdram || !physical_address(address, physical) ||
        (physical & 1U) != 0 || physical > rdram_size - 2U) return false;
    value = static_cast<std::uint16_t>(MEM_HU(0, static_cast<gpr>(
        static_cast<std::int32_t>(address))));
    return true;
}

void disable_locked(bool overflow) noexcept {
    range_stats.disabled = true;
    if (overflow) ++range_stats.overflows;
    else ++range_stats.mismatches;
    pending_count = 0;
    binding_head = 0;
    binding_count = 0;
    range_stats.pending_ranges = 0;
    range_stats.queued_tasks = 0;
}
} // namespace

namespace tooie::model_interpolation {

void model_draw_begin(std::uint8_t* rdram, recomp_context* ctx) noexcept {
    const auto nest_child_radius = pending_nest_child_radius;
    pending_nest_child_radius = {};
    if (!ctx) return;
    auto& stack = guest_draws;
    for (std::size_t i = 0; i < stack.depth; ++i) stack.draws[i].valid = false;
    if (stack.overflow_depth != 0 || stack.depth == stack.draws.size()) {
        ++stack.overflow_depth;
        return;
    }
    Draw draw{};
    draw.rdram = rdram;
    draw.buffers = static_cast<std::uint32_t>(ctx->r4);
    draw.caller_ra = static_cast<std::uint32_t>(ctx->r31);
    draw.caller_s0 = static_cast<std::uint32_t>(ctx->r16);
    draw.position = static_cast<std::uint32_t>(ctx->r5);
    draw.rotation = static_cast<std::uint32_t>(ctx->r6);
    draw.scale_bits = static_cast<std::uint32_t>(ctx->r7);
    draw.depth = stack.depth;
    draw.epoch = current_epoch.load(std::memory_order_acquire);
    draw.intro_draw = intro_draw_depth != 0;
    draw.backpack_owner = backpack_overflow_depth == 0 && backpack_depth != 0
        ? backpack_owners[backpack_depth - 1] : 0;
    draw.nest_child_radius = nest_child_radius.radius;
    draw.nest_child_radius_valid = nest_child_radius.valid;
    const std::uint32_t caller_sp = static_cast<std::uint32_t>(ctx->r29);
    read_word(rdram, draw.buffers, draw.display_list_start);
    draw.valid = read_word(rdram, draw.buffers + 4U, draw.matrix_start);
    read_word(rdram, caller_sp + 0x10U, draw.extra);
    read_word(rdram, caller_sp + 0x14U, draw.model);
    const auto& trace = trace_config();
    if (trace.enabled) {
        draw.trace_index = draw_index.fetch_add(1, std::memory_order_relaxed);
        draw.trace = draw.trace_index >= trace.start &&
            draw.trace_index - trace.start < trace.count;
    }
    stack.draws[stack.depth++] = draw;
}

void model_draw_cpu_skinning(std::uint8_t* rdram, recomp_context* ctx) noexcept {
    auto& stack = guest_draws;
    if (stack.overflow_depth == 0 && stack.depth != 0) {
        auto& draw = stack.draws[stack.depth - 1];
        std::uint32_t geometry = 0;
        // This renderer-state value is dbanim's transient vertex-list base.
        // Keep it only for F4 provenance; its double-buffered address must
        // never identify a root across submitted tasks.
        if (!read_word(draw.rdram, 0x8012C964U, geometry) ||
            !actor_trace_guest_range(geometry, 4U) ||
            (draw.cpu_skinned && draw.model_geometry != geometry))
            geometry = 0;
        draw.model_geometry = geometry;
        const auto stack_pointer = ctx ? static_cast<std::uint32_t>(ctx->r29) : 0U;
        const auto model = read_cpu_model_identity(stack_pointer,
            [rdram](std::uint32_t address, std::uint32_t& value) {
                return read_word(rdram, address, value);
            });
        if (!model || (draw.cpu_skinned && draw.model_identity != *model))
            draw.model_identity = 0;
        else
            draw.model_identity = *model;
        draw.cpu_skinned = true;
    }
}

void model_draw_end(std::uint8_t* rdram, recomp_context* ctx) noexcept {
    auto& stack = guest_draws;
    if (stack.overflow_depth != 0) {
        --stack.overflow_depth;
        return;
    }
    if (stack.depth == 0 || !ctx) return;
    const Draw draw = stack.draws[--stack.depth];
    std::uint32_t saved_buffers = 0;
    std::uint32_t display_list_end = 0;
    std::uint32_t matrix_end = 0;
    const bool saved_buffers_match = read_word(rdram,
            static_cast<std::uint32_t>(ctx->r29) + 0xF8U, saved_buffers) &&
        saved_buffers == draw.buffers;
    const bool cursors_valid = read_word(rdram, draw.buffers,
            display_list_end) &&
        read_word(rdram, draw.buffers + 4U, matrix_end);
    trace_draw(rdram, draw, display_list_end, matrix_end,
        saved_buffers_match);
    queue_draw_trace(rdram, draw, display_list_end, matrix_end,
        saved_buffers_match, cursors_valid);
    if (!draw.cpu_skinned || !draw.valid ||
        draw.epoch != current_epoch.load(std::memory_order_acquire)) return;

    std::uint32_t begin_phys = 0;
    std::uint32_t end_phys = 0;
    std::uint32_t gfx_begin_phys = 0;
    std::uint32_t gfx_end_phys = 0;
    const bool valid = saved_buffers_match && cursors_valid &&
        physical_address(draw.matrix_start, begin_phys) &&
        physical_address(matrix_end, end_phys) &&
        physical_address(draw.display_list_start, gfx_begin_phys) &&
        physical_address(display_list_end, gfx_end_phys) &&
        begin_phys < end_phys && end_phys <= rdram_size &&
        gfx_begin_phys < gfx_end_phys && gfx_end_phys <= rdram_size &&
        (gfx_begin_phys % 8U) == 0 && (gfx_end_phys % 8U) == 0 &&
        (begin_phys % 8U) == 0 && ((end_phys - begin_phys) % matrix_size) == 0 &&
        ((end_phys - begin_phys) / matrix_size) <= 4096U;
    std::lock_guard lock(ranges_mutex);
    if (range_stats.disabled) return;
    if (!valid) {
        ++range_stats.invalid_ranges;
        return;
    }
    if (pending_count == pending.size()) {
        disable_locked(true);
        return;
    }
    const auto scene = tooie::scene::snapshot();
    const auto level = static_cast<std::uint8_t>(MEM_BU(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x8012762CU))));
    const auto slot = static_cast<std::int8_t>(MEM_B(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F1U))));
    const auto game_type = static_cast<std::uint8_t>(MEM_BU(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x8012B3F2U))));
    const auto frontend_mode = static_cast<std::uint8_t>(MEM_BU(0,
        static_cast<gpr>(static_cast<std::int32_t>(0x80127760U))));
    const bool shared_scene_camera = interpolate_cpu_pose_camera({
        .level = level, .save_slot = slot, .game_type = game_type,
        .frontend_mode = frontend_mode,
        .replay_record_available = tooie::replay_timing::frame_refresh_rate() != 0,
        .map_available = scene.map_available,
        .scene_activation = scene.activation_active,
        .cutscene = tooie::camera_interpolation::cutscene_active(),
        .original_cutscene_motion = tooie::camera_interpolation::configured_cutscene_motion() ==
            tooie::camera_interpolation::CutsceneMotion::Original,
        .title_character_draw = draw.intro_draw});
    auto& range = pending[pending_count++];
    range = {{begin_phys, end_phys, shared_scene_camera}, gfx_begin_phys, gfx_end_phys};
    // Actor-owned single CPU roots share the selected output interpolation
    // weights. RT64 validates local pose correspondence for each frame pair.
    if (shared_scene_camera && end_phys - begin_phys == matrix_size) {
        auto read = [&](std::uint32_t address, std::uint32_t& value) {
            return read_word(rdram, address, value);
        };
        auto root = read_cpu_actor_root(draw.caller_s0, draw.position,
            draw.epoch, read);
        if (root && draw.model_identity != 0) {
            root->key.source_identity = draw.model_identity;
            range.smooth_root = true;
            range.root = *root;
        } else if (draw.backpack_owner != 0 && draw.model_identity != 0) {
            root = read_cpu_attachment_root(draw.backpack_owner,
                backpack_attachment_role, draw.position, draw.model_identity,
                draw.epoch, read);
            if (root) {
                range.smooth_root = true;
                range.root = *root;
            }
        }
    }
    ++range_stats.recorded_ranges;
    range_stats.pending_ranges = pending_count;
}

bool bind_task_ranges(std::uint32_t display_list,
    std::uint32_t data_size) noexcept {
    bind_draw_trace(display_list, data_size);
    std::lock_guard lock(ranges_mutex);
    if (range_stats.disabled) {
        pending_count = 0;
        root_history.reset();
        return false;
    }
    ++range_stats.submitted_tasks;
    if (binding_count == bindings.size()) {
        disable_locked(true);
        return false;
    }
    Binding& binding = bindings[(binding_head + binding_count) % bindings.size()];
    binding.display_list = normalize_display_list(display_list);
    binding.count = 0;
    const std::uint32_t task_begin = binding.display_list;
    const bool valid_task = task_begin < rdram_size && data_size != 0 &&
        (task_begin % 8U) == 0 && (data_size % 8U) == 0 &&
        data_size <= rdram_size - task_begin;
    const std::uint32_t task_end = valid_task ? task_begin + data_size : 0;
    std::array<CpuActorRoot, kMaxRangesPerTask> roots{};
    std::array<std::size_t, kMaxRangesPerTask> root_indices{};
    std::array<std::uint32_t, kMaxRangesPerTask> root_ids{};
    std::size_t root_count = 0;
    for (std::size_t i = 0; i < pending_count; ++i) {
        const PendingRange& candidate = pending[i];
        if (valid_task && candidate.gfx_begin >= task_begin &&
            candidate.gfx_end <= task_end) {
            if (candidate.smooth_root) {
                root_indices[root_count] = binding.count;
                roots[root_count++] = candidate.root;
            }
            binding.ranges[binding.count++] = candidate.matrix;
        } else {
            ++range_stats.rejected_ranges;
        }
    }
    root_history.assign(std::span(roots).first(root_count),
        std::span(root_ids).first(root_count));
    for (std::size_t i = 0; i < root_count; ++i)
        binding.ranges[root_indices[i]].root_id = root_ids[i];
    pending_count = 0;
    ++binding_count;
    range_stats.pending_ranges = 0;
    range_stats.queued_tasks = binding_count;
    return true;
}

std::vector<MatrixRange> consume_task_ranges(std::uint32_t display_list) noexcept {
    std::lock_guard lock(ranges_mutex);
    if (range_stats.disabled) return {};
    if (binding_count == 0 ||
        bindings[binding_head].display_list != normalize_display_list(display_list)) {
        disable_locked(false);
        return {};
    }
    Binding& binding = bindings[binding_head];
    std::vector<MatrixRange> result;
    try {
        result.assign(binding.ranges.begin(), binding.ranges.begin() + binding.count);
    } catch (...) {
        disable_locked(true);
        return {};
    }
    binding.count = 0;
    binding_head = (binding_head + 1) % bindings.size();
    --binding_count;
    ++range_stats.consumed_tasks;
    range_stats.queued_tasks = binding_count;
    return result;
}

TaskScope::TaskScope(const std::vector<MatrixRange>& ranges) noexcept
    : previous_(active_task_ranges) {
    active_task_ranges = &ranges;
}

TaskScope::~TaskScope() {
    active_task_ranges = previous_;
}

bool original_pose_for_matrix(std::uint32_t physical) noexcept {
    const auto* ranges = active_task_ranges;
    if (!ranges || physical >= rdram_size || physical > rdram_size - matrix_size)
        return false;
    for (const MatrixRange& range : *ranges) {
        if (physical >= range.begin && physical <= range.end - matrix_size &&
            ((physical - range.begin) % matrix_size) == 0) return true;
    }
    return false;
}

std::uint32_t original_pose_root_id_for_matrix(std::uint32_t physical) noexcept {
    if (!active_task_ranges || physical >= rdram_size || physical > rdram_size - matrix_size)
        return 0;
    std::uint32_t result = 0;
    bool found = false;
    for (const auto& range : *active_task_ranges) {
        if (physical >= range.begin && physical < range.end) {
            if (found || physical != range.begin || range.end - range.begin != matrix_size ||
                !range.interpolate_camera || range.root_id < CpuActorRootHistory::first_id ||
                range.root_id > CpuActorRootHistory::last_id) return 0;
            result = range.root_id;
            found = true;
        }
    }
    return result;
}

bool original_pose_camera_interpolation_for_matrix(std::uint32_t physical) noexcept {
    const auto* ranges = active_task_ranges;
    if (!ranges || physical >= rdram_size || physical > rdram_size - matrix_size)
        return false;
    bool found = false;
    for (const MatrixRange& range : *ranges) {
        if (physical >= range.begin && physical <= range.end - matrix_size &&
            ((physical - range.begin) % matrix_size) == 0) {
            if (!range.interpolate_camera) return false;
            found = true;
        }
    }
    return found;
}

Stats stats() noexcept {
    std::lock_guard lock(ranges_mutex);
    return range_stats;
}

void reset() noexcept {
    current_epoch.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(draw_trace_mutex);
        stop_draw_trace_locked();
        draw_trace_window.cancel(draw_trace_arm.load(std::memory_order_acquire));
        draw_trace_seen.store(draw_trace_window.seen, std::memory_order_release);
    }
    std::lock_guard lock(ranges_mutex);
    pending_count = 0;
    root_history.reset();
    backpack_depth = 0;
    backpack_overflow_depth = 0;
    pending_nest_child_radius = {};
    binding_head = 0;
    binding_count = 0;
    range_stats = {};
}

void nest_body_draw_begin(std::uint8_t* rdram, recomp_context* ctx) noexcept {
    pending_nest_child_radius = {};
    if (!ctx) return;
    const auto actor = static_cast<std::uint32_t>(ctx->r16);
    // At the source-owned func_800DE448 call, position is its first argument.
    // The wrapper moves it to a1 only when it enters func_800DE498.
    const auto position = static_cast<std::uint32_t>(ctx->r4);
    if (actor == 0 || actor > std::numeric_limits<std::uint32_t>::max() - 4U ||
        position != actor + 4U) return;
    std::uint32_t linked = 0;
    if (!read_word(rdram, actor, linked)) return;
    float radius = 0.0f;
    const bool valid = tooie::nest_visibility::cached_child_radius(linked,
        [rdram](std::uint32_t address, std::uint32_t& value) {
            return read_word(rdram, address, value);
        }, [rdram](std::uint32_t address, std::uint16_t& value) {
            return read_half(rdram, address, value);
        }, radius);
    if (valid) pending_nest_child_radius = {radius, true};
}

void nest_visibility_expand(recomp_context* ctx) noexcept {
    if (!ctx || guest_draws.overflow_depth != 0 || guest_draws.depth == 0) return;
    const auto& draw = guest_draws.draws[guest_draws.depth - 1];
    if (!draw.nest_child_radius_valid) return;
    const float existing = std::bit_cast<float>(static_cast<std::uint32_t>(ctx->r5));
    const float scale = std::bit_cast<float>(draw.scale_bits);
    const float expanded = tooie::nest_visibility::expanded_radius(existing,
        draw.nest_child_radius, scale);
    // Only a finite growth is permitted; all rejected inputs retain their
    // exact guest-provided bit pattern.
    if (std::isfinite(expanded) && std::isfinite(existing) && expanded > existing)
        ctx->r5 = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(expanded));
}

void set_trace_directory(const std::filesystem::path& directory) noexcept {
    std::lock_guard lock(draw_trace_mutex);
    try { draw_trace_directory = directory; }
    catch (...) { draw_trace_directory.clear(); }
}

void arm_trace() noexcept {
    draw_trace_arm.fetch_add(1, std::memory_order_release);
}

void backpack_draw_begin(std::uint32_t owner) noexcept {
    // Every source-bracketed begin consumes exactly one matching end. A failed
    // nested begin must not let its end pop a valid enclosing PlayerState.
    if (backpack_overflow_depth != 0 || owner == 0 ||
        backpack_depth == backpack_owners.size()) {
        ++backpack_overflow_depth;
        return;
    }
    backpack_owners[backpack_depth++] = owner;
}

void backpack_draw_end() noexcept {
    if (backpack_overflow_depth != 0) {
        --backpack_overflow_depth;
        return;
    }
    if (backpack_depth != 0) --backpack_depth;
}

} // namespace tooie::model_interpolation

extern "C" void tooie_model_draw_begin(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    tooie::model_interpolation::model_draw_begin(rdram, ctx);
}

extern "C" void tooie_model_draw_cpu_skinning(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    tooie::model_interpolation::model_draw_cpu_skinning(rdram, ctx);
}

extern "C" void tooie_model_backpack_draw_begin(std::uint32_t owner) noexcept {
    tooie::model_interpolation::backpack_draw_begin(owner);
}

extern "C" void tooie_model_backpack_draw_end() noexcept {
    tooie::model_interpolation::backpack_draw_end();
}

extern "C" void tooie_model_intro_draw_begin() noexcept {
    ++intro_draw_depth;
}

extern "C" void tooie_model_intro_draw_end() noexcept {
    if (intro_draw_depth != 0) --intro_draw_depth;
}

extern "C" void tooie_model_draw_end(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    tooie::model_interpolation::model_draw_end(rdram, ctx);
}

extern "C" void tooie_nest_body_draw_begin(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    tooie::model_interpolation::nest_body_draw_begin(rdram, ctx);
}

extern "C" void tooie_nest_visibility_expand(recomp_context* ctx) noexcept {
    tooie::model_interpolation::nest_visibility_expand(ctx);
}
