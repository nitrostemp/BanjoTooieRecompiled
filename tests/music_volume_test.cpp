#include "music_volume.hpp"
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    tooie::music::set_percent(100);
    assert(tooie::music::scale_sequence_volume(0) == 0);
    assert(tooie::music::scale_sequence_volume(32767) == 32767);
    tooie::music::set_percent(50);
    assert(tooie::music::scale_sequence_volume(20000) == 10000);
    assert(tooie::music::scale_sequence_volume(-20000) == -10000);
    tooie::music::set_percent(150);
    assert(tooie::music::percent() == 100);
    assert(tooie::music::scale_sequence_volume(20000) == 20000);
    assert(tooie::music::is_jukebox_music_track(0x32));
    assert(tooie::music::is_jukebox_music_track(0x54));
    assert(!tooie::music::is_jukebox_music_track(0x01));

    std::vector<std::uint8_t> storage(8 * 1024 * 1024);
    auto* rdram = storage.data();
    constexpr std::size_t refresh_byte = (0x801359B0U ^ 3U) - 0x80000000U;
    tooie::music::set_percent(25);
    recomp_context context{};
    constexpr auto manager_record = static_cast<gpr>(static_cast<std::int32_t>(0x801357D0U));
    MEM_H(0x28, manager_record) = 0x32;
    context.r4 = 0;
    context.r5 = 20000;
    tooie_music_volume_apply(rdram, &context);
    assert(static_cast<std::int16_t>(context.r5) == 5000);
    MEM_H(0x28, manager_record) = 0x01;
    context.r5 = 20000;
    tooie_music_volume_apply(rdram, &context);
    assert(static_cast<std::int16_t>(context.r5) == 20000);

    tooie_music_volume_tick(rdram, nullptr);
    assert(storage[refresh_byte] == 1);
    storage[refresh_byte] = 0;
    tooie_music_volume_tick(rdram, nullptr);
    assert(storage[refresh_byte] == 0);
}
