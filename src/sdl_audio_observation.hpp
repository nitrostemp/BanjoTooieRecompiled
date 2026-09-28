#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
namespace tooie::sdl_observation {
#ifdef _WIN32
const void* imported_anchor() noexcept;
#endif
struct Report {bool enabled=false;uint64_t count=0,attempted=0,overflow=0;};
class Session {
public:
    Session();
    ~Session() noexcept;
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    // Empty directory is default off: no module lookup, controls or output.
    // All controls belong to one owner thread. Anchor must be imported from SDL.
    void begin(const std::filesystem::path& directory,const void* imported_sdl_anchor,
               const std::filesystem::path& expected_module,const std::string& expected_sha256);
    // Caller must have closed/joined EVERY audio device/producer before this.
    // Always attempts Release after a successful Snapshot, even if output fails.
    Report finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
