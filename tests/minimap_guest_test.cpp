#include "minimap.hpp"
#include "scene_observer.hpp"

#include "recomp.h"

#include <cassert>
#include <bit>
#include <cstdint>
#include <vector>

namespace {
constexpr std::uint32_t rdram_base = 0x80000000U;
std::vector<std::uint8_t> memory(8U * 1024U * 1024U);
tooie::scene::Snapshot current_scene{};

gpr guest(std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}

void write_word(std::uint32_t address, std::uint32_t value) {
    auto* rdram = memory.data();
    MEM_W(0, guest(address)) = static_cast<std::int32_t>(value);
}

void write_float(std::uint32_t address, float value) {
    write_word(address, std::bit_cast<std::uint32_t>(value));
}

void install_player(std::uint32_t player, std::uint32_t position,
    std::uint32_t yaw) {
    write_word(player + 0xE4U, position);
    write_word(player + 0xF8U, yaw);
}
}

namespace tooie::scene {
Snapshot snapshot() noexcept { return current_scene; }
}

int main() {
    constexpr std::uint32_t player = 0x80001000U;
    constexpr std::uint32_t position = 0x80002000U;
    constexpr std::uint32_t yaw = 0x80003000U;
    recomp_context context{};
    context.r4 = player;

    current_scene.map_available = true;
    current_scene.map_id = 0x14AU;
    tooie::minimap::set_mode(tooie::minimap::Mode::Trail);
    install_player(player, position, yaw);
    write_float(position, 125.5f);
    write_float(position + 4U, -40.25f);
    write_float(position + 8U, 900.75f);
    write_float(yaw, 270.0f);

    // MEM_W stores a native word in the word-swapped RDRAM image. Byte reads
    // still reconstruct the guest's big-endian pointer order.
    auto* rdram = memory.data();
    assert(MEM_BU(0, guest(player + 0xE4U)) == 0x80U);
    assert(MEM_BU(1, guest(player + 0xE4U)) == 0x00U);
    assert(MEM_BU(2, guest(player + 0xE4U)) == 0x20U);
    assert(MEM_BU(3, guest(player + 0xE4U)) == 0x00U);

    tooie_minimap_tick(rdram, &context);
    auto state = tooie::minimap::trail_snapshot();
    assert(state.current);
    assert(state.map_id == 0x14AU);
    assert(state.position.x == 125.5f);
    assert(state.position.y == -40.25f);
    assert(state.position.z == 900.75f);
    assert(state.yaw_degrees == 270.0f);
    assert(state.count == 1);
    assert(!state.camera_heading_available);
    assert(state.camera_yaw_degrees == state.yaw_degrees);
    write_word(0x8012D500U, 0x80004000U);
    write_float(0x8000401CU, 90.0f);
    tooie_minimap_tick(rdram, &context);
    state = tooie::minimap::trail_snapshot();
    assert(state.camera_heading_available && state.camera_yaw_degrees == 90.0f);
    assert(state.yaw_degrees == 270.0f);
    write_word(0x8012D500U, 0x807FFFFCU);
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().camera_heading_available);

    // Each invalid pointer path must clear state before any nested read.
    install_player(player, position + 2U, yaw);
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().current);

    install_player(player, position, 0x807FFFFEU);
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().current);

    install_player(player, position, yaw);
    context.r4 = player + 2U;
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().current);

    context.r4 = player;
    current_scene.map_available = false;
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().current);

    current_scene.map_available = true;
    current_scene.activation_active = true;
    tooie_minimap_tick(rdram, &context);
    assert(!tooie::minimap::trail_snapshot().current);

    current_scene.activation_active = false;
    tooie_minimap_tick(rdram, &context);
    assert(tooie::minimap::trail_snapshot().current);
    tooie::minimap::set_mode(tooie::minimap::Mode::Off);
    assert(!tooie::minimap::trail_snapshot().current);
}
