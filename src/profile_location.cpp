#include "profile_location.hpp"

#include "platform_support.hpp"
#include "tooie_build_identity.hpp"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tooie::profile {
namespace {
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string json_string(const std::string& text) {
    std::string result = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    result += escaped;
                } else {
                    result += c;
                }
        }
    }
    return result + "\"";
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.flush();
    if (!output) throw std::runtime_error("Could not write " + utf8(path.filename()));
}

// Links, junctions and other reparse points are never followed or copied.
bool is_link(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
#endif
}

bool exists_entry(const fs::path& path) {
    std::error_code error;
    return fs::exists(fs::symlink_status(path, error));
}

std::string marker_text(const char* origin) {
    return std::string("{\n  \"schema\": 1,\n  \"origin\": ") + json_string(origin) +
        ",\n  \"created\": " + json_string(utc_now()) +
        ",\n  \"version\": " + json_string(tooie::build_identity::version) + "\n}\n";
}

struct FileRecord {
    std::uint64_t bytes = 0;
    std::string sha256;
    bool operator==(const FileRecord&) const = default;
};
struct Snapshot {
    std::vector<fs::path> directories;
    std::map<fs::path, FileRecord> files;
    std::uint64_t bytes = 0;
    bool operator==(const Snapshot&) const = default;
};

[[noreturn]] void blocked(const std::string& text) { throw StartupBlocked(text); }

std::string blocked_suffix() {
    return " Nothing in either profile folder was changed.";
}

// Captures every entry below root. Anything other than an ordinary folder or
// file stops migration rather than being skipped.
Snapshot snapshot(const fs::path& root) {
    Snapshot result;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
    if (error) blocked("The older profile folder " + utf8(root) + " could not be read (" +
        error.message() + ")." + blocked_suffix());
    // A failed increment can also end the iteration, so its error is checked
    // after every step rather than only while entries remain.
    const auto advance = [&] {
        it.increment(error);
        if (error) blocked("The older profile folder could not be read completely (" +
            error.message() + "). Check that you can open every folder inside it, then start "
            "the game again." + blocked_suffix());
    };
    for (; it != end; advance()) {
        const auto& path = it->path();
        const auto relative = path.lexically_relative(root);
        if (is_link(path)) {
            it.disable_recursion_pending();
            blocked("The older profile contains a linked item, " + utf8(relative) +
                ", which cannot be copied safely. Replace it with an ordinary file or folder, "
                "or remove it, then start the game again." + blocked_suffix());
        }
        const auto status = fs::symlink_status(path, error);
        if (error) blocked("Could not inspect " + utf8(relative) + " in the older profile (" +
            error.message() + ")." + blocked_suffix());
        if (fs::is_directory(status)) {
            result.directories.push_back(relative);
        } else if (fs::is_regular_file(status)) {
            FileRecord record;
            record.bytes = fs::file_size(path, error);
            if (error) blocked("Could not read " + utf8(relative) + " in the older profile (" +
                error.message() + ")." + blocked_suffix());
            try {
                record.sha256 = platform::file_sha256(path);
            } catch (const std::exception& failure) {
                blocked("Could not read " + utf8(relative) + " in the older profile (" +
                    failure.what() + "). Close any program using it and try again." + blocked_suffix());
            }
            result.bytes += record.bytes;
            result.files.emplace(relative, std::move(record));
        } else {
            blocked("The older profile contains an unsupported item, " + utf8(relative) +
                ". Remove it, then start the game again." + blocked_suffix());
        }
    }
    std::sort(result.directories.begin(), result.directories.end());
    return result;
}

// Interrupted migrations leave only a marked staging folder; the legacy
// profile remains the source of truth, so the partial copy is discarded.
void remove_abandoned_staging(const fs::path& parent) {
    std::error_code error;
    const std::string prefix = std::string(product_folder) + ".migrating-";
    for (fs::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
        const auto name = utf8(it->path().filename());
        if (name.rfind(prefix, 0) != 0 || is_link(it->path())) continue;
        if (!fs::is_regular_file(it->path() / staging_marker_file)) continue;
        std::error_code ignored;
        fs::remove_all(it->path(), ignored);
    }
}

bool commit_rename(const fs::path& staging, const fs::path& target) {
#ifdef _WIN32
    // No MOVEFILE_REPLACE_EXISTING: an existing destination is never replaced.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return false;
        Sleep(200); // Transient sharing violations, for example antivirus scans.
    }
    return false;
#else
    // Serialized by the destination lock; POSIX rename could replace an empty
    // destination directory, so check immediately before renaming.
    if (exists_entry(target)) return false;
    std::error_code error;
    fs::rename(staging, target, error);
    return !error;
#endif
}

enum class TargetState { Absent, Empty, Marked, ProfileContent, Unknown };

TargetState inspect(const fs::path& target) {
    if (!exists_entry(target)) return TargetState::Absent;
    std::error_code error;
    if (is_link(target) || !fs::is_directory(fs::symlink_status(target, error)))
        blocked("The profile location " + utf8(target) + " is not an ordinary folder. "
            "Rename or remove it, then start the game again." + blocked_suffix());
    if (fs::is_regular_file(target / marker_file)) return TargetState::Marked;
    if (fs::is_empty(target, error) && !error) return TargetState::Empty;
    for (const char* name : {"config", "saves", "private-practice.tooie-state"})
        if (exists_entry(target / name)) return TargetState::ProfileContent;
    return TargetState::Unknown;
}

Lock take_lock(const fs::path& root, const std::string& owner) {
    auto lock = try_lock(root);
    if (!lock) blocked("Another copy of Banjo-Tooie: Recompiled is already using " + owner +
        " (" + utf8(root) + "). Close it, then start the game again.");
    return lock;
}

Resolution migrate(const fs::path& legacy, const fs::path& target, Lock target_lock,
    const Environment& environment) {
    std::error_code error;
    if (is_link(legacy) || !fs::is_directory(fs::symlink_status(legacy, error)))
        blocked("The older profile location " + utf8(legacy) + " is not an ordinary folder. "
            "Copy your saves manually or remove it, then start the game again." + blocked_suffix());
    // Current builds hold this lock while using the legacy folder (for
    // example through --profile-dir); older builds do not, hence the
    // additional process check and the post-copy comparison below.
    auto legacy_lock = take_lock(legacy, "the older profile folder");
    if (environment.other_instance_running && environment.other_instance_running())
        blocked("Your saves and settings need to be copied to the new profile folder, but another "
            "copy of the game is running and may still be changing them. Close every other copy "
            "of Banjo-Tooie: Recompiled, then start the game again." + blocked_suffix());

    const auto before = snapshot(legacy);
    const auto parent = target.parent_path();
    const auto space = fs::space(parent, error);
    constexpr std::uint64_t margin = 16ull << 20;
    if (!error && space.available < before.bytes + margin)
        blocked("There is not enough free disk space to copy your profile (about " +
            std::to_string((before.bytes + margin) / (1u << 20) + 1) + " MB needed). "
            "Free some space, then start the game again." + blocked_suffix());

    static unsigned sequence = 0;
    const auto staging = parent / (std::string(product_folder) + ".migrating-" +
        std::to_string(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            getpid()
#endif
        ) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence));
    const auto discard = [&] { std::error_code ignored; fs::remove_all(staging, ignored); };
    const std::string changed_message = "Your older profile changed while it was being copied. "
        "Close every other copy of Banjo-Tooie: Recompiled, then start the game again." + blocked_suffix();
    try {
        if (!fs::create_directory(staging))
            throw std::runtime_error("temporary folder already exists");
        write_text(staging / staging_marker_file, "Incomplete profile copy; safe to delete.\n");
        for (const auto& directory : before.directories) fs::create_directories(staging / directory);
        for (const auto& [relative, record] : before.files) {
            const auto from = legacy / relative;
            const auto to = staging / relative;
            fs::create_directories(to.parent_path());
            if (environment.copy_file) environment.copy_file(from, to);
            else fs::copy_file(from, to, fs::copy_options::none);
            if (fs::file_size(to) != record.bytes || platform::file_sha256(to) != record.sha256) {
                std::error_code size_error;
                if (fs::file_size(from, size_error) != record.bytes || size_error ||
                    platform::file_sha256(from) != record.sha256)
                    blocked(changed_message);
                throw std::runtime_error("copied file " + utf8(relative) + " did not verify");
            }
            if (environment.after_file_copied) environment.after_file_copied(relative);
        }
    } catch (const StartupBlocked&) {
        discard();
        throw;
    } catch (const std::exception& failure) {
        discard();
        blocked(std::string("Your profile could not be copied to the new folder (") + failure.what() +
            "). Check free disk space and folder permissions, then start the game again." + blocked_suffix());
    }
    Snapshot after;
    try {
        after = snapshot(legacy);
    } catch (...) {
        discard();
        throw;
    }
    if (after != before) {
        discard();
        blocked(changed_message);
    }
    try {
        write_text(staging / marker_file, marker_text("migrated"));
        write_text(staging / receipt_file, std::string("{\n  \"schema\": 1,\n  \"source\": ") +
            json_string(utf8(legacy)) + ",\n  \"copied\": " + json_string(utc_now()) +
            ",\n  \"files\": " + std::to_string(before.files.size()) +
            ",\n  \"bytes\": " + std::to_string(before.bytes) +
            ",\n  \"version\": " + json_string(tooie::build_identity::version) +
            ",\n  \"note\": \"The source folder was copied and left unchanged.\"\n}\n");
    } catch (const std::exception& failure) {
        discard();
        blocked(std::string("Your profile could not be copied to the new folder (") + failure.what() +
            ")." + blocked_suffix());
    }
    if (!commit_rename(staging, target)) {
        discard();
        blocked("The new profile folder " + utf8(target) + " could not be completed. "
            "Start the game again; if this repeats, check folder permissions." + blocked_suffix());
    }
    std::error_code ignored;
    fs::remove(target / staging_marker_file, ignored);
    Resolution result;
    result.root = target;
    result.origin = Origin::Migrated;
    result.lock = std::move(target_lock);
    result.legacy = legacy;
    result.files = before.files.size();
    result.bytes = before.bytes;
    return result;
}
}

Lock::Lock(Lock&& other) noexcept {
#ifdef _WIN32
    handle_ = std::exchange(other.handle_, nullptr);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
}

Lock& Lock::operator=(Lock&& other) noexcept {
    if (this != &other) {
        Lock released(std::move(*this));
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

Lock::~Lock() {
#ifdef _WIN32
    if (handle_) {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) close(fd_);
#endif
}

Lock::operator bool() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

std::string lock_key(const fs::path& root) {
    auto normal = fs::absolute(root).lexically_normal();
    auto text = normal.native();
    while (text.size() > 1 && (text.back() == fs::path::preferred_separator || text.back() == '/'))
        text.pop_back();
#ifdef _WIN32
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
#endif
    const auto bytes = utf8(fs::path(text));
    return platform::digest(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), true);
}

Lock try_lock(const fs::path& root) {
    Lock lock;
    const auto key = lock_key(root);
#ifdef _WIN32
    const auto name = L"Local\\BanjoTooieRecompiled.profile." + fs::path(key).wstring();
    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!handle) throw std::system_error(GetLastError(), std::system_category(), "Create profile lock");
    const DWORD waited = WaitForSingleObject(handle, 0);
    // An abandoned mutex belonged to a process that exited, such as the
    // parent of a Restart/Return relaunch; ownership passes to this process.
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) lock.handle_ = handle;
    else CloseHandle(handle);
#else
    const auto path = fs::temp_directory_path() / ("BanjoTooieRecompiled-" + key + ".lock");
    const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Create profile lock");
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) lock.fd_ = fd;
    else close(fd);
#endif
    return lock;
}

const char* origin_name(Origin origin) noexcept {
    switch (origin) {
        case Origin::Explicit: return "explicit";
        case Origin::Portable: return "portable";
        case Origin::Existing: return "existing";
        case Origin::Created: return "created";
        case Origin::Adopted: return "adopted";
        case Origin::Migrated: return "migrated";
    }
    return "unknown";
}

Environment system_environment() {
    Environment environment;
    environment.executable_dir = platform::executable_path().parent_path();
#ifdef _WIN32
    PWSTR folder = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    if (SUCCEEDED(result) && folder) environment.default_parent = folder;
    if (folder) CoTaskMemFree(folder);
    const auto executable_name = platform::executable_path().filename().wstring();
    environment.other_instance_running = [executable_name] {
        // Older builds took no profile lock. A same-name process snapshot is
        // the practical guard; the post-copy comparison catches late writers.
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return true;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        const DWORD self = GetCurrentProcessId();
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
            found = entry.th32ProcessID != self && _wcsicmp(entry.szExeFile, executable_name.c_str()) == 0;
        CloseHandle(snapshot);
        return found;
    };
#else
    if (const char* override_path = std::getenv("APP_FOLDER_PATH")) environment.override_root = override_path;
    if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
        environment.default_parent = fs::path(home) / "Library/Application Support";
#else
        environment.default_parent = fs::path(home) / ".config";
#endif
    }
#endif
    return environment;
}

Resolution resolve(const fs::path& explicit_root, const Environment& environment) {
    Resolution result;
    const auto explicit_like = [&](const fs::path& root, Origin origin) {
        result.root = fs::absolute(root);
        result.origin = origin;
        result.lock = take_lock(result.root, "this profile folder");
        return std::move(result);
    };
    if (!explicit_root.empty()) return explicit_like(explicit_root, Origin::Explicit);
    if (!environment.executable_dir.empty() && fs::is_regular_file(environment.executable_dir / "portable.txt"))
        return explicit_like(environment.executable_dir, Origin::Portable);
    if (!environment.override_root.empty()) return explicit_like(environment.override_root, Origin::Explicit);
    if (environment.default_parent.empty())
        throw std::runtime_error("Could not locate the Banjo-Tooie: Recompiled profile folder");

    const auto parent = fs::absolute(environment.default_parent);
    const auto target = parent / product_folder;
    const auto legacy = parent / legacy_folder;
    auto target_lock = take_lock(target, "the profile folder");
    remove_abandoned_staging(parent);

    switch (inspect(target)) {
        case TargetState::Marked:
            result.origin = Origin::Existing;
            if (exists_entry(legacy)) result.legacy = legacy;
            break;
        case TargetState::ProfileContent:
            if (exists_entry(legacy))
                blocked("Two profile folders were found: " + utf8(target) + " and the older " +
                    utf8(legacy) + ". They were not merged. Keep the one you want, rename or move "
                    "the other out of " + utf8(parent) + ", then start the game again." + blocked_suffix());
            write_text(target / marker_file, marker_text("adopted"));
            result.origin = Origin::Adopted;
            break;
        case TargetState::Unknown:
            blocked("The folder " + utf8(target) + " contains files that are not a Banjo-Tooie: "
                "Recompiled profile. Move them elsewhere, then start the game again." + blocked_suffix());
        case TargetState::Empty: {
            std::error_code error;
            fs::remove(target, error); // Removes only an empty folder.
            if (error) blocked("The empty folder " + utf8(target) + " could not be prepared (" +
                error.message() + ")." + blocked_suffix());
            [[fallthrough]];
        }
        case TargetState::Absent:
            if (exists_entry(legacy)) return migrate(legacy, target, std::move(target_lock), environment);
            fs::create_directories(target);
            write_text(target / marker_file, marker_text("created"));
            result.origin = Origin::Created;
            break;
    }
    std::error_code ignored;
    fs::remove(target / staging_marker_file, ignored);
    result.root = target;
    result.lock = std::move(target_lock);
    return result;
}
}
