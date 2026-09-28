"""Materialize opt-in device checkpoints over the pinned continuous runtime.

Only the separate persistent-runtime target consumes these derived files. The
normal player runtime, pinned dependency tree, and continuous generator remain
untouched. Input anchors fail closed when the prepared runtime changes.
"""

import argparse
import hashlib
import json
from pathlib import Path


def once(text: str, old: str, new: str, owner: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"{owner}: expected one unchanged anchor, found {text.count(old)}")
    return text.replace(old, new, 1)


def events(text: str) -> str:
    owner = "events"
    if "tooie_persistent_events_export" in text:
        raise ValueError("events: device hooks already installed")
    text = ('#include "persistent_state_devices.hpp"\n'
            '#include <condition_variable>\n#include <memory>\n') + text
    text = once(text,
        'using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, DummyWorkloadAction>;',
        '''struct PersistentRendererCompletion {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool cancelled{false};
    bool done = false;
    bool success = false;
};
struct PersistentRendererAction {
    std::uint64_t epoch;
    bool (*callback)(ultramodern::renderer::RendererContext*, std::uint64_t) noexcept;
    std::shared_ptr<PersistentRendererCompletion> completion;
};
using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction,
    DummyWorkloadAction, PersistentRendererAction>;
// Diagnostic only: 0 startup, 1 loop/checkpoint, 2 dequeue, 3 DP parse wait,
// 4 display-list parse, 5 DP completion wait, 6 VI screen, 7 config,
// 8 owner restore, 9 dummy workload. Never used for admission.
static std::atomic_uint32_t tooie_device_gfx_phase{0};''', owner)
    text = once(text, "    int remaining_retraces = 1;\n", "", owner)
    text = text.replace("remaining_retraces", "tooie_device_remaining_retraces")
    text = once(text, "void vi_thread_func() {",
        "static int tooie_device_remaining_retraces = 1;\n\nvoid vi_thread_func() {", owner)
    text = once(text, "    while (!exited) {\n        // Determine the next VI time",
        "    while (!exited) {\n"
        "        if (!tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Vi)) return;\n"
        "        // Determine the next VI time", owner)
    text = once(text,
        "            while (!exited.load() && !tooie::lifecycle::stopping()) {\n"
        "                const auto deadline = tooie::timing::host_deadline_for_virtual(",
        "            while (!exited.load() && !tooie::lifecycle::stopping()) {\n"
        "                if (!tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Vi)) return;\n"
        "                const auto deadline = tooie::timing::host_deadline_for_virtual(", owner)
    text = once(text,
        "            if (exited.load() || tooie::lifecycle::stopping()) return;",
        "            if (exited.load() || tooie::lifecycle::stopping() ||\n"
        "                tooie::persistent_state::devices::terminal_abort_requested()) return;", owner)
    text = once(text, "        events_context.sp_task_queue.wait_dequeue(task);",
        "        if (tooie::persistent_state::devices::experiment_enabled()) {\n"
        "            if (!events_context.sp_task_queue.wait_dequeue_timed(task, "
        "std::chrono::milliseconds{2})) {\n"
        "                if (!tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Sp)) return;\n"
        "                continue;\n"
        "            }\n"
        "        } else events_context.sp_task_queue.wait_dequeue(task);", owner)
    text = once(text, "    while (true) {\n        // Wait until an RSP task has been sent",
        "    while (true) {\n"
        "        if (tooie::persistent_state::devices::terminal_abort_requested()) return;\n"
        "        // Wait until an RSP task has been sent", owner)
    text = once(text, "        if (task == nullptr) {",
        "        if (tooie::persistent_state::devices::terminal_abort_requested()) return;\n"
        "        if (task == nullptr) {", owner)
    text = once(text, "        sp_complete();\n    }\n    } catch (...) {",
        "        sp_complete();\n"
        "        if (events_context.sp_task_queue.size_approx() == 0 &&\n"
        "            !tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Sp)) return;\n"
        "    }\n    } catch (...) {", owner)
    text = once(text, "    while (!exited) {\n        // Try to pull an action from the queue",
        "    while (!exited) {\n"
        "        tooie_device_gfx_phase.store(1, std::memory_order_release);\n"
        "        if (tooie::persistent_state::devices::terminal_abort_requested()) break;\n"
        "        if (events_context.action_queue.size_approx() == 0 &&\n"
        "            !tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Graphics)) break;\n"
        "        // Try to pull an action from the queue", owner)
    text = once(text, "        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {",
        "        tooie_device_gfx_phase.store(2, std::memory_order_release);\n"
        "        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {\n"
        "            if (tooie::persistent_state::devices::terminal_abort_requested()) break;", owner)
    text = once(text, "                if (!tooie::dp::admit(tooie::dp::Stage::parse,",
        "                tooie_device_gfx_phase.store(3, std::memory_order_release);\n"
        "                if (!tooie::dp::admit(tooie::dp::Stage::parse,", owner)
    text = once(text, "                renderer_context->send_dl(&task_action->task);",
        "                tooie_device_gfx_phase.store(4, std::memory_order_release);\n"
        "                renderer_context->send_dl(&task_action->task);", owner)
    text = once(text, "                if (!tooie::dp::admit(tooie::dp::Stage::completion,",
        "                tooie_device_gfx_phase.store(5, std::memory_order_release);\n"
        "                if (!tooie::dp::admit(tooie::dp::Stage::completion,", owner)
    text = once(text, "                renderer_context->update_screen();",
        "                tooie_device_gfx_phase.store(6, std::memory_order_release);\n"
        "                renderer_context->update_screen();", owner)
    text = once(text, "                if (renderer_context->update_config(old_config, new_config)) {",
        "                tooie_device_gfx_phase.store(7, std::memory_order_release);\n"
        "                if (renderer_context->update_config(old_config, new_config)) {", owner)
    text = once(text,
        '            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {',
        '''            else if (const auto* persistent = std::get_if<PersistentRendererAction>(&action)) {
                tooie_device_gfx_phase.store(8, std::memory_order_release);
                bool success = false;
                if (!persistent->completion->cancelled.load() &&
                    persistent->epoch == tooie::persistent_state::devices::producer_epoch())
                    success = persistent->callback(renderer_context.get(), persistent->epoch);
                {
                    std::lock_guard lock(persistent->completion->mutex);
                    persistent->completion->success = success;
                    persistent->completion->done = true;
                }
                persistent->completion->cv.notify_all();
            }
            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {''', owner)
    text = once(text, "                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);",
        "                tooie_device_gfx_phase.store(9, std::memory_order_release);\n"
        "                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);", owner)
    text += r'''

extern "C" void tooie_persistent_events_graphics_diagnostic(
    std::uint32_t* phase, std::uint64_t* queue_depth,
    std::uint32_t* dp_status) noexcept {
    if (phase) *phase = tooie_device_gfx_phase.load(std::memory_order_acquire);
    if (queue_depth) *queue_depth = events_context.action_queue.size_approx();
    if (dp_status) *dp_status = tooie::dp::status.load(std::memory_order_acquire);
}

// Called only while the five producer workers are parked. The mode pointer is
// converted to a guest address; no native pointer enters the snapshot.
extern "C" bool tooie_persistent_events_export(
    tooie::persistent_state::devices::Snapshot* out) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !out || !events_context.rdram) return false;
    if (events_context.action_queue.size_approx() != 0 ||
        events_context.sp_task_queue.size_approx() != 0) return false;
    const auto mode_address = [](const OSViMode* mode, std::uint8_t* memory,
                                 std::uint32_t& guest) noexcept {
        if (mode == &dummy_mode) { guest = 0; return true; }
        const auto base = reinterpret_cast<std::uintptr_t>(memory);
        const auto value = reinterpret_cast<std::uintptr_t>(mode);
        if (value < base || value - base >= 0x800000U) return false;
        guest = 0x80000000U + static_cast<std::uint32_t>(value - base);
        return true;
    };
    out->total_vis = total_vis;
    out->retraces_remaining = tooie_device_remaining_retraces;
    out->vi_current = events_context.vi.cur_state;
    out->vi_field = events_context.vi.field;
    for (int i = 0; i < 2; ++i) {
        const auto& source = events_context.vi.states[i];
        auto& target = out->vi_states[i];
        if (!mode_address(source.mode, events_context.rdram,
                          target.mode_guest_address)) return false;
        target.framebuffer = source.framebuffer;
        target.retrace = {source.mq, source.msg};
        target.state = source.state;
        target.control = source.control;
        target.retrace_count = source.retrace_count;
    }
    static_assert(sizeof(ultramodern::renderer::ViRegs) == 14 * sizeof(std::uint32_t));
    std::memcpy(out->vi_regs.data(), &events_context.vi.regs,
        sizeof(events_context.vi.regs));
    std::memcpy(out->next_screen_regs.data(), &events_context.vi.update_screen_regs,
        sizeof(events_context.vi.update_screen_regs));
    out->sp = {events_context.sp.mq, events_context.sp.msg};
    out->dp = {events_context.dp.mq, events_context.dp.msg};
    out->ai = {events_context.ai.mq, events_context.ai.msg};
    out->si = {events_context.si.mq, events_context.si.msg};
    return true;
}

extern "C" bool tooie_persistent_events_import(
    const tooie::persistent_state::devices::Snapshot* in) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !in || !events_context.rdram ||
        !valid_snapshot(*in) || events_context.action_queue.size_approx() != 0 ||
        events_context.sp_task_queue.size_approx() != 0) return false;
    std::lock_guard lock{events_context.message_mutex};
    total_vis = in->total_vis;
    tooie_device_remaining_retraces = in->retraces_remaining;
    events_context.vi.cur_state = in->vi_current;
    events_context.vi.field = in->vi_field;
    for (int i = 0; i < 2; ++i) {
        const auto& source = in->vi_states[i];
        auto& target = events_context.vi.states[i];
        target.mode = source.mode_guest_address == 0 ? &dummy_mode :
            reinterpret_cast<const OSViMode*>(events_context.rdram +
                (source.mode_guest_address - 0x80000000U));
        target.framebuffer = source.framebuffer;
        target.mq = source.retrace.queue;
        target.msg = source.retrace.message;
        target.state = source.state;
        target.control = source.control;
        target.retrace_count = source.retrace_count;
    }
    std::memcpy(&events_context.vi.regs, in->vi_regs.data(),
        sizeof(events_context.vi.regs));
    std::memcpy(&events_context.vi.update_screen_regs, in->next_screen_regs.data(),
        sizeof(events_context.vi.update_screen_regs));
    events_context.sp.mq = in->sp.queue;
    events_context.sp.msg = in->sp.message;
    events_context.dp.mq = in->dp.queue;
    events_context.dp.msg = in->dp.message;
    events_context.ai.mq = in->ai.queue;
    events_context.ai.msg = in->ai.message;
    events_context.si.mq = in->si.queue;
    events_context.si.msg = in->si.message;
    return true;
}

extern "C" bool tooie_persistent_events_work_empty() noexcept {
    return events_context.action_queue.size_approx() == 0 &&
        events_context.sp_task_queue.size_approx() == 0;
}

// A transient owner-thread command is not part of the disk snapshot. The
// graphics worker alone owns RendererContext and re-parks before return.
extern "C" bool tooie_persistent_events_renderer_action(
    std::uint64_t epoch,
    bool (*callback)(ultramodern::renderer::RendererContext*, std::uint64_t) noexcept,
    std::chrono::milliseconds timeout) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !callback || timeout.count() <= 0 ||
        events_context.action_queue.size_approx() != 0) return false;
    static std::mutex action_mutex;
    std::lock_guard action_lock(action_mutex);
    std::shared_ptr<PersistentRendererCompletion> completion;
    try {
        completion = std::make_shared<PersistentRendererCompletion>();
        if (!events_context.action_queue.enqueue(
                PersistentRendererAction{epoch, callback, completion})) return false;
    } catch (...) { return false; }
    if (!permit_owner_action(Worker::Graphics, epoch)) {
        completion->cancelled.store(true);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(completion->mutex);
    if (!completion->cv.wait_until(lock, deadline, [&] { return completion->done; })) {
        completion->cancelled.store(true);
        return false;
    }
    const bool success = completion->success;
    lock.unlock();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    return success && wait_worker_parked(Worker::Graphics, epoch,
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}
'''
    return text


def timer(text: str) -> str:
    owner = "timer"
    if "tooie_persistent_timer_export" in text:
        raise ValueError("timer: device hooks already installed")
    text = '#include "persistent_state_devices.hpp"\n#include <algorithm>\n' + text
    comparator = '''    // Lambda comparator function to keep the set ordered
    auto timer_sort = [PASS_RDRAM1](PTR(OSTimer) a_, PTR(OSTimer) b_) {
        OSTimer* a = TO_PTR(OSTimer, a_);
        OSTimer* b = TO_PTR(OSTimer, b_);

        // Order by timestamp if the timers have different timestamps
        if (a->timestamp != b->timestamp) {
            return a->timestamp < b->timestamp;
        }

        // If they have the exact same timestamp then order by address instead
        return a < b;
    };

    // Ordered set of timers that are currently active
    std::set<PTR(OSTimer), decltype(timer_sort)> active_timers{timer_sort};'''
    text = once(text, comparator,
        '''    // The timer worker is the sole mutator; a parked checkpoint may
    // inspect or rebuild this set without persisting a native iterator.
    auto& active_timers = tooie_device_active_timers;''', owner)
    text = once(text, "void timer_thread(RDRAM_ARG1) {",
        '''static std::uint8_t* tooie_device_timer_rdram = nullptr;
struct TooieDeviceTimerOrder {
    bool operator()(PTR(OSTimer) left, PTR(OSTimer) right) const noexcept {
        if (!tooie_device_timer_rdram) return left < right;
        const auto* a = reinterpret_cast<const OSTimer*>(tooie_device_timer_rdram +
            (static_cast<std::uint32_t>(left) & 0x7FFFFFFFU));
        const auto* b = reinterpret_cast<const OSTimer*>(tooie_device_timer_rdram +
            (static_cast<std::uint32_t>(right) & 0x7FFFFFFFU));
        return a->timestamp == b->timestamp ? left < right : a->timestamp < b->timestamp;
    }
};
static std::set<PTR(OSTimer), TooieDeviceTimerOrder> tooie_device_active_timers{};

void timer_thread(RDRAM_ARG1) {
    tooie_device_timer_rdram = rdram;''', owner)
    text = once(text, "        while (timer_context.action_queue.try_dequeue(cur_action)) {\n"
        "            process_timer_action(cur_action);\n        }",
        "        while (timer_context.action_queue.try_dequeue(cur_action)) {\n"
        "            process_timer_action(cur_action);\n        }\n"
        "        if (timer_context.action_queue.size_approx() == 0 &&\n"
        "            !tooie::persistent_state::devices::worker_checkpoint("
        "tooie::persistent_state::devices::Worker::Timer)) return;", owner)
    text = once(text, "    while (!stop_requested) {\n        // Empty the action queue",
        "    while (!stop_requested) {\n"
        "        if (tooie::persistent_state::devices::terminal_abort_requested()) return;\n"
        "        // Empty the action queue", owner)
    text = once(text, "            timer_context.action_queue.wait_dequeue(cur_action);\n"
        "            process_timer_action(cur_action);",
        '''            if (tooie::persistent_state::devices::experiment_enabled()) {
                if (!timer_context.action_queue.wait_dequeue_timed(
                        cur_action, std::chrono::milliseconds{2})) {
                    if (!tooie::persistent_state::devices::worker_checkpoint(
                            tooie::persistent_state::devices::Worker::Timer)) return;
                    continue;
                }
            } else timer_context.action_queue.wait_dequeue(cur_action);
            process_timer_action(cur_action);''', owner)
    text = once(text,
        "        auto wait_duration = ticks_to_timepoint(cur_timer->timestamp) - std::chrono::steady_clock::now();",
        '''        auto wait_duration = ticks_to_timepoint(cur_timer->timestamp) - std::chrono::steady_clock::now();
        if (tooie::persistent_state::devices::experiment_enabled())
            wait_duration = std::min(wait_duration,
                std::chrono::duration_cast<decltype(wait_duration)>(std::chrono::milliseconds{2}));''', owner)
    text = once(text,
        "        else {\n            // Waiting for the timer completed, so send the timer's message to its message queue",
        '''        else {
            if (tooie::persistent_state::devices::terminal_abort_requested()) return;
            if (tooie::persistent_state::devices::experiment_enabled() &&
                ticks_to_timepoint(cur_timer->timestamp) > std::chrono::steady_clock::now()) {
                active_timers.insert(cur_timer_);
                continue;
            }
            // Waiting for the timer completed, so send the timer's message to its message queue''', owner)
    text += r'''

extern "C" bool tooie_persistent_timer_export(
    tooie::persistent_state::devices::Snapshot* out) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !out || !tooie_device_timer_rdram ||
        timer_context.action_queue.size_approx() != 0) return false;
    try {
        const auto now = time_now();
        out->virtual_ticks = now;
        out->os_time_offset = ostime_offset;
        out->timers.clear();
        out->timers.reserve(tooie_device_active_timers.size());
        for (const auto address : tooie_device_active_timers) {
            const auto* source = reinterpret_cast<const OSTimer*>(
                tooie_device_timer_rdram +
                (static_cast<std::uint32_t>(address) & 0x7FFFFFFFU));
            out->timers.push_back(Timer{address,
                source->timestamp > now ? source->timestamp - now : 0,
                source->interval, {source->mq, source->msg}});
        }
        return true;
    } catch (...) { return false; }
}

extern "C" bool tooie_persistent_timer_import(
    const tooie::persistent_state::devices::Snapshot* in) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !in || !tooie_device_timer_rdram ||
        !valid_snapshot(*in) || timer_context.action_queue.size_approx() != 0)
        return false;
    try {
        const auto now = time_now();
        tooie_device_active_timers.clear();
        for (const auto& item : in->timers) {
            auto* target = reinterpret_cast<OSTimer*>(tooie_device_timer_rdram +
                (static_cast<std::uint32_t>(item.guest_address) & 0x7FFFFFFFU));
            target->timestamp = rebase_timer_deadline(now, item.remaining_ticks);
            target->interval = item.interval_ticks;
            target->mq = item.route.queue;
            target->msg = item.route.message;
            tooie_device_active_timers.insert(item.guest_address);
        }
        // clock_restore has already reinstated the captured virtual tick.
        // Keep the guest OS-time offset rather than deriving it via signed
        // subtraction (which can overflow near the counter limit).
        ostime_offset = in->os_time_offset;
        return true;
    } catch (...) { return false; }
}
'''
    return text


def pi(text: str) -> str:
    owner = "pi"
    if "tooie_persistent_pi_export" in text:
        raise ValueError("pi: device hooks already installed")
    text = '#include "persistent_state_devices.hpp"\n' + text
    text = once(text, "void update_save_file() {\n    bool saving_failed = false;",
        '''void update_save_file() {
    // The opt-in experiment never writes the ordinary progress-save path.
    // EEPROM lives only in the private in-memory state session.
    if (tooie::persistent_state::devices::experiment_enabled()) {
        // A private in-memory write still completes the continuous runtime's
        // persistence receipt; it never opens the player's progress file.
        tooie::save_persistence::acknowledge(
            tooie::save_persistence::generation(), true);
        return;
    }
    bool saving_failed = false;''', owner)
    text = once(text, "void read_save_file() {\n",
        '''void read_save_file() {
    if (tooie::persistent_state::devices::experiment_enabled()) {
        std::lock_guard lock{save_context.save_buffer_mutex};
        tooie::persistent_state::devices::seed_private_eeprom(
            ultramodern::get_save_file_path(), save_context.save_buffer);
        return;
    }
''', owner)
    text = once(text, "void ultramodern::change_save_file(const std::u8string& subfolder, const std::u8string& name) {",
        '''void ultramodern::change_save_file(const std::u8string& subfolder, const std::u8string& name) {
    if (tooie::persistent_state::devices::experiment_enabled()) return;''', owner)
    text = once(text, "    while (!exited) {\n        bool save_buffer_updated = false;",
        '''    while (!exited) {
        if (!tooie::persistent_state::devices::worker_checkpoint(
                tooie::persistent_state::devices::Worker::PrivateSave)) break;
        bool save_buffer_updated = false;''', owner)
    text = once(text,
        "        constexpr int64_t wait_time_microseconds = 10000;\n"
        "        constexpr int max_actions = 128;",
        "        const int64_t wait_time_microseconds = "
        "tooie::persistent_state::devices::experiment_enabled() ? 2000 : 10000;\n"
        "        const int max_actions = "
        "tooie::persistent_state::devices::experiment_enabled() ? 1 : 128;", owner)
    text = once(text, "        if (save_buffer_updated) {",
        "        if (tooie::persistent_state::devices::terminal_abort_requested()) break;\n"
        "        if (save_buffer_updated) {", owner)
    text += r'''

extern "C" bool tooie_persistent_pi_export(
    tooie::persistent_state::devices::Snapshot* out) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !out) return false;
    const auto type = recomp::get_save_type();
    if (type != recomp::SaveType::Eep4k && type != recomp::SaveType::Eep16k)
        return false;
    std::lock_guard lock{save_context.save_buffer_mutex};
    const auto expected = type == recomp::SaveType::Eep4k ? 0x200U : 0x800U;
    if (save_context.save_buffer.size() != expected) return false;
    try {
        out->medium = type == recomp::SaveType::Eep4k ?
            SaveMedium::Eeprom4K : SaveMedium::Eeprom16K;
        out->private_eeprom.assign(save_context.save_buffer.begin(),
            save_context.save_buffer.end());
        return true;
    } catch (...) { return false; }
}

extern "C" bool tooie_persistent_pi_import(
    const tooie::persistent_state::devices::Snapshot* in) noexcept {
    using namespace tooie::persistent_state::devices;
    if (!experiment_enabled() || !in || !valid_snapshot(*in)) return false;
    const auto type = recomp::get_save_type();
    if ((type == recomp::SaveType::Eep4k && in->medium != SaveMedium::Eeprom4K) ||
        (type == recomp::SaveType::Eep16k && in->medium != SaveMedium::Eeprom16K) ||
        (type != recomp::SaveType::Eep4k && type != recomp::SaveType::Eep16k))
        return false;
    std::lock_guard lock{save_context.save_buffer_mutex};
    if (save_context.save_buffer.size() != in->private_eeprom.size()) return false;
    std::copy(in->private_eeprom.begin(), in->private_eeprom.end(),
        save_context.save_buffer.begin());
    return true;
}
'''
    return text


def transform(name: str, text: str) -> str:
    if name == "events":
        return events(text)
    if name == "timer":
        return timer(text)
    if name == "pi":
        return pi(text)
    raise ValueError(f"Unsupported device source: {name}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-directory", required=True, type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args()
    source_dir = args.input_directory.resolve(strict=True)
    output_dir = args.output_directory.resolve(strict=True)
    if source_dir == output_dir or source_dir in output_dir.parents or (
            output_dir in source_dir.parents and output_dir != source_dir.parent):
        raise ValueError("Device output must be outside prepared runtime input")
    outputs = {}
    for name in ("events", "timer", "pi"):
        source = source_dir / f"{name}.cpp"
        original = source.read_text()
        rendered = transform(name, original)
        outputs[name] = (rendered,
            hashlib.sha256(original.encode()).hexdigest(),
            hashlib.sha256(rendered.encode()).hexdigest())
    for name, (rendered, _, _) in outputs.items():
        (output_dir / f"persistent_{name}.cpp").write_text(rendered)
    (output_dir / "persistent_devices_manifest.json").write_text(json.dumps({
        name: {"input_sha256": before, "output_sha256": after}
        for name, (_, before, after) in outputs.items()
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
