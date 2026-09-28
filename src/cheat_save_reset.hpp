#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace tooie::cheat_save_reset {

struct Result {
    bool success = false;
    bool changed = false;
    std::filesystem::path backup_path;
    std::string message;
};

// The Rare type-A checksum over the 0x1B8 data bytes of a Tooie save block.
std::uint64_t checksum(std::span<const std::uint8_t> data);

// Operates only on a selected in-game file (1, 2, or 3) in the offline 2048-byte
// EEPROM image. The caller must establish that gameplay has not started.
Result disable_active_cheats(const std::filesystem::path& save_path, int slot);

} // namespace tooie::cheat_save_reset
