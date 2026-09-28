#include "persistent_state_file.hpp"
#include "platform_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace tooie::persistent_state;
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t max_file = 64U * 1024U * 1024U;
constexpr std::size_t ram_size = 8U * 1024U * 1024U;
constexpr std::array<std::uint8_t, 8> magic{'T','O','O','I','E','S','T','1'};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("Persistent state: ") + message);
}
bool sha256(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void identity_valid(const Compatibility& c) {
    require(sha256(c.program_sha256) && sha256(c.executable_sha256) &&
        sha256(c.rom_sha256) && sha256(c.settings_sha256), "invalid compatibility digest");
    require(!c.runtime_revision.empty() && c.runtime_revision.size() <= 256,
        "invalid runtime identity");
}
void compatible(const Compatibility& a, const Compatibility& b) {
    identity_valid(a); identity_valid(b);
    require(a.program_sha256 == b.program_sha256, "generated program differs");
    require(a.executable_sha256 == b.executable_sha256, "executable differs");
    require(a.rom_sha256 == b.rom_sha256, "ROM differs");
    require(a.runtime_revision == b.runtime_revision, "runtime differs");
    require(a.settings_sha256 == b.settings_sha256, "guest settings differ");
}
void bounded(const Snapshot& s) {
    require(std::endian::native == std::endian::little, "unsupported guest-memory host endian");
    require(s.schema == snapshot_schema && s.save_medium_policy == isolated_eeprom_policy,
        "unsupported schema or save-medium policy");
    identity_valid(s.compatibility);
    require(s.epoch != 0 && s.rdram.size() == ram_size, "invalid epoch or RDRAM size");
    require(!s.threads.empty() && s.threads.size() <= 64, "thread count outside bounds");
    require(s.overlays.size() <= 4096 && s.overlay_generations.size() <= 4096,
        "overlay count outside bounds");
    require(s.external_messages.size() <= 4096, "external FIFO outside bounds");
    std::unordered_set<std::uint32_t> addresses;
    for (const auto& t : s.threads) {
        require(t.address >= 0x80000000U && t.address < 0x80800000U &&
            (t.address & 7U) == 0 && addresses.insert(t.address).second,
            "invalid or duplicate thread address");
        require(t.phase == ThreadPhase::SchedulerWait || t.phase == ThreadPhase::ExternalWait,
            "unknown thread phase");
        require(t.context.mips3_float_mode == 0, "unsupported FPR mode");
        require(t.cop1_rounding_mode <= 3, "invalid FPU rounding mode");
        require(!t.frames.empty() && t.frames.size() <= 1024 && t.overlay_callsites.size() <= 1024,
            "thread continuation depth outside bounds");
        for (const auto& f : t.frames) {
            require(f.function != 0 && f.extra_locals.size() <= 124,
                "invalid continuation frame");
        }
    }
    require(s.si.version == 1, "unsupported SI schema");
    require(devices::valid_snapshot(s.devices), "invalid device snapshot");
}

struct Writer {
    Bytes bytes;
    static constexpr bool reading = false;
    void append(std::span<const std::uint8_t> data) {
        require(data.size() <= max_file - 64U - bytes.size(), "file exceeds size limit");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    template<class T> void value(T input) {
        if constexpr (std::is_enum_v<T>) value(static_cast<std::underlying_type_t<T>>(input));
        else if constexpr (std::is_same_v<T, bool>) value(std::uint8_t(input ? 1 : 0));
        else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            const U bits = static_cast<U>(input);
            std::array<std::uint8_t, sizeof(T)> encoded{};
            for (std::size_t i = 0; i < sizeof(T); ++i) encoded[i] = std::uint8_t(bits >> (i * 8));
            append(encoded);
        }
    }
    template<class T> void count(const std::vector<T>& values, std::size_t limit) {
        require(values.size() <= limit, "vector exceeds schema bound");
        value(std::uint32_t(values.size()));
    }
    void blob(const Bytes& values, std::size_t limit) { count(values, limit); append(values); }
    void text(const std::string& text) {
        require(!text.empty() && text.size() <= 256, "identity length outside bounds");
        value(std::uint32_t(text.size()));
        append({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    static constexpr bool reading = true;
    std::span<const std::uint8_t> take(std::size_t count) {
        require(count <= bytes.size(), "truncated file");
        const auto result = bytes.first(count); bytes = bytes.subspan(count); return result;
    }
    template<class T> void value(T& output) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{}; value(raw); output = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t raw{}; value(raw); require(raw <= 1, "invalid boolean"); output = raw != 0;
        } else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            U raw = 0; const auto data = take(sizeof(T));
            for (std::size_t i = 0; i < sizeof(T); ++i) raw |= U(data[i]) << (i * 8);
            if constexpr (std::is_signed_v<T>) output = std::bit_cast<T>(raw);
            else output = raw;
        }
    }
    template<class T> void count(std::vector<T>& values, std::size_t limit) {
        std::uint32_t count{}; value(count);
        require(count <= limit && count <= bytes.size(), "vector count outside bounds");
        values.resize(count);
    }
    void blob(Bytes& values, std::size_t limit) {
        std::uint32_t count{}; value(count); require(count <= limit, "blob exceeds schema bound");
        const auto data = take(count); values.assign(data.begin(), data.end());
    }
    void text(std::string& text) {
        std::uint32_t count{}; value(count); require(count && count <= 256, "identity length outside bounds");
        const auto data = take(count); text.assign(reinterpret_cast<const char*>(data.data()), data.size());
    }
};

template<class A, class C> void context_fields(A& a, C& c) {
#define REG(N) a.value(c.r##N); a.value(c.f##N.u64);
    REG(0) REG(1) REG(2) REG(3) REG(4) REG(5) REG(6) REG(7)
    REG(8) REG(9) REG(10) REG(11) REG(12) REG(13) REG(14) REG(15)
    REG(16) REG(17) REG(18) REG(19) REG(20) REG(21) REG(22) REG(23)
    REG(24) REG(25) REG(26) REG(27) REG(28) REG(29) REG(30) REG(31)
#undef REG
    a.value(c.hi); a.value(c.lo); a.value(c.status_reg); a.value(c.mips3_float_mode);
    require(c.mips3_float_mode == 0, "unsupported FPR mode");
    // A decoded image may still be copied by its coordinator. It is not an
    // executable context: rebind this alias only in the final live placement.
    if constexpr (A::reading) c.f_odd = nullptr;
}
template<class A, class R> void route_fields(A& a, R& r) { a.value(r.queue); a.value(r.message); }
template<class A, class D> void device_fields(A& a, D& d) {
    a.value(d.version); a.value(d.producer_epoch); a.value(d.virtual_ticks);
    a.value(d.os_time_offset); a.value(d.total_vis); a.value(d.retraces_remaining);
    a.value(d.vi_current); a.value(d.vi_field);
    for (auto& vi : d.vi_states) {
        a.value(vi.mode_guest_address); a.value(vi.framebuffer); route_fields(a, vi.retrace);
        a.value(vi.state); a.value(vi.control); a.value(vi.retrace_count);
    }
    for (auto& v : d.vi_regs) a.value(v);
    for (auto& v : d.next_screen_regs) a.value(v);
    route_fields(a, d.sp); route_fields(a, d.dp); route_fields(a, d.ai); route_fields(a, d.si);
    a.count(d.timers, 4096);
    for (auto& timer : d.timers) {
        a.value(timer.guest_address); a.value(timer.remaining_ticks);
        a.value(timer.interval_ticks); route_fields(a, timer.route);
    }
    a.value(d.medium); a.blob(d.private_eeprom, 0x800);
    a.value(d.audio.frequency); a.value(d.audio.queued_stereo_samples); a.value(d.audio.rebuffering);
    a.count(d.audio.queued_pcm, 192000U * 2U * 2U);
    for (auto& sample : d.audio.queued_pcm) a.value(sample);
    a.blob(d.renderer_blob, 256U * 1024U);
}
template<class A, class S> void fields(A& a, S& s) {
    a.value(s.schema); a.value(s.save_medium_policy);
    auto& c = s.compatibility;
    a.text(c.program_sha256); a.text(c.executable_sha256); a.text(c.rom_sha256);
    a.text(c.runtime_revision); a.text(c.settings_sha256);
    a.value(s.epoch); a.blob(s.rdram, ram_size);
    a.count(s.threads, 64);
    for (auto& t : s.threads) {
        a.value(t.address); a.value(t.entry); a.value(t.initial_sp); a.value(t.initial_arg);
        a.value(t.phase); a.value(t.replay_active_divisor); a.value(t.cop1_rounding_mode);
        context_fields(a, t.context); a.count(t.frames, 1024);
        for (auto& f : t.frames) {
            a.value(f.function); a.value(f.pc); a.value(f.hi); a.value(f.lo);
            a.value(f.result); a.value(f.c1cs);
            for (auto& operand : f.operands) a.value(operand);
            a.count(f.extra_locals, 124);
            for (auto& local : f.extra_locals) a.value(local);
        }
        a.count(t.overlay_callsites, 1024);
        for (auto& callsite : t.overlay_callsites) a.value(callsite);
    }
    a.value(s.running_queue_head); a.value(s.external_wait_thread); a.value(s.boot_fcsr);
    a.value(s.replay_pending_divisor); a.value(s.replay_published_refresh_rate);
    a.value(s.scene_map_available); a.value(s.scene_map_id);
    a.value(s.host.dp_status); a.value(s.host.cutscene_active);
    a.count(s.external_messages, 4096);
    for (auto& message : s.external_messages) {
        a.value(message.queue); a.value(message.message);
        a.value(message.jam); a.value(message.requeue_if_blocked);
    }
    a.count(s.overlays, 4096);
    for (auto& o : s.overlays) {
        a.value(o.id); a.value(o.header); a.value(o.text); a.value(o.active); a.value(o.generation);
    }
    a.count(s.overlay_generations, 4096);
    for (auto& g : s.overlay_generations) { a.value(g.id); a.value(g.generation); }
    device_fields(a, s.devices);
    a.value(s.si.version);
    for (auto& byte : s.si.pif) a.value(byte);
    a.value(s.si.response_available); a.value(s.si.transfer_sequence); a.value(s.si.challenge_sequence);
    for (auto& byte : s.si.original_challenge) a.value(byte);
    for (auto& byte : s.rsp_dmem) a.value(byte);
}

std::filesystem::path checked_path(const std::filesystem::path& input) {
    const auto path = std::filesystem::absolute(input).lexically_normal();
    require(path.extension() == ".tooie-state", "only .tooie-state files are allowed");
    require(std::filesystem::is_directory(path.parent_path()), "state directory must already exist");
    return path;
}
void publish(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    static std::atomic_uint64_t sequence{0};
    auto temporary = path;
#ifdef _WIN32
    temporary += L".part-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Create checkpoint temporary");
    try {
        DWORD written = 0;
        if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
            throw std::system_error(GetLastError(), std::system_category(), "Write checkpoint");
        if (!FlushFileBuffers(file)) throw std::system_error(GetLastError(), std::system_category(), "Flush checkpoint");
        const auto closing = file; file = INVALID_HANDLE_VALUE;
        if (!CloseHandle(closing)) throw std::system_error(GetLastError(), std::system_category(), "Close checkpoint");
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Publish checkpoint");
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(temporary.c_str()); // Only our exclusively created temporary.
        throw;
    }
#else
    temporary += ".part-" + std::to_string(getpid()) + "-" + std::to_string(++sequence);
    int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) throw std::system_error(errno, std::generic_category(), "Create checkpoint temporary");
    try {
        while (!bytes.empty()) {
            const auto written = write(file, bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "Write checkpoint");
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
        if (fsync(file)) throw std::system_error(errno, std::generic_category(), "Flush checkpoint");
        const auto closing = file; file = -1;
        if (close(closing)) throw std::system_error(errno, std::generic_category(), "Close checkpoint");
        if (rename(temporary.c_str(), path.c_str())) throw std::system_error(errno, std::generic_category(), "Publish checkpoint");
        const int parent = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (parent < 0) throw std::system_error(errno, std::generic_category(), "Open checkpoint directory");
        const int result = fsync(parent), error = errno; close(parent);
        if (result) throw std::system_error(error, std::generic_category(), "Flush checkpoint directory");
    } catch (...) {
        if (file >= 0) close(file);
        unlink(temporary.c_str());
        throw;
    }
#endif
}
}

namespace tooie::persistent_state::file {
void save_atomic(const std::filesystem::path& input, const Snapshot& snapshot) {
    const auto path = checked_path(input);
    bounded(snapshot);
    Writer writer; writer.bytes.reserve(ram_size + 1024U * 1024U);
    writer.append(magic); fields(writer, snapshot);
    const auto hash = platform::digest(writer.bytes, true);
    writer.bytes.insert(writer.bytes.end(), hash.begin(), hash.end());
    require(writer.bytes.size() <= max_file, "file exceeds size limit");
    publish(path, writer.bytes);
}
Snapshot load_validated(const std::filesystem::path& input, const Compatibility& expected) {
    identity_valid(expected);
    const auto path = checked_path(input);
    const auto size = std::filesystem::file_size(path);
    require(size >= ram_size + 72 && size <= max_file, "file size outside bounds");
    Bytes bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "cannot open checkpoint");
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(stream.gcount() == static_cast<std::streamsize>(bytes.size()) &&
        stream.peek() == std::char_traits<char>::eof(), "short or changed checkpoint");
    const auto payload = std::span<const std::uint8_t>(bytes).first(bytes.size() - 64);
    const std::string stored(reinterpret_cast<const char*>(bytes.data() + payload.size()), 64);
    require(sha256(stored) && platform::digest(payload, true) == stored, "checksum mismatch");
    Reader reader{payload};
    const auto prefix = reader.take(magic.size());
    require(std::equal(prefix.begin(), prefix.end(), magic.begin()), "invalid magic");
    Snapshot snapshot; fields(reader, snapshot);
    require(reader.bytes.empty(), "trailing checkpoint payload");
    compatible(snapshot.compatibility, expected); bounded(snapshot);
    // No live state is touched. The scheduler still validates registry labels,
    // queue ownership, overlay maps and guest address extents before committing.
    return snapshot;
}
}
