#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <json/json.hpp>

union SDL_Event;

namespace tooie::menu {
using Log = std::function<void(const char*, const nlohmann::json&)>;
enum class RelaunchMode { None, RestartGame, ReturnToLauncher, PracticeGame };
void initialize(const std::filesystem::path& profile_root, Log log,
    bool start_game = false, bool persistent_practice = false);
// SDL thread only; read after runtime shutdown to arrange a fresh process.
RelaunchMode relaunch_requested() noexcept;
// Returns false when profile settings could not be flushed. Other menu-owned
// resources are still released before returning.
bool shutdown();
// SDL owner thread: perform queued ROM and save actions, and persist settings.
void tick();
void handle_event(const SDL_Event& event);
// RT64 present thread: draws menu and passive overlays; never calls SDL APIs.
void draw();
bool is_open() noexcept;
void set_open(bool open) noexcept;
void show_notice(std::string title, std::string text, bool success = true,
    double auto_close_seconds = 0.0);
bool get_bool(std::string_view id, bool fallback = false);
int get_int(std::string_view id, int fallback = 0);
double get_number(std::string_view id, double fallback = 0.0);
std::string get_string(std::string_view id, std::string_view fallback = {});
void set_bool(std::string_view id, bool value);
void set_int(std::string_view id, int value);
void set_number(std::string_view id, double value);
void set_string(std::string_view id, std::string value);
}
