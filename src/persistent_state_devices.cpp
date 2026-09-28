#include "persistent_state_devices.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

extern "C" void tooie_persistent_events_graphics_diagnostic(
    std::uint32_t* phase, std::uint64_t* queue_depth,
    std::uint32_t* dp_status) noexcept;

namespace {
using namespace tooie::persistent_state::devices;

std::atomic_bool enabled{false};
std::atomic_uint64_t next_epoch{1};
std::atomic_flag transaction_active = ATOMIC_FLAG_INIT;
std::mutex worker_mutex;
std::condition_variable worker_cv;
bool freeze_requested = false;
bool abort_requested = false;
std::uint64_t frozen_epoch = 0;
std::uint32_t worker_seen = 0;
std::uint32_t worker_parked = 0;
std::uint32_t owner_permits = 0;
constexpr std::uint32_t all_workers = (1U << 5U) - 1U;
constexpr std::uint32_t vi_worker_bit =
    std::uint32_t{1} << static_cast<unsigned>(Worker::Vi);
constexpr std::uint32_t gfx_worker_bit =
    std::uint32_t{1} << static_cast<unsigned>(Worker::Graphics);
std::atomic_uint64_t active_producer_epoch{0};

bool workers_parked(std::uint64_t epoch) noexcept {
    std::lock_guard lock(worker_mutex);
    return freeze_requested && frozen_epoch == epoch &&
        worker_parked == all_workers && owner_permits == 0;
}

bool complete_hooks(const Hooks& hooks) noexcept {
    return hooks.guest_context && hooks.capture_guest && hooks.restore_guest &&
        hooks.clock_quiesce && hooks.clock_resume && hooks.clock_restore &&
        hooks.freeze && hooks.resume && hooks.export_state &&
        hooks.import_state && hooks.work_queues_empty &&
        hooks.external_queue_ready && hooks.renderer_quiesce &&
        hooks.renderer_resume && hooks.audio_quiesce && hooks.audio_resume &&
        hooks.audio_export && hooks.renderer_export && hooks.renderer_validate &&
        hooks.renderer_restore_epoch &&
        hooks.audio_restore_epoch && hooks.fatal_restore_failure;
}

class Transaction final {
public:
    explicit Transaction(const Hooks& hooks) noexcept : hooks_(hooks) {
        acquired_ = !transaction_active.test_and_set(std::memory_order_acquire);
    }
    ~Transaction() {
        if (fatal_) return; // A mixed epoch may only be torn down, not resumed.
        if (audio_) hooks_.audio_resume(epoch_);
        if (renderer_) hooks_.renderer_resume(epoch_);
        if (clock_) hooks_.clock_resume(epoch_);
        if (frozen_) hooks_.resume(epoch_);
        if (acquired_) transaction_active.clear(std::memory_order_release);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    bool acquired() const noexcept { return acquired_; }
    std::uint64_t epoch() const noexcept { return epoch_; }
    void set_epoch(std::uint64_t value) noexcept { epoch_ = value; }
    void frozen() noexcept { frozen_ = true; }
    void clock() noexcept { clock_ = true; }
    void renderer() noexcept { renderer_ = true; }
    void audio() noexcept { audio_ = true; }
    void fatal() noexcept { fatal_ = true; }

private:
    const Hooks& hooks_;
    std::uint64_t epoch_ = 0;
    bool acquired_ = false;
    bool frozen_ = false;
    bool clock_ = false;
    bool renderer_ = false;
    bool audio_ = false;
    bool fatal_ = false;
};

Status reject_or_park(const Hooks& hooks, Transaction& transaction,
    Status ordinary_refusal) noexcept {
    // A timed-out owner-thread action may already be executing. Releasing
    // producers then would race a renderer mutation against live guest work.
    if (workers_parked(transaction.epoch())) return ordinary_refusal;
    transaction.fatal();
    hooks.fatal_restore_failure(transaction.epoch());
    return Status::FatalParked;
}

std::chrono::milliseconds remaining_until(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::max(std::chrono::milliseconds{1},
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

Status quiesce(const Hooks& hooks, Transaction& transaction,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (!hooks.work_queues_empty())
        return reject_or_park(hooks, transaction, Status::WorkInFlight);
    if (!hooks.external_queue_ready())
        return reject_or_park(hooks, transaction, Status::ExternalMessagesPending);
    auto remaining = remaining_until(deadline);
    if (remaining == std::chrono::milliseconds::zero() ||
        !hooks.renderer_quiesce(transaction.epoch(), remaining))
        return reject_or_park(hooks, transaction, Status::RendererUnsupported);
    transaction.renderer();
    remaining = remaining_until(deadline);
    if (remaining == std::chrono::milliseconds::zero() ||
        !hooks.audio_quiesce(transaction.epoch(), remaining))
        return reject_or_park(hooks, transaction, Status::AudioUnsupported);
    transaction.audio();
    if (!hooks.work_queues_empty())
        return reject_or_park(hooks, transaction, Status::WorkInFlight);
    if (!hooks.external_queue_ready())
        return reject_or_park(hooks, transaction, Status::ExternalMessagesPending);
    if (!workers_parked(transaction.epoch()))
        return reject_or_park(hooks, transaction, Status::ProducerTimeout);
    return Status::Ready;
}
}

namespace tooie::persistent_state::devices {

void enable_experiment(bool value) noexcept {
    if (value && !enabled.load(std::memory_order_acquire)) {
        // Opt in once before producer startup. A fatal transaction remains
        // terminal within the process and cannot be toggled back to live.
        std::lock_guard lock(worker_mutex);
        if (!abort_requested) {
            worker_seen = 0;
            worker_parked = 0;
            owner_permits = 0;
        }
    }
    enabled.store(value, std::memory_order_release);
}

bool experiment_enabled() noexcept {
    return enabled.load(std::memory_order_acquire);
}

void seed_private_eeprom(const std::filesystem::path& active_save_file,
    std::vector<char>& private_buffer) {
    if (!experiment_enabled() ||
        (private_buffer.size() != 0x200U && private_buffer.size() != 0x800U))
        throw std::runtime_error("Persistent private EEPROM seed requires an active supported session");

    enum class ReadResult { Missing, Invalid, Valid };
    auto read_exact = [&](const std::filesystem::path& path) {
        std::error_code error;
        const bool present = std::filesystem::exists(path, error);
        if (error) return ReadResult::Invalid;
        if (!present) return ReadResult::Missing;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            std::filesystem::file_size(path, error) != private_buffer.size() || error)
            return ReadResult::Invalid;
        std::ifstream input(path, std::ios::binary);
        if (!input) return ReadResult::Invalid;
        std::vector<char> bytes(private_buffer.size());
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            return ReadResult::Invalid;
        char extra = 0;
        if (input.get(extra) || !input.eof()) return ReadResult::Invalid;
        private_buffer.swap(bytes);
        return ReadResult::Valid;
    };

    const auto primary = read_exact(active_save_file);
    if (primary == ReadResult::Valid) {
        std::fprintf(stderr, "Persistent private EEPROM seeded read-only from active profile primary; normal progress will not be written\n");
        return;
    }
    auto backup_path = active_save_file;
    backup_path += u8".bak";
    const auto backup = read_exact(backup_path);
    if (backup == ReadResult::Valid) {
        std::fprintf(stderr, "Persistent private EEPROM seeded read-only from active profile backup after %s primary; normal progress will not be written\n",
            primary == ReadResult::Missing ? "missing" : "invalid");
        return;
    }
    if (primary != ReadResult::Missing || backup != ReadResult::Missing)
        throw std::runtime_error("Persistent private EEPROM seed refused: active profile primary and backup have no valid exact-size image");
    std::fill(private_buffer.begin(), private_buffer.end(), 0);
    std::fprintf(stderr, "Persistent private EEPROM initialized blank (no active profile primary or backup); normal progress will not be written\n");
}

bool terminal_abort_requested() noexcept {
    std::lock_guard lock(worker_mutex);
    return abort_requested;
}

bool worker_checkpoint(Worker worker) noexcept {
    if (!experiment_enabled()) {
        std::lock_guard lock(worker_mutex);
        return !abort_requested;
    }
    const auto bit = std::uint32_t{1} << static_cast<unsigned>(worker);
    std::unique_lock lock(worker_mutex);
    if (abort_requested) return false;
    worker_seen |= bit;
    if (!freeze_requested) return true;
    const bool consumer = worker == Worker::Sp || worker == Worker::Graphics;
    // VI can enqueue a final screen update while entering its checkpoint.
    // Keep consumers running until that producer is parked, then their
    // queue-empty checkpoints can drain its final work before they park.
    if (consumer && (worker_parked & vi_worker_bit) == 0) return true;
    const auto epoch = frozen_epoch;
    worker_parked |= bit;
    worker_cv.notify_all();
    worker_cv.wait(lock, [&] {
        return abort_requested || !freeze_requested || frozen_epoch != epoch ||
            (owner_permits & bit) != 0;
    });
    owner_permits &= ~bit;
    worker_parked &= ~bit;
    return !abort_requested;
}

bool freeze_workers(std::uint64_t epoch,
    std::chrono::milliseconds timeout) noexcept {
    if (!experiment_enabled() || epoch == 0 ||
        timeout <= std::chrono::milliseconds::zero()) return false;
    std::unique_lock lock(worker_mutex);
    if (freeze_requested || abort_requested) return false;
    frozen_epoch = epoch;
    freeze_requested = true;
    worker_parked = 0;
    owner_permits = 0;
    worker_cv.notify_all();
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    const auto ready = [&] {
        return worker_seen == all_workers && worker_parked == all_workers;
    };
    unsigned blocked_samples = 0;
    bool blocked_dp = false;
    auto first_blocked_sample = started;
    while (!ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        // A dequeued GFX task held at DP admission cannot finish while the
        // guest that must clear FREEZE is parked. Idle GFX plus FREEZE is fine.
        if (worker_seen == all_workers &&
            worker_parked == (all_workers & ~gfx_worker_bit) &&
            owner_permits == 0) {
            std::uint32_t phase = 0, dp_status = 0;
            tooie_persistent_events_graphics_diagnostic(&phase, nullptr, &dp_status);
            if ((phase == 3 || phase == 5) && (dp_status & 0x2U)) {
                if (blocked_samples == 0) first_blocked_sample = now;
                ++blocked_samples;
                if (blocked_samples >= 2 &&
                    now - first_blocked_sample >= std::chrono::milliseconds{2}) {
                    blocked_dp = true;
                    break;
                }
            } else blocked_samples = 0;
        } else blocked_samples = 0;
        worker_cv.wait_until(lock,
            std::min(deadline, now + std::chrono::milliseconds{2}));
    }
    if (!ready()) {
        const auto seen = worker_seen;
        const auto parked = worker_parked;
        const auto permits = owner_permits;
        std::uint32_t gfx_phase = 0, dp_status = 0;
        std::uint64_t gfx_queue = 0;
        tooie_persistent_events_graphics_diagnostic(&gfx_phase, &gfx_queue, &dp_status);
        freeze_requested = false;
        worker_cv.notify_all();
        lock.unlock();
        std::fprintf(stderr,
            "Persistent device freeze %s epoch=%llu elapsed_ms=%lld seen=0x%02X parked=0x%02X missing=0x%02X permits=0x%02X gfx_phase=%u gfx_queue=%llu dp_status=0x%08X (VI=01 GFX=02 SP=04 timer=08 save=10; gfx 1=loop 2=dequeue 3=DP-parse-wait 4=send-dl 5=DP-complete-wait 6=screen 7=config 8=owner 9=dummy)\n",
            blocked_dp ? "gfx_dp_freeze_wait" : "timeout",
            static_cast<unsigned long long>(epoch),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()),
            seen, parked, all_workers & ~parked, permits, gfx_phase,
            static_cast<unsigned long long>(gfx_queue), dp_status);
        return false;
    }
    active_producer_epoch.store(epoch, std::memory_order_release);
    return true;
}

void resume_workers(std::uint64_t epoch) noexcept {
    std::lock_guard lock(worker_mutex);
    if (!freeze_requested || frozen_epoch != epoch || abort_requested) return;
    freeze_requested = false;
    worker_cv.notify_all();
}

void terminal_abort(std::uint64_t epoch) noexcept {
    std::lock_guard lock(worker_mutex);
    if (!freeze_requested || frozen_epoch != epoch) return;
    abort_requested = true;
    worker_cv.notify_all();
}

bool permit_owner_action(Worker worker, std::uint64_t epoch) noexcept {
    if (worker != Worker::Graphics) return false;
    const auto bit = std::uint32_t{1} << static_cast<unsigned>(worker);
    std::lock_guard lock(worker_mutex);
    if (!freeze_requested || frozen_epoch != epoch ||
        (worker_parked & bit) == 0 || (owner_permits & bit) != 0)
        return false;
    owner_permits |= bit;
    worker_cv.notify_all();
    return true;
}

bool wait_worker_parked(Worker worker, std::uint64_t epoch,
    std::chrono::milliseconds timeout) noexcept {
    if (timeout <= std::chrono::milliseconds::zero()) return false;
    const auto bit = std::uint32_t{1} << static_cast<unsigned>(worker);
    std::unique_lock lock(worker_mutex);
    return worker_cv.wait_for(lock, timeout, [&] {
        return freeze_requested && frozen_epoch == epoch &&
            (worker_parked & bit) != 0 && (owner_permits & bit) == 0;
    });
}

std::uint64_t producer_epoch() noexcept {
    return active_producer_epoch.load(std::memory_order_acquire);
}

std::uint64_t rebase_timer_deadline(std::uint64_t now,
    std::uint64_t remaining) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return remaining > maximum - now ? maximum : now + remaining;
}

bool valid_snapshot(const Snapshot& snapshot) noexcept {
    if (snapshot.version != snapshot_version || snapshot.producer_epoch == 0 ||
        snapshot.vi_current < 0 || snapshot.vi_current > 1 ||
        snapshot.vi_field < 0 || snapshot.vi_field > 1 ||
        snapshot.retraces_remaining < 1) return false;
    // Match the virtual clock's microsecond lattice before any guest import;
    // otherwise clock_restore could fail only after RDRAM had been replaced.
    const auto seconds = snapshot.virtual_ticks / 46875000U;
    const auto remainder = snapshot.virtual_ticks % 46875000U;
    if (seconds > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() / 1000000000)) return false;
    const auto micros = seconds * 1000000U +
        (remainder * 1000U + 46874U) / 46875U;
    if (micros > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() / 1000)) return false;
    if ((micros / 1000U) * 46875U + (micros % 1000U) * 46875U / 1000U !=
        snapshot.virtual_ticks) return false;
    for (const auto& state : snapshot.vi_states) {
        if (state.retrace_count < 1 ||
            (state.mode_guest_address != 0 &&
             (state.mode_guest_address < 0x80000000U ||
              state.mode_guest_address >= 0x80800000U))) return false;
    }
    const auto eeprom_size = snapshot.medium == SaveMedium::Eeprom4K ? 0x200U
        : snapshot.medium == SaveMedium::Eeprom16K ? 0x800U : 0U;
    if (eeprom_size == 0 || snapshot.private_eeprom.size() != eeprom_size ||
        snapshot.renderer_blob.empty() ||
        snapshot.renderer_blob.size() > 256U * 1024U ||
        snapshot.timers.size() > 4096U) return false;
    const auto& audio = snapshot.audio;
    if (audio.frequency == 0) {
        if (audio.queued_stereo_samples != 0 || !audio.queued_pcm.empty()) return false;
    } else if (audio.frequency < 1000U || audio.frequency > 192000U ||
        audio.queued_stereo_samples > audio.frequency * 2U ||
        audio.queued_pcm.size() !=
            static_cast<std::size_t>(audio.queued_stereo_samples) * 2U) {
        return false;
    }
    for (std::size_t i = 0; i < snapshot.timers.size(); ++i) {
        const auto& timer = snapshot.timers[i];
        const auto address = static_cast<std::uint32_t>(timer.guest_address);
        if (address < 0x80000000U || address >= 0x80800000U) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (snapshot.timers[j].guest_address == timer.guest_address) return false;
    }
    return true;
}

Status capture(const Hooks& hooks, Snapshot& output,
    std::chrono::milliseconds timeout) noexcept {
    if (!experiment_enabled()) return Status::Disabled;
    if (!complete_hooks(hooks)) return Status::IncompleteHooks;
    if (timeout <= std::chrono::milliseconds::zero()) return Status::ProducerTimeout;
    Transaction transaction{hooks};
    if (!transaction.acquired()) return Status::ProducerTimeout;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    transaction.set_epoch(next_epoch.fetch_add(1, std::memory_order_acq_rel));
    if (!hooks.clock_quiesce(transaction.epoch(), remaining_until(deadline)))
        return Status::ClockUnsupported;
    transaction.clock();
    if (!hooks.freeze(transaction.epoch(), remaining_until(deadline)))
        return Status::ProducerTimeout;
    transaction.frozen();
    if (const auto status = quiesce(hooks, transaction, deadline);
        status != Status::Ready) return status;
    Snapshot candidate{};
    if (!hooks.export_state(candidate))
        return reject_or_park(hooks, transaction, Status::ExportFailed);
    if (!hooks.audio_export(candidate.audio, transaction.epoch()))
        return reject_or_park(hooks, transaction, Status::AudioUnsupported);
    if (!hooks.renderer_export(candidate.renderer_blob, transaction.epoch()))
        return reject_or_park(hooks, transaction, Status::RendererUnsupported);
    if (!workers_parked(transaction.epoch()))
        return reject_or_park(hooks, transaction, Status::ProducerTimeout);
    candidate.version = snapshot_version;
    candidate.producer_epoch = transaction.epoch();
    if (!valid_snapshot(candidate) || !hooks.renderer_validate(candidate.renderer_blob))
        return reject_or_park(hooks, transaction, Status::InvalidSnapshot);
    if (!hooks.capture_guest(hooks.guest_context, transaction.epoch()))
        return reject_or_park(hooks, transaction, Status::ExportFailed);
    output = std::move(candidate);
    return Status::Ready;
}

Status restore(const Hooks& hooks, const Snapshot& input,
    std::chrono::milliseconds timeout) noexcept {
    if (!experiment_enabled()) return Status::Disabled;
    if (!valid_snapshot(input)) return Status::InvalidSnapshot;
    if (!complete_hooks(hooks)) return Status::IncompleteHooks;
    if (!hooks.renderer_validate(input.renderer_blob)) return Status::InvalidSnapshot;
    if (timeout <= std::chrono::milliseconds::zero()) return Status::ProducerTimeout;
    Transaction transaction{hooks};
    if (!transaction.acquired()) return Status::ProducerTimeout;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    transaction.set_epoch(next_epoch.fetch_add(1, std::memory_order_acq_rel));
    if (!hooks.clock_quiesce(transaction.epoch(), remaining_until(deadline)))
        return Status::ClockUnsupported;
    transaction.clock();
    if (!hooks.freeze(transaction.epoch(), remaining_until(deadline)))
        return Status::ProducerTimeout;
    transaction.frozen();
    if (const auto status = quiesce(hooks, transaction, deadline);
        status != Status::Ready) return status;
    // Guest RDRAM/scheduler and devices share this one lease. Renderer and
    // audio invalidation happen *after* the new VI/EEPROM/timer state exists.
    // A failure beyond this point cannot safely resume a mixed guest epoch.
    try {
        Snapshot rebased = input;
        rebased.producer_epoch = transaction.epoch();
        if (!hooks.restore_guest(hooks.guest_context, transaction.epoch()) ||
            !hooks.clock_restore(input.virtual_ticks, transaction.epoch()) ||
            !hooks.import_state(rebased) ||
            !hooks.renderer_restore_epoch(input.renderer_blob, transaction.epoch()) ||
            !hooks.audio_restore_epoch(input.audio, transaction.epoch())) {
            transaction.fatal();
            hooks.fatal_restore_failure(transaction.epoch());
            return Status::FatalParked;
        }
    } catch (...) {
        transaction.fatal();
        hooks.fatal_restore_failure(transaction.epoch());
        return Status::FatalParked;
    }
    return Status::Ready;
}

} // namespace tooie::persistent_state::devices
