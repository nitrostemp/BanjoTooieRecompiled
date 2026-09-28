#pragma once

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include <cstdint>
#include <vector>

namespace RT64 { struct State; struct WorkloadQueue; struct PresentQueue; }

namespace tooie::native_host::persistent_bridge {

// This image contains typed semantic RDP/VI values, never live RT64 pointers,
// synchronization primitives, GPU resources, or queue cursors. NativeRenderer
// resets RSP before every guest task, so no RSP stack/vertex state spans the
// admitted full-sync boundary. Native layout is bound to executable identity.
constexpr std::uint32_t renderer_image_version = 2;

// Admission requires a complete full-sync boundary, not just idle workers.
const char* clean_boundary_reason(const RT64::State& state,
    const RT64::WorkloadQueue& workloads) noexcept;
bool export_image(const RT64::State& state,
    std::vector<std::uint8_t>& output) noexcept;
bool valid_image_blob(const std::vector<std::uint8_t>& input) noexcept;
bool import_image(RT64::State& state,
    const std::vector<std::uint8_t>& input) noexcept;

} // namespace tooie::native_host::persistent_bridge
#endif
