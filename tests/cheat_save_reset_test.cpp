#include "cheat_save_reset.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::size_t block_size = 0x1C0;
constexpr std::size_t block_data_size = block_size - 8;
constexpr std::size_t flag_count = 176;

void put_checksum(std::vector<std::uint8_t>& bytes, std::size_t base) {
    const auto sum = tooie::cheat_save_reset::checksum(
        std::span(bytes.data() + base, block_data_size));
    for (unsigned i = 0; i < 8; ++i)
        bytes[base + block_data_size + i] = static_cast<std::uint8_t>(sum >> (56 - 8 * i));
}

void set_flag(std::vector<std::uint8_t>& bytes, std::size_t flags, unsigned flag) {
    const auto index = flag - 0x28;
    bytes[flags + index / 8] |= static_cast<std::uint8_t>(1u << (index % 8));
}

std::size_t make_slot(std::vector<std::uint8_t>& bytes, int slot) {
    const auto base = 0x100 + (slot - 1) * block_size;
    bytes[base] = 'K'; bytes[base + 1] = 'H';
    bytes[base + 2] = 'J'; bytes[base + 3] = 'C';
    // TLV tag 2 stores the game file number; tag 6 stores its flags.
    bytes[base + 4] = 2; bytes[base + 5] = 1;
    bytes[base + 6] = static_cast<std::uint8_t>(slot);
    bytes[base + 7] = 6; bytes[base + 8] = flag_count;
    const auto flags = base + 9;
    bytes[flags + 0] = 0xA5; // unrelated discovery/progression bits
    set_flag(bytes, flags, 0x533); // shares a byte with active flags
    set_flag(bytes, flags, 0x4C);
    set_flag(bytes, flags, 0x534);
    set_flag(bytes, flags, 0x53D);
    set_flag(bytes, flags, 0x5A3);
    put_checksum(bytes, base);
    return flags;
}

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!stream) throw std::runtime_error("fixture write failed");
}
}

int main() {
    const std::array<std::uint8_t, block_data_size> zeros{};
    assert(tooie::cheat_save_reset::checksum(zeros) == 0xB0E934687B9CF8C6ULL);

    const auto fixture_dir = std::filesystem::temp_directory_path() /
        ("tooie-cheat-reset-fixture-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(fixture_dir);
    const auto save = fixture_dir / "disposable-eeprom.bin";
    std::vector<std::uint8_t> original(2048, 0);
    make_slot(original, 1);
    const auto flags2 = make_slot(original, 2);
    make_slot(original, 3);
    write(save, original);

    const auto result = tooie::cheat_save_reset::disable_active_cheats(save, 2);
    assert(result.success && result.changed && !result.backup_path.empty());
    assert(read(result.backup_path) == original);
    const auto updated = read(save);
    assert(updated.size() == original.size());
    const auto selected_base = 0x100 + block_size;
    auto expected = original;
    for (const auto flag : {0x4C, 0x534, 0x535, 0x536, 0x537, 0x538,
                            0x539, 0x53A, 0x53B, 0x53C, 0x53D, 0x5A3}) {
        const auto index = flag - 0x28;
        expected[flags2 + index / 8] &= static_cast<std::uint8_t>(~(1u << (index & 7)));
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (i >= selected_base + block_data_size && i < selected_base + block_size) continue;
        assert(updated[i] == expected[i]);
    }
    const auto flag_is_set = [&](unsigned flag) {
        const auto index = flag - 0x28;
        return (updated[flags2 + index / 8] & (1u << (index % 8))) != 0;
    };
    assert(!flag_is_set(0x4C));
    for (unsigned flag = 0x534; flag <= 0x53D; ++flag) assert(!flag_is_set(flag));
    assert(!flag_is_set(0x5A3));
    assert(flag_is_set(0x533));
    assert(updated[flags2] == original[flags2]);

    std::uint64_t stored = 0;
    for (unsigned i = 0; i < 8; ++i)
        stored = (stored << 8) | updated[selected_base + block_data_size + i];
    assert(stored == tooie::cheat_save_reset::checksum(
        std::span(updated.data() + selected_base, block_data_size)));

    const auto no_change = tooie::cheat_save_reset::disable_active_cheats(save, 2);
    assert(no_change.success && !no_change.changed);
    auto corrupt = original;
    corrupt[selected_base + 10] ^= 1;
    write(save, corrupt);
    const auto rejected = tooie::cheat_save_reset::disable_active_cheats(save, 2);
    assert(!rejected.success && read(save) == corrupt);
    std::filesystem::remove(save);
    std::filesystem::remove(result.backup_path);
    std::filesystem::remove(fixture_dir);
    std::cout << "PASS selected-slot active flags, unrelated bytes, backup, checksum and corruption rejection\n";
}
