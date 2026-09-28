#include "frontend_cheats.hpp"

#include "artifact_capture.hpp"
#include "free_camera.hpp"
#include "frontend_input_preference.hpp"
#include "frontend_config.hpp"
#include "game_features.hpp"
#include "imgui_menu.hpp"

#include "imgui/imgui.h"
#include "ultramodern/ultramodern.hpp"

#include <array>
#include <functional>
#include <string>

namespace {
using namespace tooie::features;
struct CheatItem { Cheat cheat; const char* name; const char* description; };
constexpr std::array cheat_items{
    CheatItem{Cheat::Feathers,"Double Feather Capacity","Carry twice as many red and gold feathers."},
    CheatItem{Cheat::Eggs,"Double Egg Capacity","Carry twice as many eggs of unlocked types."},
    CheatItem{Cheat::Fallproof,"Fallproof","Prevent damage from long falls."},
    CheatItem{Cheat::Honeyback,"Honeyback","Restore missing health over time."},
    CheatItem{Cheat::Jukebox,"Jukebox","Unlock the music jukebox in Jolly's tavern."},
    CheatItem{Cheat::GetJiggy,"Get Jiggy","Decipher Jiggywiggy Temple hint signposts."},
    CheatItem{Cheat::SuperBanjo,"Super Banjo","Increase player movement speed."},
    CheatItem{Cheat::SuperBaddy,"Super Baddy","Increase enemy movement speed."},
    CheatItem{Cheat::HoneyKing,"Honey King","Unlimited health and air."},
    CheatItem{Cheat::NestKing,"Infinite Eggs and Feathers","Use ammo without running out."},
    CheatItem{Cheat::JiggywiggySpecial,"Jiggywiggy Special","Unlock every world entrance."},
    CheatItem{Cheat::Homing,"Homing","Make eggs home toward nearby enemies."},
};
constexpr std::array<const char*, 29> move_names{
    "Grip Grab","Breegull Blaster","Egg Aim","Bill Drill","Beak Bayonet",
    "Airborne Egg Aim","Split Up","Wing Whack","Talon Torpedo","Sub-Aqua Egg Aim",
    "T-Rex Roar","Shack Pack","Glide","Snooze Pack","Leg Spring",
    "Claw Clamber Boots","Springy Step Shoes","Taxi Pack","Hatch","Pack Whack",
    "Sack Pack","Amaze-O-Gaze Goggles","Fire Eggs","Grenade Eggs",
    "Clockwork Kazooie Eggs","Ice Eggs","Fast Swimming","Blue Eggs","Breegull Bash"};
constexpr std::array<const char*, 9> world_names{
    "Mayahem Temple","Glitter Gulch Mine","Witchyworld","Jolly Roger's Lagoon",
    "Terrydactyland","Grunty Industries","Hailfire Peaks","Cloud Cuckooland","Cauldron Keep"};
constexpr std::array<const char*, 6> station_names{
    "Witchyworld","Terrydactyland","Grunty Industries","Hailfire Peaks (Lava)",
    "Hailfire Peaks (Ice)","Isle o' Hags - Cliff Top"};
constexpr std::array<const char*, 13> boss_names{
    "Klungo 1","Klungo 2","Klungo 3","Targitzan","Old King Coal","Mr. Patch",
    "Lord Woo Fak Fak","Terry","Weldar","Chilly Willy","Chilli Billi","Mingy Jongo","Hag 1"};

std::function<bool()> confirm_action;
std::string confirm_title;
bool confirm_is_cheat = false;
struct DisplayedRequest {
    std::string label;
    std::uint64_t sequence = 0;
    std::uint64_t file_generation = 0;
    bool cheat = false;
    bool unavailable = false;
};
DisplayedRequest displayed_request;
bool confirmation_requested=false;
bool access_confirmation_requested=false;

void record_request(const std::string& label, bool accepted, bool cheat) {
    if (cheat) {
        const auto readout = tooie::features::cheat_readout();
        displayed_request = {label, accepted ? readout.receipt.sequence : 0,
            readout.generation, true, !accepted};
    } else {
        const auto readout = tooie::features::progression_readout();
        displayed_request = {label, accepted ? readout.receipt.sequence : 0,
            readout.generation, false, !accepted};
    }
}

void action(const char* label, bool done, bool permanent, std::function<bool()> request,
    const char* explanation = nullptr) {
    ImGui::PushID(label);
    if (permanent && done) {
        ImGui::TextDisabled("Unlocked: %s", label);
    } else {
        const std::string button = std::string(permanent ? "Unlock " : "Use ") + label;
        if (ImGui::Button(button.c_str())) {
            if (permanent) {
                confirm_action = std::move(request);
                confirm_title = label;
                confirm_is_cheat = false;
                confirmation_requested=true;
            } else {
                record_request(label, request(), false);
            }
        }
    }
    if (explanation) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.66f,.67f,.67f,1));
        ImGui::TextWrapped("%s", explanation);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

void confirmation_dialog() {
    if (confirmation_requested) {
        ImGui::OpenPopup("Confirm permanent action");
        confirmation_requested=false;
    }
    const auto screen=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x*.5f,screen.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(540.0f,screen.x-30.0f),0),ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Confirm permanent action", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Apply %s in the current game file? The game will report when it applies the request. Save normally in game to keep permanent changes.", confirm_title.c_str());
        if (ImGui::Button("Unlock", ImVec2(115,0))) {
            record_request(confirm_title, confirm_action && confirm_action(), confirm_is_cheat);
            confirm_action = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(115,0))) {
            confirm_action = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void readback(const ProgressionReadout& p, const CheatReadout& c) {
    const auto generation = displayed_request.cheat ? c.generation : p.generation;
    const auto& receipt = displayed_request.cheat ? c.receipt : p.receipt;
    if (displayed_request.file_generation != generation ||
        (!displayed_request.unavailable && displayed_request.sequence != receipt.sequence))
        displayed_request = {};
    if (!displayed_request.label.empty()) {
        if (displayed_request.unavailable) {
            ImGui::TextWrapped("Current game file - %s: unavailable", displayed_request.label.c_str());
        } else {
            const char* outcome = "Unknown";
            switch (receipt.outcome) {
            case RequestOutcome::Queued: outcome = "Queued"; break;
            case RequestOutcome::Applied: outcome = "Applied"; break;
            case RequestOutcome::Unchanged: outcome = "Already complete"; break;
            case RequestOutcome::Rejected: outcome = "Rejected"; break;
            case RequestOutcome::Failed: outcome = "Failed"; break;
            case RequestOutcome::Canceled: outcome = "Canceled"; break;
            default: break;
            }
            ImGui::TextWrapped("Current game file - %s: %s", displayed_request.label.c_str(), outcome);
        }
    }
    if (p.request_pending || c.request_pending)
        ImGui::TextWrapped("A game request is pending. Close Settings to let the game apply it.");
}
}

namespace tooie::frontend::cheats {
void initialize() {
    displayed_request = {};
    features::set_cheats_access_enabled(menu::get_bool("tooie_enable_cheats", false));
    features::request_cheat_refresh();
    features::request_progression_refresh();
    camera::set_free_camera_enabled(menu::get_bool("tooie_detached_free_camera", false));
    camera::set_free_camera_speed(float(menu::get_int("tooie_free_camera_speed", 600)));
}

bool revoke_access_from_launcher() {
    features::set_cheats_access_enabled(false);
    menu::set_bool("tooie_enable_cheats", false);
    return settings::flush();
}

void trigger_graphics_issue_marker() noexcept {
    artifact_capture::request();
    input::post_issue_marker();
}

void draw_free_camera() {
    bool enabled = camera::free_camera_enabled();
    if (ImGui::Checkbox("Detached Free Camera", &enabled)) {
        camera::set_free_camera_enabled(enabled);
        menu::set_bool("tooie_detached_free_camera", enabled);
    }
    ImGui::TextWrapped("Offsets only the view. Player movement and collision stay game owned.");
    int speed = menu::get_int("tooie_free_camera_speed", 600);
    if (ImGui::SliderInt("Camera Speed", &speed, 150, 1200)) {
        camera::set_free_camera_speed(float(speed));
        menu::set_int("tooie_free_camera_speed", speed);
    }
    if (ImGui::Button("Recenter Camera")) camera::request_free_camera_reset();
}

void draw_access_gate() {
    bool access = features::cheats_access_enabled();
    if (!access) {
        ImGui::TextWrapped("Cheat controls are hidden for this profile. Enable access to show them; enabling alone changes no save data.");
        if (ImGui::Button("Enable Cheats")) access_confirmation_requested=true;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.66f,.67f,.67f,1));
        ImGui::TextWrapped("Cheats enabled. To turn off active effects, restart and use the launcher reset for one game file.");
        ImGui::PopStyleColor();
    }
    if (access_confirmation_requested) {
        ImGui::OpenPopup("Enable cheat access");
        access_confirmation_requested=false;
    }
    const auto screen=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x*.5f,screen.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(540.0f,screen.x-30.0f),0),ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Enable cheat access",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Show cheat tools for this profile? Enabling access alone changes no game file. Permanent unlocks remain after turning off active effects. To turn off active effects later, restart and use the launcher reset for a selected file.");
        if (ImGui::Button("Enable",ImVec2(110,0))) {
            features::set_cheats_access_enabled(true);
            menu::set_bool("tooie_enable_cheats",true);
            if (settings::flush()) access=true;
            else {
                features::set_cheats_access_enabled(false);
                menu::set_bool("tooie_enable_cheats",false);
                menu::show_notice("Cheats Remain Disabled",
                    "Could not save cheat access for this profile. Check the config folder and try again.",false);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void draw_tools(int category) {
    if (!features::cheats_access_enabled()) return;
    const auto c = features::cheat_readout();
    const auto p = features::progression_readout();
    if (!ultramodern::is_game_started() || !p.current || !p.observed || !c.current || !c.observed) {
        ImGui::TextWrapped("Start a game to use Tools. Actions apply only to the current game file.");
        return;
    }
    readback(p,c);
    ImGui::Separator();
    if (p.request_pending || c.request_pending) ImGui::BeginDisabled();
    switch (category) {
    case 0: {
        ImGui::TextUnformatted("Reversible effects");
        for (std::size_t i = 0; i < cheat_items.size(); ++i) {
            auto item = cheat_items[i];
            const auto status = c.cheats[i];
            bool active = status.active;
            ImGui::PushID(int(i));
            if (status.available) {
                if (ImGui::Checkbox(item.name, &active)) {
                    record_request(item.name, features::request_cheat_enabled(item.cheat, active), true);
                }
            } else if (ImGui::Button((std::string("Unlock & Enable ") + item.name).c_str())) {
                confirm_title = item.name;
                confirm_action = [cheat=item.cheat] { return features::request_unlock_and_enable_cheat(cheat); };
                confirm_is_cheat = true;
                confirmation_requested=true;
            }
            ImGui::SameLine(); ImGui::TextWrapped("%s", item.description);
            ImGui::PopID();
        }
        break;
    }
    case 1: {
        ImGui::TextUnformatted("Travel unlocks");
        action("All Train Platforms", false, true, [] { return features::request_unlock_all_train_stations(); },
            "Station switches only; the train still needs repair.");
        for (std::size_t i=0;i<station_names.size();++i)
            action(station_names[i], p.train_stations_unlocked[i], true,
                [i] { return features::request_unlock_train_station(features::kTrainStations[i]); });
        action("All Silos", p.silos_unlocked >= p.silo_total, true, [] { return features::request_unlock_all_silos(); });
        action("All Warp Pads", false, true, [] { return features::request_activate_all_warp_pads(); });
        for (std::size_t i=0;i<world_names.size();++i)
            action(world_names[i], p.warp_pads_active[i] >= p.warp_pad_totals[i], true,
                [i] { return features::request_activate_world_warp_pads(features::kWorlds[i]); });
        break;
    }
    case 2: {
        action("All Moves", false, true, [] { return features::request_unlock_all_moves(); });
        for (std::size_t i=0;i<move_names.size();++i)
            action(move_names[i], p.moves[i], true,
                [i] { return features::request_unlock_move(features::kMoves[i]); });
        break;
    }
    case 3: {
        action("All World Entrances", false, true, [] { return features::request_unlock_all_worlds(); });
        action("All Hub Connections", p.hub_connections_open >= p.hub_connection_total,
            true, [] { return features::request_unlock_all_hub_connections(); });
        action("Grunty Industries Front Door", p.gi_front_door_open,
            true, [] { return features::request_open_gi_front_door(); });
        for (std::size_t i=0;i<world_names.size();++i)
            action(world_names[i], p.worlds[i], true,
                [i] { return features::request_unlock_world(features::kWorlds[i]); });
        break;
    }
    case 4: {
        ImGui::Text("Notes: %u / %u",p.notes,p.note_total);
        action("All Notes",p.notes>=p.note_total,true,[] { return features::request_collect_all_notes(); });
        ImGui::Text("Jiggies: %u / %u",p.jiggies,p.jiggy_total);
        action("All Jiggies",p.jiggies>=p.jiggy_total,true,[] { return features::request_collect_all_jiggies(); });
        ImGui::Text("Honeycombs: %u / %u",p.honeycombs,p.honeycomb_total);
        action("All Honeycombs",p.honeycombs>=p.honeycomb_total,true,[] { return features::request_collect_all_honeycombs(); });
        ImGui::Text("Jinjos: %u / %u",p.jinjos,p.jinjo_total);
        action("All Jinjos",p.jinjos>=p.jinjo_total,true,[] { return features::request_collect_all_jinjos(); });
        break;
    }
    case 5: {
        ImGui::Text("Health: %u / %u",p.current_health,p.health_capacity);
        action("Upgrade Max Health",p.health_capacity>=p.max_health_capacity,
            true,[] { return features::request_upgrade_max_health(); });
        action("Refill Health",false,false,[] { return features::request_refill_health(); });
        ImGui::Text("Selected egg ammo: %u / %u",p.selected_egg_ammo,p.selected_egg_capacity);
        action("Refill Selected Eggs",false,false,[] { return features::request_refill_selected_eggs(); });
        bool invincible=p.invincible;
        if (ImGui::Checkbox("Invincibility",&invincible))
            record_request("Invincibility", features::request_invincibility_enabled(invincible), false);
        break;
    }
    case 6: {
        ImGui::TextWrapped("Only independently verified boss flags can be changed. A boss's battle and reward may need other story state.");
        for (std::size_t i=0;i<boss_names.size();++i) {
            const auto support=features::boss_support(features::kBosses[i]);
            ImGui::PushID(int(i));
            if (!p.boss_edits_available[i]) {
                ImGui::TextDisabled("%s: unavailable in current area",boss_names[i]);
            } else if (!p.bosses[i]) {
                if (support.can_mark_defeated)
                    action(boss_names[i],false,true,[i] { return features::request_boss_defeated(features::kBosses[i],true); });
                else ImGui::TextDisabled("%s: %s",boss_names[i],support.mark_reason);
            } else {
                ImGui::Text("%s: defeated",boss_names[i]);
                if (support.can_reset)
                    action((std::string("Restore encounter: ")+boss_names[i]).c_str(),false,true,
                        [i] { return features::request_boss_defeated(features::kBosses[i],false); });
                else ImGui::TextDisabled("%s",support.reset_reason);
            }
            ImGui::PopID();
        }
        break;
    }
    }
    if (p.request_pending || c.request_pending) ImGui::EndDisabled();
    confirmation_dialog();
}
}
