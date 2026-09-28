#include "cheat_save_reset.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tooie::cheat_save_reset {
namespace {
constexpr std::size_t eeprom_size = 2048;
constexpr std::size_t first_block = 0x100;
constexpr std::size_t block_size = 0x1C0;
constexpr std::size_t data_size = block_size - 8;
constexpr std::size_t flag_bytes = 176;
constexpr std::array<unsigned, 12> active_flags{
    0x4C, 0x534, 0x535, 0x536, 0x537, 0x538, 0x539,
    0x53A, 0x53B, 0x53C, 0x53D, 0x5A3
};

struct SlotBlock {
    std::size_t base;
    std::size_t flags;
};

std::uint64_t read_be64(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (const auto byte : bytes) value = (value << 8) | byte;
    return value;
}

void write_be64(std::span<std::uint8_t> bytes, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
}

struct ParsedBlock {
    bool has_magic = false;
    int slot = 0;
    std::optional<std::size_t> flags;
    bool malformed = false;
};

ParsedBlock parse_block(std::span<const std::uint8_t> data) {
    ParsedBlock parsed;
    if (data.size() != data_size || data[0] != 'K' || data[1] != 'H' ||
        data[2] != 'J' || data[3] != 'C') return parsed;
    parsed.has_magic = true;
    std::size_t cursor = 4;
    bool terminated = false;
    while (cursor + 2 <= data.size()) {
        const auto tag = data[cursor++];
        if (tag == 0) { terminated = true; break; }
        const auto length = data[cursor++];
        if (tag > 6) parsed.malformed = true;
        if (cursor + length > data.size()) {
            parsed.malformed = true;
            return parsed;
        }
        if (tag == 2) {
            if (length != 1 || parsed.slot != 0) parsed.malformed = true;
            else parsed.slot = data[cursor];
        } else if (tag == 6) {
            if (length != flag_bytes || parsed.flags.has_value()) parsed.malformed = true;
            else parsed.flags = cursor;
        }
        cursor += length;
    }
    if (!terminated) parsed.malformed = true;
    return parsed;
}

std::filesystem::path unused_sibling(const std::filesystem::path& save,
                                     const char* suffix) {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        const auto candidate = save.parent_path() /
            (save.filename().string() + suffix + std::to_string(stamp) + "-" +
             std::to_string(attempt));
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error("Could not reserve a backup or temporary filename");
}

std::vector<std::uint8_t> read_image(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open EEPROM save");
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{stream},
                                    std::istreambuf_iterator<char>{});
    if (!stream.eof() && stream.fail()) throw std::runtime_error("Could not read EEPROM save");
    return bytes;
}

void write_image(const std::filesystem::path& path,
                 std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Could not create temporary save");
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.flush();
    if (!stream) throw std::runtime_error("Could not write temporary save");
    stream.close();
    if (!stream) throw std::runtime_error("Could not close temporary save");
}

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& save) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), save.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "Could not replace save");
#else
    std::filesystem::rename(temporary, save);
#endif
}
} // namespace

std::uint64_t checksum(std::span<const std::uint8_t> data) {
    // Tooie uses Rare's 64-bit type-A checksum. The guest iterates forward
    // and backward with different shift increments over the 0x1B8 data bytes.
    // Cross-checked against https://github.com/bryc/rare-n64-chksm and the
    // game's glcrc func_80800000_glcrc and core2 func_801168AC disassembly.
    // See docs/CHECKSUM_PROVENANCE.md for instruction and block-size mapping.
    const auto mix = [](std::uint64_t value) {
        const auto high = (value << 63) >> 31;
        const auto low = (value << 31) >> 32;
        const auto cross = (value << 44) >> 32;
        value = (high | low) ^ cross;
        return value ^ ((value >> 20) & 0xFFF);
    };
    std::uint64_t sum = 0x8F809F473108B3C1ULL;
    std::uint64_t forward = 0, backward = 0;
    unsigned shift = 0;
    for (const auto byte : data) {
        sum = mix(sum + (std::uint64_t{byte} << (shift & 15)));
        forward ^= sum;
        shift += 7;
    }
    for (std::size_t i = data.size(); i > 0; --i) {
        sum = mix(sum + (std::uint64_t{data[i - 1]} << (shift & 15)));
        backward ^= sum;
        shift += 3;
    }
    return ((forward & 0xFFFFFFFFULL) << 32) | (backward & 0xFFFFFFFFULL);
}

Result disable_active_cheats(const std::filesystem::path& save_path, int slot) {
    if (slot < 1 || slot > 3)
        return {.message = "Select game file 1, 2, or 3."};
    std::filesystem::path backup_path;
    try {
        if (!std::filesystem::exists(save_path))
            return {.success = true, .message = "No save exists for this profile; cheat access can be disabled."};
        const auto original = read_image(save_path);
        auto bytes = original;
        if (bytes.size() != eeprom_size)
            return {.message = "EEPROM save has an unexpected size; no change was made."};

        std::optional<SlotBlock> selected;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto base = first_block + i * block_size;
            const auto data = std::span<const std::uint8_t>(bytes.data() + base, data_size);
            const auto parsed = parse_block(data);
            if (!parsed.has_magic) continue;
            if (parsed.malformed)
                return {.message = "EEPROM contains a malformed save block; no change was made."};
            const auto stored = read_be64(std::span<const std::uint8_t>(
                bytes.data() + base + data_size, 8));
            if (checksum(data) != stored)
                return {.message = "EEPROM contains an invalid save checksum; no change was made."};
            if (parsed.slot != slot) continue;
            if (!parsed.flags)
                return {.message = "Selected game file has no flag data; no change was made."};
            if (selected)
                return {.message = "Duplicate selected game file blocks; no change was made."};
            selected = SlotBlock{base, base + *parsed.flags};
        }
        if (!selected)
            return {.success = true, .message = "Selected game file has no saved data; cheat access can be disabled."};

        bool changed = false;
        for (const auto flag : active_flags) {
            const auto index = flag - 0x28;
            auto& value = bytes[selected->flags + index / 8];
            const auto mask = static_cast<std::uint8_t>(1u << (index & 7));
            changed |= (value & mask) != 0;
            value &= static_cast<std::uint8_t>(~mask);
        }
        if (!changed)
            return {.success = true, .message = "Active cheats were already off in the selected game file."};

        write_be64(std::span<std::uint8_t>(bytes.data() + selected->base + data_size, 8),
                   checksum(std::span<const std::uint8_t>(
                       bytes.data() + selected->base, data_size)));
        // Back up the untouched original before attempting any replacement.
        backup_path = unused_sibling(save_path, ".cheat-reset-backup-");
        std::filesystem::copy_file(save_path, backup_path,
                                   std::filesystem::copy_options::none);
        if (read_image(backup_path) != original)
            return {.backup_path = backup_path,
                    .message = "Save backup verification failed; no change was made."};

        const auto temporary = unused_sibling(save_path, ".cheat-reset-temporary-");
        try {
            write_image(temporary, bytes);
            if (read_image(temporary) != bytes)
                throw std::runtime_error("Temporary save verification failed");
            if (read_image(save_path) != original)
                throw std::runtime_error("Save changed during reset; original was preserved");
            replace_file(temporary, save_path);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        return {.success = true, .changed = true, .backup_path = backup_path,
                .message = "Active cheats were turned off in the selected game file."};
    } catch (const std::exception& error) {
        return {.backup_path = backup_path, .message = error.what()};
    }
}
} // namespace tooie::cheat_save_reset
