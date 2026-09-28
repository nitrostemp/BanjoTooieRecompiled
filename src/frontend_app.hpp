#pragma once

#include <filesystem>

namespace tooie::frontend {
int run(const std::filesystem::path& profile_override, bool start_game = false,
    bool persistent_practice = false);
// Called only after successful runtime, logging, trace and watchdog shutdown.
// Consumes the request before launch so it cannot start duplicate children.
void relaunch_if_requested();
// Records the final process result after watchdog/trace cleanup and any child
// launch attempt, so a persisted successful exit cannot precede later failure.
void record_process_exit(int result) noexcept;
}
