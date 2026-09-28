#pragma once
#include <filesystem>
#include <cstdint>
#include <string>
namespace tooie::persistent_state::controller {
enum class Action { Capture, Restore };
struct Status {
    bool practice_enabled=false;
    bool busy=false;
    uint64_t completed=0;
    std::string action;
    std::string outcome;
    std::string message;
    std::filesystem::path path;
};
// Resolve only before runtime preinit. Frontend sessions require the explicit
// launch option so their UI label and save medium cannot disagree. Environment
// engineering opt-ins are accepted only for nonfrontend diagnostic execution.
bool practice_requested(bool launch_option,bool frontend) noexcept;
// UI-safe: queues bounded work for poll(), never freezes from the UI thread.
bool request(Action,const std::filesystem::path&);
Status status();
void initialize(const std::filesystem::path& profile,bool frontend);
void poll();
}
