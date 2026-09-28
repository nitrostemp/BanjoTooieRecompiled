#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

// Experimental native device checkpoint contract. This is deliberately not a
// player-facing save-state API. The normal runtime does not opt in to it.
namespace tooie::persistent_state::devices {

constexpr std::uint32_t snapshot_version = 1;

enum class SaveMedium : std::uint8_t { Eeprom4K = 1, Eeprom16K = 2 };
enum class Status : std::uint8_t {
    Ready,
    Disabled,
    IncompleteHooks,
    InvalidSnapshot,
    ProducerTimeout,
    WorkInFlight,
    // External FIFO was not frozen/exportable, exceeded its bound, or failed
    // validation; ordinary pending messages are allowed in the snapshot.
    ExternalMessagesPending,
    RendererUnsupported,
    AudioUnsupported,
    ClockUnsupported,
    ExportFailed,
    ImportFailed,
    FatalParked,
};

enum class Worker : std::uint8_t { Vi, Graphics, Sp, Timer, PrivateSave };

struct EventRoute {
    std::int32_t queue = 0;
    std::int32_t message = 0;
};

struct ViState {
    // A zero mode address represents the runtime-owned dummy mode. All other
    // addresses are guest offsets from 0x80000000, never native pointers.
    std::uint32_t mode_guest_address = 0;
    std::int32_t framebuffer = 0;
    EventRoute retrace{};
    std::uint32_t state = 0;
    std::uint32_t control = 0;
    std::int32_t retrace_count = 1;
};

struct Timer {
    std::int32_t guest_address = 0;
    std::uint64_t remaining_ticks = 0;
    std::uint64_t interval_ticks = 0;
    EventRoute route{};
};

struct AudioState {
    std::uint32_t frequency = 0;
    std::uint32_t queued_stereo_samples = 0;
    bool rebuffering = false;
    // Exact queued PCM from a host-owned shadow ledger, not merely SDL's
    // queued byte count. Empty only when frequency/samples are both zero.
    std::vector<std::int16_t> queued_pcm{};
};

struct Snapshot {
    std::uint32_t version = snapshot_version;
    std::uint64_t producer_epoch = 0;
    std::uint64_t virtual_ticks = 0;
    std::int64_t os_time_offset = 0;
    std::uint64_t total_vis = 0;
    std::int32_t retraces_remaining = 1;
    std::int32_t vi_current = 0;
    std::int32_t vi_field = 0;
    std::array<ViState, 2> vi_states{};
    std::array<std::uint32_t, 14> vi_regs{};
    std::array<std::uint32_t, 14> next_screen_regs{};
    EventRoute sp{};
    EventRoute dp{};
    EventRoute ai{};
    EventRoute si{};
    std::vector<Timer> timers{};
    SaveMedium medium = SaveMedium::Eeprom16K;
    std::vector<std::uint8_t> private_eeprom{};
    AudioState audio{};
    // Pointer-free renderer/RDP state only. Pending commands or triangles must
    // cause renderer_export to refuse, never be serialized as host pointers.
    std::vector<std::uint8_t> renderer_blob{};
};

// Runtime-private hooks are installed only by the opt-in continuation target.
// freeze must park VI, graphics, SP, timer and private-save producers at safe
// points and return only once all five have acknowledged. An acquired lease
// must be released by resume even when capture or restore refuses. Each
// quiesce hook that returns false must release its own partial acquisition;
// successful acquisitions are released by this coordinator on every ordinary
// return path. Callers park all guest continuations before entering capture or
// restore and keep them parked until the same-lease guest visitor returns.
struct Hooks {
    void* guest_context = nullptr; // Native-only for this transaction, never serialized.
    bool (*capture_guest)(void*, std::uint64_t) noexcept = nullptr;
    bool (*restore_guest)(void*, std::uint64_t) noexcept = nullptr;
    // Freeze virtual time before parking device workers, so timer deadlines
    // and VI phase do not advance during a transaction.
    bool (*clock_quiesce)(std::uint64_t, std::chrono::milliseconds) noexcept = nullptr;
    void (*clock_resume)(std::uint64_t) noexcept = nullptr;
    bool (*clock_restore)(std::uint64_t, std::uint64_t) noexcept = nullptr;
    bool (*freeze)(std::uint64_t, std::chrono::milliseconds) noexcept = nullptr;
    void (*resume)(std::uint64_t) noexcept = nullptr;
    bool (*export_state)(Snapshot&) noexcept = nullptr;
    bool (*import_state)(const Snapshot&) noexcept = nullptr;
    bool (*work_queues_empty)() noexcept = nullptr;
    // True when the external FIFO is frozen, bounded and exportable; pending
    // messages themselves are serialized by the guest snapshot visitor.
    bool (*external_queue_ready)() noexcept = nullptr;
    bool (*renderer_quiesce)(std::uint64_t, std::chrono::milliseconds) noexcept = nullptr;
    void (*renderer_resume)(std::uint64_t) noexcept = nullptr;
    bool (*audio_quiesce)(std::uint64_t, std::chrono::milliseconds) noexcept = nullptr;
    void (*audio_resume)(std::uint64_t) noexcept = nullptr;
    bool (*audio_export)(AudioState&, std::uint64_t) noexcept = nullptr;
    bool (*renderer_export)(std::vector<std::uint8_t>&, std::uint64_t) noexcept = nullptr;
    // Pure schema check; no renderer mutation and safe before acquiring a
    // whole-machine lease. Refuses unknown RDP versions and native pointers.
    bool (*renderer_validate)(const std::vector<std::uint8_t>&) noexcept = nullptr;
    bool (*renderer_restore_epoch)(const std::vector<std::uint8_t>&,
        std::uint64_t) noexcept = nullptr;
    bool (*audio_restore_epoch)(const AudioState&, std::uint64_t) noexcept = nullptr;
    // Called only after an import has possibly mutated device/guest state and
    // renderer/audio rebase fails. Must force experimental shutdown; leases
    // intentionally remain parked, never resume a mixed epoch.
    void (*fatal_restore_failure)(std::uint64_t) noexcept = nullptr;
};

// Set before init_saving, init_timers and events threads are started. Toggling
// this after boot could read a personal save before private-medium isolation.
void enable_experiment(bool enabled) noexcept;
bool experiment_enabled() noexcept;
// Called once by experimental init_saving before its private save worker starts.
// Reads the active profile's primary EEPROM or backup without creating files
// or directories. Both absent means a blank private medium; present malformed
// files refuse startup. Normal sessions never call this helper.
void seed_private_eeprom(const std::filesystem::path& active_save_file,
    std::vector<char>& private_buffer);
bool terminal_abort_requested() noexcept;
// Called only at post-work safe points by the five prepared runtime workers.
// freeze_workers is reversible; a timed-out request releases all parked workers.
bool worker_checkpoint(Worker worker) noexcept;
bool freeze_workers(std::uint64_t epoch,
    std::chrono::milliseconds timeout) noexcept;
void resume_workers(std::uint64_t epoch) noexcept;
// Fatal restore shutdown only: wake parked workers so each exits before its
// next work step. Does not restore a mixed guest/device epoch.
void terminal_abort(std::uint64_t epoch) noexcept;
bool permit_owner_action(Worker worker, std::uint64_t epoch) noexcept;
bool wait_worker_parked(Worker worker, std::uint64_t epoch,
    std::chrono::milliseconds timeout) noexcept;
std::uint64_t producer_epoch() noexcept;
bool valid_snapshot(const Snapshot& snapshot) noexcept;
std::uint64_t rebase_timer_deadline(std::uint64_t now,
    std::uint64_t remaining) noexcept;
Status capture(const Hooks& hooks, Snapshot& output,
    std::chrono::milliseconds timeout) noexcept;
Status restore(const Hooks& hooks, const Snapshot& input,
    std::chrono::milliseconds timeout) noexcept;

} // namespace tooie::persistent_state::devices
