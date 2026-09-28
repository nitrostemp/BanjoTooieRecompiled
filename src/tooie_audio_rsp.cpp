// Tooie-specific adapter over pinned RSPRecomp output and N64ModernRuntime.
// The upstream DMEM/task load contract is retained; unsupported exit reasons
// report failure without the upstream assertion or a forged completion event.
#include "tooie_audio_rsp.hpp"
#include "platform_support.hpp"
#include "librecomp/rsp.hpp"
#include "audio_identity.hpp"
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

RspExitReason tooie_n_aspMain_mission_initial(uint8_t*, RspContext*);
RspExitReason tooie_n_aspMain_mission0(uint8_t*, RspContext*);
RspExitReason tooie_n_aspMain_mission1(uint8_t*, RspContext*);

namespace tooie::audio_rsp {
namespace {
constexpr uint32_t rdram_size = 0x800000;
std::mutex task_mutex;
std::once_flag initialized;
Log logger;
Failure failure_handler;
uint64_t task_sequence = 0;
bool detailed_success_logging = true;

bool log_success_detail() noexcept {
    // Normal player mode keeps one positive-path record without hashing and
    // flushing every audio task. Diagnostics retain the complete task stream.
    return detailed_success_logging || task_sequence == 1;
}

bool range(uint32_t address, size_t count) {
    const uint32_t segment = address & 0xE0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xA0000000u) return false;
    const uint32_t physical = address & 0x1FFFFFFFu;
    return (address & 7) == 0 && physical < rdram_size && count <= rdram_size - physical;
}
template<size_t N>
bool matches(const uint8_t* rdram, uint32_t address, const std::array<uint8_t, N>& expected) {
    if (!range(address, N)) return false;
    const uint32_t physical = address & 0x1FFFFFFFu;
    for (size_t i = 0; i < N; ++i) if (rdram[(physical + i) ^ 3] != expected[i]) return false;
    return true;
}
std::string guest_sha256(const uint8_t* rdram, uint32_t address, size_t size) {
    std::vector<uint8_t> canonical(size);
    const uint32_t physical = address & 0x1FFFFFFFu;
    for (size_t i = 0; i < size; ++i) canonical[i] = rdram[(physical + i) ^ 3];
    return platform::digest(canonical, true);
}
void log(const char* event, const nlohmann::json& data) {
    if (logger) logger(event, data);
}
bool fail(const std::string& reason) noexcept {
    // Host failure reporting cannot turn a rejected task into success, even if
    // a caller's logging or lifecycle callback also fails.
    try { log("audio_rsp_failure", {{"task_sequence", task_sequence}, {"reason", reason}}); } catch (...) {}
    try { if (failure_handler) failure_handler(reason); } catch (...) {}
    return false;
}
void init() {
    std::call_once(initialized, [] { recomp::rsp::constants_init(); });
}
RspExitReason execute(uint8_t* rdram, uint32_t ucode) {
    RspContext context{};
    auto result = tooie_n_aspMain_mission_initial(rdram, &context);
    uint64_t swaps = 0;
    while (result == RspExitReason::SwapOverlay) {
        const uint32_t offset = context.dma_dram_address - ucode;
        const int overlay = overlay_index(context.dma_mem_address, offset, context.r3);
        ++swaps;
        if (log_success_detail()) {
            log("audio_rsp_overlay", {{"task_sequence", task_sequence}, {"ordinal", swaps},
                {"imem", context.dma_mem_address}, {"source_offset", offset},
                {"dma_length_register", context.r3}, {"permutation", overlay},
                {"resume_address", context.resume_address}, {"resume_delay", context.resume_delay}});
        }
        if (overlay < 0) return RspExitReason::Unsupported;
        result = overlay == 0 ? tooie_n_aspMain_mission0(rdram, &context)
                              : tooie_n_aspMain_mission1(rdram, &context);
    }
    return result;
}
bool run(uint8_t* rdram, const OSTask* task) {
    std::lock_guard lock(task_mutex); // The pinned vector runtime owns global DMEM.
    ++task_sequence;
    try {
        const auto recognition = recognize_task(rdram, task);
        if (!recognition.supported) return fail(recognition.reason);
        init();
        const auto& t = task->t;
        if (log_success_detail()) {
            log("audio_rsp_task", {{"task_sequence", task_sequence}, {"type", t.type}, {"flags", t.flags},
                {"ucode", t.ucode}, {"ucode_size", t.ucode_size}, {"ucode_sha256", identity::code_sha256},
                {"ucode_data", t.ucode_data}, {"ucode_data_size", t.ucode_data_size},
                {"ucode_data_sha256", identity::data_sha256}, {"boot", t.ucode_boot},
                {"boot_sha256", identity::boot_sha256}, {"data_ptr", t.data_ptr}, {"data_size", t.data_size},
                {"command_sha256", guest_sha256(rdram, t.data_ptr, t.data_size)}});
        }
        // Same load layout as pinned librecomp/src/rsp.cpp. Its fixed 0xF80
        // data load exceeds OSTask.ucode_data_size=0x800; range checked above.
        std::memcpy(dmem + 0xFC0, task, sizeof(OSTask));
        dma_rdram_to_dmem(rdram, 0, t.ucode_data, 0xF7F);
        const auto result = execute(rdram, t.ucode);
        if (log_success_detail()) {
            log("audio_rsp_exit", {{"task_sequence", task_sequence}, {"exit_reason", static_cast<int>(result)},
                {"broke", result == RspExitReason::Broke}});
        }
        if (result != RspExitReason::Broke) return fail("Tooie audio RSP did not exit with Broke");
        return true;
    } catch (const std::exception& e) { return fail(e.what()); }
      catch (...) { return fail("unknown exception in Tooie audio RSP"); }
}
}
Recognition recognize_task(const uint8_t* rdram, const OSTask* task) {
    if (!rdram || !task) return {false, "null audio task or RDRAM"};
    const auto& t = task->t;
    if (t.type != 2) return {false, "unsupported RSP task type"};
    if (t.flags != 0 || t.ucode_size != 0x1000 || t.ucode_data_size != 0x800 || t.ucode_boot_size != 0xD0)
        return {false, "audio OSTask shape differs from core1 n_aspMain task"};
    if (!range(t.data_ptr, t.data_size) || t.data_size == 0 || (t.data_size & 7))
        return {false, "audio command list is empty, unaligned or outside original RDRAM"};
    if (!range(t.ucode_data, 0xF80)) return {false, "audio initial DMEM DMA exceeds RDRAM"};
    if (!matches(rdram, t.ucode, identity::code)) return {false, "unknown audio microcode or overlay bytes"};
    if (!matches(rdram, t.ucode_data, identity::data)) return {false, "unknown audio microcode data"};
    if (!matches(rdram, t.ucode_boot, identity::boot)) return {false, "unknown RSP boot bytes"};
    return {true, "Tooie NTSC-U n_aspMain with both known overlays"};
}
int overlay_index(uint32_t imem, uint32_t offset, uint32_t length) noexcept {
    if (imem != 0x1238) return -1;
    if (offset == 0x1B8 && length == 0xDC7) return 0;
    if (offset == 0xF80 && length == 0x847) return 1;
    return -1;
}
void configure(Log log_callback, Failure failure_callback, bool detailed_logging) {
    std::lock_guard lock(task_mutex);
    logger = std::move(log_callback);
    failure_handler = std::move(failure_callback);
    task_sequence = 0;
    detailed_success_logging = detailed_logging;
}
ultramodern::rsp::callbacks_t callbacks() { return {init, run}; }
}
