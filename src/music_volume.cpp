#include "music_volume.hpp"
#include "recomp.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <limits>

namespace {
std::atomic_uint music_percent{100};
std::atomic_uint requested_generation{0};
std::atomic_uint guest_applied_generation{0};

constexpr gpr guest_address(std::uint32_t address) noexcept {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}

// chmusicmenu ROM 0x020809D0, overlay +0x12A0 through the 0x009A sentinel.
constexpr std::array<std::uint16_t, 47> jukebox_tracks{{
    0x32, 0x33, 0x55, 0x39, 0x38, 0x3E, 0x42, 0x48, 0x40, 0x49,
    0x47, 0x4C, 0x45, 0x4B, 0x4F, 0x50, 0x53, 0x3B, 0x52, 0x4E,
    0x3F, 0x63, 0x41, 0x56, 0x57, 0x61, 0x76, 0x75, 0x7E, 0x82,
    0x7A, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x62, 0x7F, 0x5E,
    0x5F, 0x1F, 0x20, 0x65, 0x66, 0x64, 0x54,
}};
}

void tooie::music::set_percent(unsigned percent) noexcept {
    const auto normalized = std::min(percent, 100U);
    if (music_percent.exchange(normalized, std::memory_order_acq_rel) != normalized) {
        requested_generation.fetch_add(1, std::memory_order_release);
    }
}

unsigned tooie::music::percent() noexcept {
    return music_percent.load(std::memory_order_acquire);
}

std::int16_t tooie::music::scale_sequence_volume(std::int16_t original) noexcept {
    const auto gain=percent();
    const auto scaled=(static_cast<std::int32_t>(original)*static_cast<std::int32_t>(gain))/100;
    return static_cast<std::int16_t>(std::clamp(scaled,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
}

bool tooie::music::is_jukebox_music_track(std::int16_t track_id) noexcept {
    return std::find(jukebox_tracks.begin(), jukebox_tracks.end(),
        static_cast<std::uint16_t>(track_id)) != jukebox_tracks.end();
}

extern "C" void tooie_music_volume_apply(uint8_t* rdram, recomp_context* ctx) noexcept {
    const auto slot = static_cast<unsigned>(ctx->r4 & 0xFFU);
    if (slot >= 6U) {
        return;
    }
    constexpr std::uint32_t manager_records = 0x801357D0U;
    constexpr std::uint32_t record_size = 0x50U;
    constexpr std::uint32_t track_id_offset = 0x28U;
    const auto track_id = MEM_H(0, guest_address(
        manager_records + slot * record_size + track_id_offset));
    if (!tooie::music::is_jukebox_music_track(track_id)) {
        return;
    }
    ctx->r5=static_cast<gpr>(static_cast<std::int32_t>(tooie::music::scale_sequence_volume(
        static_cast<std::int16_t>(ctx->r5))));
}

extern "C" void tooie_music_volume_tick(uint8_t* rdram, recomp_context*) noexcept {
    const auto requested = requested_generation.load(std::memory_order_acquire);
    if (guest_applied_generation.load(std::memory_order_relaxed) == requested) {
        return;
    }

    // D_801359B0 is the original music-manager refresh byte. Its normal
    // per-frame path clears it and calls func_80017404 with each active
    // player's unscaled cached volume. Touch it only from that guest thread.
    MEM_B(0, guest_address(0x801359B0U)) = 1;
    guest_applied_generation.store(requested, std::memory_order_release);
}
