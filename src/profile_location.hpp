#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

// Chooses, locks and, once, migrates the frontend profile folder.
// An explicit --profile-dir, a portable.txt beside the executable, or the
// platform's per-user application-data folder select the profile. Only the
// default folder is migrated from the legacy development name; the legacy
// folder itself is never modified.
namespace tooie::profile {
inline constexpr const char* product_folder = "BanjoTooieRecompiled";
inline constexpr const char* legacy_folder = "TooieRecomp-frontend";
// Written into a default profile this application created or migrated.
inline constexpr const char* marker_file = "profile.json";
// Written into a migrated profile; local user data, never packaged.
inline constexpr const char* receipt_file = "migrated-from.json";
inline constexpr const char* staging_marker_file = ".migration-staging";

// Startup cannot safely continue. The message is for the player; no profile
// data was changed.
struct StartupBlocked : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Exclusive, non-blocking per-profile lock held for the process lifetime.
// Windows uses a session-local named mutex keyed by the normalized absolute
// profile path; other platforms use flock on a close-on-exec temporary file.
class Lock {
public:
    Lock() = default;
    Lock(Lock&& other) noexcept;
    Lock& operator=(Lock&& other) noexcept;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    ~Lock();
    explicit operator bool() const noexcept;
private:
    friend Lock try_lock(const std::filesystem::path& root);
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};
// Returns an empty Lock when another holder has this profile.
Lock try_lock(const std::filesystem::path& root);
// Stable name component for a profile path (case-insensitive on Windows).
std::string lock_key(const std::filesystem::path& root);

enum class Origin { Explicit, Portable, Existing, Created, Adopted, Migrated };
const char* origin_name(Origin origin) noexcept;

struct Resolution {
    std::filesystem::path root;
    Origin origin = Origin::Explicit;
    Lock lock;
    // Migrated: the copied legacy folder. Existing: a legacy folder that was
    // present and deliberately not merged.
    std::filesystem::path legacy;
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

struct Environment {
    std::filesystem::path executable_dir;
    // Folder that contains the product and legacy profile folders.
    std::filesystem::path default_parent;
    // Non-Windows APP_FOLDER_PATH override: used as-is, never migrated.
    std::filesystem::path override_root;
    // Best-effort detection of another running copy that may still write the
    // legacy profile. Older builds hold no profile lock.
    std::function<bool()> other_instance_running;
    // Test injection points; unset in the application.
    std::function<void(const std::filesystem::path& from, const std::filesystem::path& to)> copy_file;
    std::function<void(const std::filesystem::path& relative)> after_file_copied;
};

Environment system_environment();
// Throws StartupBlocked for conditions the player must resolve.
Resolution resolve(const std::filesystem::path& explicit_root, const Environment& environment);
}
