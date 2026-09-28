#pragma once
#include "persistent_state_continuation.hpp"
#include "persistent_state_devices.hpp"
#include "si_adapter.hpp"
#include "persistent_state_frontend.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Pointer-free-on-disk whole-machine schema shared by the experimental codec
// and scheduler. No native thread handles, C stacks, function addresses or host
// f_odd pointers may be encoded. The codec serializes recomp_context fields,
// then rebinds f_odd to the destination context when installing it.
namespace tooie::persistent_state {
constexpr std::uint32_t snapshot_schema=1;
constexpr std::uint32_t isolated_eeprom_policy=1;
struct Compatibility {
    std::string program_sha256;
    std::string executable_sha256;
    std::string rom_sha256;
    std::string runtime_revision;
    std::string settings_sha256;
};
enum class ThreadPhase : std::uint32_t { SchedulerWait=1, ExternalWait=2 };
struct ThreadImage {
    std::uint32_t address=0;
    std::uint64_t entry=0,initial_sp=0,initial_arg=0;
    ThreadPhase phase=ThreadPhase::SchedulerWait;
    recomp_context context{};
    std::vector<continuation::Frame> frames;
    std::vector<std::uint32_t> overlay_callsites;
    std::uint32_t replay_active_divisor=0;
    std::uint32_t cop1_rounding_mode=0; // Symbolic N64 0..3, never FE_* constants.
};
struct OverlayImage {
    std::uint32_t id=0,header=0,text=0,active=0;
    std::uint64_t generation=0;
};
struct OverlayGeneration {std::uint32_t id=0;std::uint64_t generation=0;};
struct ExternalMessage {std::int32_t queue=0,message=0;bool jam=false,requeue_if_blocked=false;};
struct Snapshot {
    std::uint32_t schema=snapshot_schema;
    std::uint32_t save_medium_policy=isolated_eeprom_policy;
    Compatibility compatibility;
    std::uint64_t epoch=0;
    // Guest OSThread::context fields are zeroed in this copy. Import replaces
    // them with newly owned UltraThreadContext pointers before guest execution.
    std::vector<std::uint8_t> rdram;
    std::vector<ThreadImage> threads;
    std::int32_t running_queue_head=0;
    std::uint32_t external_wait_thread=0;
    std::uint32_t boot_fcsr=0;
    std::vector<ExternalMessage> external_messages;
    std::vector<OverlayImage> overlays;
    std::vector<OverlayGeneration> overlay_generations;
    devices::Snapshot devices;
    si::PersistentState si;
    std::array<std::uint8_t,4096> rsp_dmem{};
    std::uint32_t replay_pending_divisor=0,replay_published_refresh_rate=0;
    bool scene_map_available=false;
    std::uint16_t scene_map_id=0;
    frontend::HostState host;
};
}
