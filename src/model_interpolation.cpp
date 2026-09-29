#include "model_interpolation.hpp"
#include "camera_interpolation.hpp"
#include "scene_observer.hpp"
#include "model_camera_policy.hpp"
#include "replay_timing.hpp"

#include "recomp.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
using tooie::model_interpolation::MatrixRange;
using tooie::model_interpolation::Stats;
constexpr std::uint32_t rdram_size = 0x00800000U;
constexpr std::uint32_t matrix_size = 0x40U;
constexpr std::size_t max_draw_depth = 8;

struct Draw {
    std::uint32_t buffers = 0;
    std::uint32_t display_list_start = 0;
    std::uint32_t matrix_start = 0;
    std::uint32_t caller_s0 = 0;
    std::uint32_t position = 0;
    std::uint32_t rotation = 0;
    std::uint32_t scale_bits = 0;
    std::uint32_t extra = 0;
    std::uint32_t model = 0;
    std::uint64_t trace_index = 0;
    std::size_t depth = 0;
    std::uint64_t epoch = 0;
    bool valid = false;
    bool cpu_skinned = false;
    bool intro_draw = false;
    bool trace = false;
};
struct GuestDrawStack {
    std::array<Draw, max_draw_depth> draws{};
    std::size_t depth = 0;
    std::size_t overflow_depth = 0;
};
thread_local GuestDrawStack guest_draws;
thread_local std::size_t intro_draw_depth = 0;
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
};
std::mutex ranges_mutex;
std::array<PendingRange, tooie::model_interpolation::kMaxRangesPerTask> pending{};
std::size_t pending_count = 0;
std::array<Binding, tooie::model_interpolation::kMaxQueuedTasks> bindings{};
std::size_t binding_head = 0;
std::size_t binding_count = 0;
Stats range_stats{};
std::mutex trace_mutex;
std::atomic_uint64_t draw_index{0};
bool read_word(std::uint8_t* rdram, std::uint32_t address,
    std::uint32_t& value) noexcept;

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
    if (!ctx) return;
    auto& stack = guest_draws;
    for (std::size_t i = 0; i < stack.depth; ++i) stack.draws[i].valid = false;
    if (stack.overflow_depth != 0 || stack.depth == stack.draws.size()) {
        ++stack.overflow_depth;
        return;
    }
    Draw draw{};
    draw.buffers = static_cast<std::uint32_t>(ctx->r4);
    draw.caller_s0 = static_cast<std::uint32_t>(ctx->r16);
    draw.position = static_cast<std::uint32_t>(ctx->r5);
    draw.rotation = static_cast<std::uint32_t>(ctx->r6);
    draw.scale_bits = static_cast<std::uint32_t>(ctx->r7);
    draw.depth = stack.depth;
    draw.epoch = current_epoch.load(std::memory_order_acquire);
    draw.intro_draw = intro_draw_depth != 0;
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

void model_draw_cpu_skinning() noexcept {
    auto& stack = guest_draws;
    if (stack.overflow_depth == 0 && stack.depth != 0)
        stack.draws[stack.depth - 1].cpu_skinned = true;
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
    const bool ordinary_gameplay = interpolate_cpu_pose_camera({
        .level = level, .save_slot = slot, .game_type = game_type,
        .frontend_mode = frontend_mode,
        .replay_record_available = tooie::replay_timing::frame_refresh_rate() != 0,
        .map_available = scene.map_available,
        .scene_activation = scene.activation_active,
        .cutscene = tooie::camera_interpolation::cutscene_active(),
        .title_character_draw = draw.intro_draw});
    pending[pending_count++] = {{begin_phys, end_phys, ordinary_gameplay}, gfx_begin_phys,
        gfx_end_phys};
    ++range_stats.recorded_ranges;
    range_stats.pending_ranges = pending_count;
}

bool bind_task_ranges(std::uint32_t display_list,
    std::uint32_t data_size) noexcept {
    std::lock_guard lock(ranges_mutex);
    if (range_stats.disabled) {
        pending_count = 0;
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
    for (std::size_t i = 0; i < pending_count; ++i) {
        const PendingRange& candidate = pending[i];
        if (valid_task && candidate.gfx_begin >= task_begin &&
            candidate.gfx_end <= task_end) {
            binding.ranges[binding.count++] = candidate.matrix;
        } else {
            ++range_stats.rejected_ranges;
        }
    }
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
    std::lock_guard lock(ranges_mutex);
    pending_count = 0;
    binding_head = 0;
    binding_count = 0;
    range_stats = {};
}

} // namespace tooie::model_interpolation

extern "C" void tooie_model_draw_begin(std::uint8_t* rdram,
    recomp_context* ctx) noexcept {
    tooie::model_interpolation::model_draw_begin(rdram, ctx);
}

extern "C" void tooie_model_draw_cpu_skinning() noexcept {
    tooie::model_interpolation::model_draw_cpu_skinning();
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
