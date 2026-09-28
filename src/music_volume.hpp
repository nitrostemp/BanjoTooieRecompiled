#pragma once
#include <cstdint>
#include "recomp.h"

namespace tooie::music {
// Frontend-owned setting. Values above 100 never amplify sequence data.
void set_percent(unsigned percent) noexcept;
unsigned percent() noexcept;
std::int16_t scale_sequence_volume(std::int16_t original) noexcept;
bool is_jukebox_music_track(std::int16_t track_id) noexcept;
}

// Generated-code hooks at Tooie's six-player music manager and its private
// master-volume setter. The tick asks the original manager to republish all
// active music volumes after a setting change; the setter scales a1/r5 only
// for track IDs in the original Jukebox playlist. Other sequence content and
// the independent func_800C4xxx spatial sound-effect service remain unchanged.
extern "C" void tooie_music_volume_tick(uint8_t* rdram, recomp_context* ctx) noexcept;
extern "C" void tooie_music_volume_apply(uint8_t* rdram, recomp_context* ctx) noexcept;
