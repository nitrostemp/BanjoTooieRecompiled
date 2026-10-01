#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace tooie::platform {
// Lowercase hexadecimal. false selects SHA-1; true selects SHA-256.
std::string digest(std::span<const uint8_t> bytes,bool sha256);
std::string file_sha256(const std::filesystem::path& path);
std::filesystem::path executable_path();
// Directory holding assets/, runtime-data/ and recompcontrollerdb.txt: beside
// the executable, or Contents/Resources when it runs inside a macOS app bundle.
std::filesystem::path resource_directory(const std::filesystem::path& executable);
std::filesystem::path resource_directory();
enum class FrontendLaunch : std::uint8_t { RestartGame, Launcher, PracticeGame };
#ifdef _WIN32
// CreateProcessW command line; exposed for focused argument-quoting coverage.
std::wstring frontend_command_line(const std::filesystem::path& executable,
    const std::filesystem::path& profile, FrontendLaunch mode);
// Relaunched children wait before opening the shared profile logs. The parent
// can therefore record its actual launch result and finish all destructors first.
void wait_for_frontend_parent(std::uint32_t process_id);
#endif
// Fresh frontend process with the same profile; caller must first drain runtime.
// On Windows the child waits for parent exit before it initializes profile logs.
void launch_frontend(const std::filesystem::path& profile, FrontendLaunch mode);

enum class ProcessMemoryKind : std::uint8_t { Unavailable, PrivateBytes, ResidentSet, PhysicalFootprint };
struct ProcessMemory {
    std::uint64_t bytes = 0;
    ProcessMemoryKind kind = ProcessMemoryKind::Unavailable;
};
// Windows reports committed private bytes; Linux reports RSS from /proc/self/statm;
// macOS reports task physical footprint. These are intentionally not interchangeable.
ProcessMemory process_memory() noexcept;

// Reserve the full guard address space, then make only the leading accessible
// bytes readable/writable and zero-initialized. Accessible bytes must be in
// (0,reservation_size]. Throws on invalid arguments or allocation/protection
// failure, cleaning any partially created reservation before returning.
uint8_t* reserve_rdram(std::size_t reservation_size,std::size_t accessible_size);
// Both arguments must identify the successful reservation returned above.
void release_rdram(uint8_t* address,std::size_t reservation_size);
}
