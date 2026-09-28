#pragma once
#include "recomp.h"
#include "trace.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
namespace tooie {
struct ContinuousOptions {
    std::filesystem::path runtime_directory;
    std::chrono::milliseconds observation_window{30000};
    bool native_window=false;
    unsigned audio_capture_seconds=0;
    bool audio_pacing_observation=false;
    bool sdl_audio_observation=false;
    bool frontend=false;
    // Explicit session choice, fixed before any normal progress-save access.
    bool persistent_practice=false;
    std::function<void(const char*, const Json&)> frontend_log;
};
// Caller registers continuous_game_entry rather than the bounded game entry.
bool run_continuous_host(const ContinuousOptions& options);
bool continuous_enabled() noexcept;
// True only for the ordinary embedded frontend player. Diagnostic continuous
// runs retain their immutable developer oracle; player runs derive guest
// images from the user's validated ROM and do not require that oracle.
bool continuous_frontend_mode() noexcept;
Json graphics_observation_summary();
Json title_observation_summary();
Json map_actor_list_summary();
Json menu_observation_summary();
Json sfx_wait_observation_summary();
void continuous_poll();
void continuous_thread_create_callback(uint8_t*,recomp_context*);
void continuous_core1_ready(uint8_t*,recomp_context*);
// on_init callback: validates original initial DMA, then prepares observer.
void continuous_on_init(uint8_t*,recomp_context*);
}
extern "C" void tooie_continuous_poll(uint8_t*,recomp_context*,uint32_t pc);
