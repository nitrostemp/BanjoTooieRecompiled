#pragma once

#include <cstdint>

namespace tooie::scene {

enum class ActivationPhase : std::uint8_t {
    SectionShutdown,
    LevelShutdown,
    LevelStartup,
    SectionStartup,
    WorldSetup,
    Count,
};

struct Snapshot {
    bool map_available = false;
    std::uint16_t map_id = 0;
    bool activation_active = false;
    std::uint64_t activations_completed = 0;
    double last_activation_ms = 0.0;
    double max_activation_ms = 0.0;
    // Each value measures just the named original call during the latest
    // activation; zero means that branch did not execute.
    double section_shutdown_ms = 0.0;
    double level_shutdown_ms = 0.0;
    double level_startup_ms = 0.0;
    double section_startup_ms = 0.0;
    double world_setup_ms = 0.0;
    std::uint32_t world_slowest_call_pc = 0;
    double world_slowest_call_ms = 0.0;
    // Immediate children of the two dominant world-setup parents. These are
    // recorded only while an activation is active and retain only one winner.
    std::uint32_t setup_a5c28_slowest_call_pc = 0;
    double setup_a5c28_slowest_call_ms = 0.0;
    std::uint32_t setup_a5c28_observed_calls = 0;
    std::uint32_t setup_f73c4_slowest_call_pc = 0;
    double setup_f73c4_slowest_call_ms = 0.0;
    std::uint32_t setup_f73c4_observed_calls = 0;
};

void observe_map(std::uint16_t map_id) noexcept;
void activation_started() noexcept;
void activation_finished() noexcept;
void activation_phase_started(ActivationPhase phase) noexcept;
void activation_phase_finished(ActivationPhase phase) noexcept;
void world_call_started(std::uint32_t call_pc) noexcept;
void world_call_finished(std::uint32_t call_pc) noexcept;
void nested_call_started(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept;
void nested_call_finished(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept;
Snapshot snapshot() noexcept;
void reset() noexcept;

} // namespace tooie::scene

extern "C" void tooie_scene_observe_map(std::uint16_t map_id) noexcept;
extern "C" void tooie_scene_activation_started() noexcept;
extern "C" void tooie_scene_activation_finished() noexcept;
extern "C" void tooie_scene_activation_phase_started(std::uint32_t phase) noexcept;
extern "C" void tooie_scene_activation_phase_finished(std::uint32_t phase) noexcept;
extern "C" void tooie_scene_world_call_started(std::uint32_t call_pc) noexcept;
extern "C" void tooie_scene_world_call_finished(std::uint32_t call_pc) noexcept;
extern "C" void tooie_scene_nested_call_started(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept;
extern "C" void tooie_scene_nested_call_finished(std::uint32_t parent_pc, std::uint32_t child_pc) noexcept;
