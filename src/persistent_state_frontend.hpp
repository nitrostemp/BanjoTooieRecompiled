#pragma once

#include <cstdint>
#include <string>

// Host-owned semantic state not contained in guest RAM or the device image.
// These hooks are compiled and called only by the opt-in persistent runtime.
namespace tooie::persistent_state::frontend {

struct HostState {
    std::uint32_t dp_status = 0x80;
    bool cutscene_active = false;
};

// Both calls are pure with respect to guest RAM. capture_frozen requires the
// guest/device lease; validate_restore runs before any native stack unwind.
bool capture_frozen(HostState& output, std::string& reason) noexcept;
bool validate_restore(const HostState& input, std::string& reason) noexcept;

// Called after restored RDRAM is installed but before producers resume.
// Clears newer-timeline transient native state without touching guest RAM.
void restore_epoch(const HostState& input) noexcept;

} // namespace tooie::persistent_state::frontend
