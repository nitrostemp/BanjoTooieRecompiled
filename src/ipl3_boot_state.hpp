#pragma once
#include <cstdint>
#include <span>

namespace tooie::boot {
struct ScatterReport {
    uint32_t rom_source, rdram_destination, blocks, block_bytes, rdram_stride;
};
// Pre-entry hardware effect, applied once before any guest worker. Source is
// the already identity-verified original ROM, never decoded reference bytes.
// Invalid input is rejected before any guest-memory write.
ScatterReport apply_ipl3_scatter(std::span<const uint8_t> rom,
                               std::span<uint8_t> word_swapped_rdram);
}
