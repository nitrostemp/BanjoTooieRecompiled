#include "frontend_app.hpp"

#include "continuous_host.hpp"
#include "frontend_session_log.hpp"
#include "game.hpp"
#include "imgui_menu.hpp"
#include "platform_support.hpp"
#include "profile_location.hpp"
#include "runtime_save_root.hpp"
#include "rt64_matrix_trace.hpp"
#include "model_interpolation.hpp"
#include "issue_capture_archive.hpp"
#include "tooie_build_identity.hpp"

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr std::uint64_t tooie_ntsc_u_xxh3 = 0x00F70DE5F2D70EA2ULL;
std::filesystem::path relaunch_profile;
tooie::platform::FrontendLaunch relaunch_mode = tooie::platform::FrontendLaunch::Launcher;
std::shared_ptr<tooie::session_log::Writer> process_exit_log;
bool process_exit_personal_play_used = false;
// Held until process exit; a relaunched child waits for this process to exit.
tooie::profile::Lock profile_lock;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

recomp::GameEntry make_entry() {
    auto entry = tooie::game_entry(tooie_ntsc_u_xxh3, false);
    entry.display_name = "Banjo-Tooie";
    entry.on_init_callback = tooie::continuous_on_init;
    entry.thread_create_callback = tooie::continuous_thread_create_callback;
    return entry;
}
}

std::vector<recomp::GameEntry> supported_games = {make_entry()};

int tooie::frontend::run(const std::filesystem::path& profile_override,
    bool start_game, bool persistent_practice) {
#ifndef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if (persistent_practice)
        throw std::runtime_error("Private save-state practice is unavailable in this build");
#endif
    relaunch_profile.clear();
    relaunch_mode=tooie::platform::FrontendLaunch::Launcher;
    process_exit_log.reset();
    // The continuous host bypasses recomp::start(), which normally publishes
    // the project version for the runtime's launcher state.
    const_cast<recomp::Version&>(recomp::get_project_version()) =
        recomp::Version(tooie::build_identity::major, tooie::build_identity::minor,
            tooie::build_identity::patch, tooie::build_identity::suffix);
    const bool personal_play_used = profile_override.empty();
    auto profile = tooie::profile::resolve(profile_override, tooie::profile::system_environment());
    profile_lock = std::move(profile.lock);
    const auto profile_root = profile.root;
    const auto config_root = profile_root / "config";
    const auto log_root = profile_root / "logs";
    std::filesystem::create_directories(config_root);
    std::filesystem::create_directories(profile_root / "saves");
    std::filesystem::create_directories(log_root);
    // Claim a fresh directory even on rapid process restarts. Capture numbers
    // are local to this directory and never reuse an earlier session's files.
    const auto capture_root = tooie::capture_archive::create_session_directory(log_root);
    RT64::tooieScreenXTraceSetLogDirectory(capture_root);
    tooie::model_interpolation::set_trace_directory(capture_root);
    auto capture_archive = std::make_shared<tooie::capture_archive::Writer>(capture_root);
    auto marker_sequence = std::make_shared<std::atomic_uint64_t>(0);

    auto session_log = std::make_shared<tooie::session_log::Writer>(log_root);
    process_exit_log = session_log;
    process_exit_personal_play_used = personal_play_used;
    std::shared_ptr<tooie::session_log::Writer> graphics_capture_log;
    try {
        graphics_capture_log=std::make_shared<tooie::session_log::Writer>(
            log_root,1U<<20,3,"graphics-captures.jsonl");
    } catch (...) { /* Optional diagnostics do not prevent play. */ }
    const auto log = [session_log,graphics_capture_log,capture_archive,marker_sequence,capture_root](const char* event, const nlohmann::json& fields) {
        session_log->write(event, fields.dump());
        const std::string_view name(event);
        if (name == "issue_marker" || name == "artifact_draw_capture" ||
            name == "artifact_draw_capture_failure" || name == "artifact_branch_capture" ||
            name == "frontend_build_identity" || name == "native_graphics_applied") {
            try {
                auto saved_fields = fields;
                if (name == "issue_marker") {
                    saved_fields["marker_sequence"] = marker_sequence->fetch_add(1) + 1;
                    saved_fields["capture_directory"] = utf8(capture_root);
                }
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                capture_archive->append(nlohmann::json{{"timestamp_unix_ms", now},
                    {"event", event}, {"fields", std::move(saved_fields)}}.dump());
            } catch (...) { /* Optional capture archive must not interrupt play. */ }
        }
        if (name == "frontend_exit" || name == "frontend_failure" ||
            name == "issue_marker") session_log->flush();
        if (graphics_capture_log &&
            (name=="artifact_draw_capture" || name=="artifact_draw_capture_failure" ||
             name=="artifact_branch_capture" || name=="issue_marker")) {
            try {
                graphics_capture_log->write(event,fields.dump());
                graphics_capture_log->flush();
            } catch (...) {}
        }
    };
    log("frontend_profile", {{"root", utf8(profile_root)},
        {"config", utf8(config_root)}, {"saves", utf8(profile_root / "saves")},
        {"personal_play_used", personal_play_used},
        {"origin", tooie::profile::origin_name(profile.origin)},
        {"legacy_present", !profile.legacy.empty()},
        {"migrated_files", profile.files}, {"migrated_bytes", profile.bytes},
        {"persistent_practice", persistent_practice}});
    const auto executable = tooie::platform::executable_path();
    const nlohmann::json identity={{"version", tooie::build_identity::version},
        {"revision_at_configure", tooie::build_identity::revision_at_configure},
        {"git_state_at_configure", tooie::build_identity::git_state_at_configure},
        {"executable", utf8(executable)},
        {"executable_sha256", tooie::platform::file_sha256(executable)}};
    log("frontend_build_identity",identity);
    if (graphics_capture_log) {
        try {
            graphics_capture_log->write("frontend_build_identity",identity.dump());
            graphics_capture_log->flush();
        } catch (...) {}
    }

    recomp::register_config_path(config_root);
    tooie::register_save_root(profile_root / "saves");
    if (!recomp::register_game(supported_games.front()))
        throw std::runtime_error("Could not register the Banjo-Tooie NTSC-U game entry");
    recomp::check_all_stored_roms();
    recomp::mods::initialize_mods();
    tooie::menu::initialize(profile_root, log, start_game, persistent_practice);
    if (profile.origin == tooie::profile::Origin::Migrated) {
        tooie::menu::show_notice("Profile Folder Updated",
            "Your saves, settings and controls were copied to " + utf8(profile_root) +
            ". The original folder was left unchanged at " + utf8(profile.legacy) +
            ". A practice state saved by an earlier version may not load in this version; "
            "if it does not, start a new practice session and save a fresh state.");
    }
    int result = 3;
    try {
        const bool completed = run_continuous_host({profile_root,
            std::chrono::milliseconds(0), true, 0, false, false, true,
            persistent_practice, log});
        result = completed ? 0 : 3;
    } catch (const std::exception& error) {
        log("frontend_failure", {{"reason", error.what()}});
        tooie::menu::shutdown();
        throw;
    } catch (...) {
        log("frontend_failure", {{"reason", "unknown exception"}});
        tooie::menu::shutdown();
        throw;
    }
    if (!tooie::menu::shutdown()) {
        log("frontend_failure", {{"reason", "settings flush failed during shutdown"}});
        result=3;
    }
    if (result==0) {
        const auto requested=tooie::menu::relaunch_requested();
        if (requested!=tooie::menu::RelaunchMode::None) {
            relaunch_profile=profile_root;
            relaunch_mode=requested==tooie::menu::RelaunchMode::PracticeGame
                ? tooie::platform::FrontendLaunch::PracticeGame
                : requested==tooie::menu::RelaunchMode::RestartGame
                    ? tooie::platform::FrontendLaunch::RestartGame
                    : tooie::platform::FrontendLaunch::Launcher;
        }
    }
    return result;
}

void tooie::frontend::relaunch_if_requested() {
    auto profile=std::exchange(relaunch_profile,{});
    const auto mode=std::exchange(relaunch_mode,tooie::platform::FrontendLaunch::Launcher);
    if (!profile.empty()) tooie::platform::launch_frontend(profile,mode);
}

void tooie::frontend::record_process_exit(int result) noexcept {
    auto output=std::exchange(process_exit_log,{});
    if (!output) return;
    try {
        output->write("frontend_exit",nlohmann::json({{"result",result},{"completed",result==0},
            {"personal_play_used",process_exit_personal_play_used}}).dump());
        output->flush();
    } catch (...) {}
}
