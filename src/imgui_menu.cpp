#include "imgui_menu.hpp"

#include "cheat_save_reset.hpp"
#include "draw_distance.hpp"
#include "frontend_cheats.hpp"
#include "frontend_config.hpp"
#include "frontend_diagnostics_overlay.hpp"
#include "frontend_prompt.hpp"
#include "practice_state.hpp"
#include "practice_travel.hpp"
#include "practice_forms.hpp"
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "persistent_state_controller.hpp"
#endif
#include "frontend_input_preference.hpp"
#include "frontend_settings.hpp"
#include "game_features.hpp"
#include "hud_layout.hpp"
#include "minimap.hpp"
#include "music_volume.hpp"
#include "native_host_devices.hpp"
#include "tooie_build_identity.hpp"
#include "virtual_clock.hpp"
#include "widescreen.hpp"
#include "camera_analog.hpp"
#include "camera_interpolation.hpp"

#include "librecomp/game.hpp"
#include "imgui/imgui.h"
#include "SDL.h"
#include "ultramodern/ultramodern.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {
using tooie::menu::Log;
std::filesystem::path profile_root;
Log logger;
std::atomic_bool menu_open{true};
std::atomic_bool notice_active{false};
std::atomic_bool rom_ready{false};
std::mutex queue_mutex;
enum class TaskKind { Browse, Import, Start, Reset, Restart, ReturnToLauncher, StartPractice, Quit };
tooie::menu::RelaunchMode relaunch_mode = tooie::menu::RelaunchMode::None;
bool practice_process = false;
struct Task { TaskKind kind; std::filesystem::path path; int slot = 0; };
std::deque<Task> tasks;
struct Notice { std::string title, text; bool success = true; double seconds = 0.0; };
std::deque<Notice> notices;
bool notice_open = false;
std::chrono::steady_clock::time_point notice_deadline{};

enum class Page { Home, General, Graphics, Controls, Sound, Tools, About };
Page page = Page::Home;
int general_category = 0;
int graphics_category = 0;
int tools_category = 0;
char rom_path[1024]{};
char controller_friendly_name[96]{};
char controller_profile_name[96]{};
std::string edited_controller_id;
std::string edited_profile_name;
std::filesystem::path browse_directory;
bool browse_open = false;
int pending_reset_slot = 0;
bool pending_quit=false;
bool pending_restart=false;
bool pending_return=false;
bool pending_practice=false;
bool pending_save_state=false;
bool pending_load_state=false;
bool manual_rom_path=false;
char search_text[96]{};
float ui_scale = 1.0f;

void prepare_modal(float width=540.0f) {
    const auto screen=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x*.5f,screen.y*.5f),
        ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width*ui_scale,screen.x-30.0f),0),
        ImGuiCond_Always);
}

struct SearchEntry { const char* label; const char* terms; Page page; int category; };
constexpr std::array search_entries{
    SearchEntry{"Analog Camera","right stick invert horizontal vertical",Page::General,1},
    SearchEntry{"Detached Free Camera","recenter speed practice",Page::General,1},
    SearchEntry{"Preferred Controller","gamepad input device",Page::General,0},
    SearchEntry{"Stick Deadzone","controller input",Page::General,0},
    SearchEntry{"Rumble Strength","controller vibration",Page::General,0},
    SearchEntry{"Fast Forward","speed hold",Page::General,2},
    SearchEntry{"Diagnostics Overlay","fps frame rate performance",Page::General,3},
    SearchEntry{"Mark Graphics Issue","F4 log capture",Page::General,3},
    SearchEntry{"Render Output Scale","resolution pixels internal",Page::Graphics,0},
    SearchEntry{"Downsampling","resolution supersampling quality",Page::Graphics,0},
    SearchEntry{"MS Anti-Aliasing","AA MSAA edges",Page::Graphics,0},
    SearchEntry{"Actor Draw Distance","scenery render",Page::Graphics,0},
    SearchEntry{"Game Widescreen","aspect 4:3 16:9",Page::Graphics,1},
    SearchEntry{"HUD Counter Placement","widescreen framing",Page::Graphics,1},
    SearchEntry{"HUD Proportions","widescreen original stretch",Page::Graphics,1},
    SearchEntry{"Cutscene Aspect and Motion","cinematic original",Page::Graphics,1},
    SearchEntry{"Pacing Comparison","smooth console vsync output rate",Page::Graphics,2},
    SearchEntry{"Output Display and Resolution","window monitor fullscreen 3440",Page::Graphics,3},
    SearchEntry{"Bindings","keyboard controller remap",Page::Controls,0},
    SearchEntry{"Main Volume","audio sound mute",Page::Sound,0},
    SearchEntry{"Jukebox Music Volume","sound tracks",Page::Sound,0},
    SearchEntry{"Reversible Cheats","feathers eggs fallproof honeyback homing",Page::Tools,0},
    SearchEntry{"Travel Unlocks","train station platform silo warp pad cliff top",Page::Tools,1},
    SearchEntry{"Moves and Egg Types","learn abilities",Page::Tools,2},
    SearchEntry{"World Entrances","jiggywiggy grunty industries door",Page::Tools,3},
    SearchEntry{"Collectibles","notes jiggies honeycombs jinjos",Page::Tools,4},
    SearchEntry{"Player Actions","health eggs refill invincibility",Page::Tools,5},
    SearchEntry{"Boss State","defeated restore encounter",Page::Tools,6},
    SearchEntry{"Practice Alignment","mark clear target overlay",Page::Tools,7},
};

std::string lowercase(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),
        [](unsigned char ch){ return char(std::tolower(ch)); });
    return value;
}

void draw_search_results() {
    const auto needle=lowercase(search_text);
    if (needle.empty()) return;
    ImGui::Text("Search results for '%s'",search_text);
    ImGui::Separator();
    bool any=false;
    for (const auto& entry:search_entries) {
        if (lowercase(std::string(entry.label)+" "+entry.terms).find(needle)==std::string::npos) continue;
        any=true;
        if (ImGui::Selectable(entry.label)) {
            page=entry.page;
            if (page==Page::General) general_category=entry.category;
            if (page==Page::Graphics) graphics_category=entry.category;
            if (page==Page::Tools) tools_category=entry.category;
            search_text[0]='\0';
        }
    }
    if (!any) ImGui::TextDisabled("No matching settings. Try resolution, train, AA, or controller.");
}

void enqueue(Task task) { std::lock_guard lock(queue_mutex); tasks.push_back(std::move(task)); }

int enum_setting(const char* id, int fallback, std::initializer_list<const char*> names) {
    const std::string raw = tooie::menu::get_string(id);
    if (!raw.empty()) {
        int index = 0;
        for (const char* name : names) {
            if (raw == name) return index;
            ++index;
        }
        try { return std::stoi(raw); } catch (...) {}
    }
    return tooie::menu::get_int(id, fallback);
}

bool choose(const char* label, const char* id, int& value,
    std::initializer_list<const char*> labels, std::initializer_list<int> stored = {}) {
    std::vector<const char*> display(labels);
    std::vector<int> ids(stored);
    if (ids.empty()) for (int i=0; i<int(display.size()); ++i) ids.push_back(i);
    const auto found = std::find(ids.begin(), ids.end(), value);
    int selected = found == ids.end() ? 0 : int(found - ids.begin());
    ImGui::SetNextItemWidth(std::min(360.0f*ui_scale, ImGui::GetContentRegionAvail().x));
    if (!ImGui::Combo(label, &selected, display.data(), int(display.size()))) return false;
    value = ids[selected];
    tooie::menu::set_int(id, value);
    return true;
}

void caption(const char* title, const char* detail) {
    ImGui::TextUnformatted(title);
    ImGui::SameLine(); ImGui::TextDisabled("%s", detail);
}

void apply_graphics() {
    using namespace tooie;
    frontend::GraphicsSettings graphics{};
    const int scale = enum_setting("res_option",0,{"Auto","Original","Original2x"});
    graphics.auto_resolution = scale == 0;
    graphics.resolution_multiplier = scale == 0 ? 1U : static_cast<unsigned>(scale);
    graphics.downsample_multiplier = unsigned(menu::get_int("tooie_downsample_quality",1));
    graphics.msaa_samples = unsigned(menu::get_int("msaa_option",0));
    graphics.vsync = menu::get_bool("tooie_vsync",false);
    int preset = menu::get_int("tooie_pacing_preset",0);
    if (preset == 1) {
        const auto pacing=frontend::baseline_graphics_preset();
        graphics.vsync=pacing.vsync;
        graphics.output_rate_mode=pacing.output_rate_mode;
        graphics.custom_output_rate=pacing.custom_output_rate;
        graphics.presentation_mode=pacing.presentation_mode;
    } else if (preset == 2 || preset == 3) {
        const auto pacing=preset==2 ? frontend::display_early_graphics_preset()
            : frontend::display_console_graphics_preset();
        graphics.vsync=pacing.vsync;
        graphics.output_rate_mode=pacing.output_rate_mode;
        graphics.custom_output_rate=pacing.custom_output_rate;
        graphics.presentation_mode=pacing.presentation_mode;
    } else {
        graphics.output_rate_mode = static_cast<frontend::OutputRateMode>(menu::get_int("tooie_output_rate_mode",0));
        graphics.custom_output_rate = unsigned(menu::get_int("tooie_custom_output_rate",60));
        graphics.presentation_mode = static_cast<frontend::PresentationMode>(menu::get_int("tooie_presentation_mode",1));
    }
    const auto display = frontend::DisplaySettings{
        static_cast<frontend::OutputMode>(menu::get_int("tooie_output_mode",0)),
        static_cast<frontend::OutputDisplayId>(menu::get_int("tooie_output_display",0)),
        static_cast<frontend::OutputResolutionId>(menu::get_int("tooie_output_resolution",6)),
        frontend::OutputFit::PreserveGameAspect};
    graphics.fullscreen = display.mode == frontend::OutputMode::ExclusiveFullscreen;
    native_host::request_graphics_settings(graphics);
    native_host::request_display_settings(display);
    widescreen::configure_profile(menu::get_int("tooie_game_widescreen",0)!=0);
    widescreen::configure_native_aspect(static_cast<widescreen::NativeAspect>(menu::get_int("tooie_native_aspect",0)));
    draw_distance::configure_actor_distance(static_cast<draw_distance::ActorDistance>(menu::get_int("tooie_actor_draw_distance",0)));
    hud_layout::configure_counter_layout(static_cast<hud_layout::CounterLayout>(menu::get_int("tooie_counter_layout",0)));
    hud_layout::configure_proportions(static_cast<hud_layout::Proportions>(menu::get_int("tooie_hud_proportions",0)));
    features::configure_cutscene_aspect(static_cast<features::CutsceneAspect>(menu::get_int("tooie_cutscene_aspect",1)));
    camera_interpolation::configure_cutscene_motion(static_cast<camera_interpolation::CutsceneMotion>(menu::get_int("tooie_cutscene_motion",0)));
}

void draw_home() {
    if (ultramodern::is_game_started()) {
        ImGui::TextWrapped("The game is running. Resume, restart from the title, or return to the launcher using the actions at the top right. Quit Application remains separate below.");
    } else {
    ImGui::TextWrapped("A native recompiled Banjo-Tooie. Select your own NTSC-U 1.0 ROM to begin.");
    const bool valid=rom_ready.load(std::memory_order_acquire);
    if (ImGui::Button("Choose ROM...",ImVec2(190*ui_scale,42*ui_scale))) {
#ifdef _WIN32
        enqueue({TaskKind::Browse,{}});
#else
        browse_directory = profile_root;
        browse_open = true;
#endif
    }
    ImGui::SameLine();
    if (valid) ImGui::TextColored(ImVec4(.48f,.86f,.55f,1),"Selected ROM: NTSC-U 1.0 validated");
    else ImGui::TextDisabled("No validated ROM selected");
    if (ImGui::Button("Start Game", ImVec2(190*ui_scale,45*ui_scale))) {
        if (valid) enqueue({TaskKind::Start,{}});
        else tooie::menu::show_notice("Choose a ROM","Select and validate your Banjo-Tooie NTSC-U 1.0 ROM first.",false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(manual_rom_path?"Hide manual path":"Enter ROM path manually"))
        manual_rom_path=!manual_rom_path;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if (ImGui::Button("Start Private Practice...",ImVec2(190*ui_scale,40*ui_scale))) {
        if (valid) pending_practice=true;
        else tooie::menu::show_notice("Choose a ROM",
            "Select and validate your Banjo-Tooie NTSC-U 1.0 ROM first.",false);
    }
    ImGui::TextWrapped("Private practice is a separate session. Its progress is not written to your normal save file; use Save State in Tools > Practice.");
#endif
    if (manual_rom_path) {
        ImGui::SetNextItemWidth(std::max(220.0f,ImGui::GetContentRegionAvail().x-145*ui_scale));
        ImGui::InputTextWithHint("##rom_path","Full path to a .z64 file",rom_path,sizeof(rom_path));
        ImGui::SameLine();
        if (ImGui::Button("Validate Path")) {
            const auto text=std::u8string(reinterpret_cast<const char8_t*>(rom_path));
            enqueue({TaskKind::Import,std::filesystem::path(text)});
        }
    }
    if (browse_open) {
        ImGui::Separator();
        ImGui::TextWrapped("Folder: %s", browse_directory.string().c_str());
        if (ImGui::Button("Parent folder")) browse_directory = browse_directory.parent_path();
        ImGui::SameLine(); if (ImGui::Button("Close browser")) browse_open = false;
        ImGui::BeginChild("rom_browser", ImVec2(0,190*ui_scale),true);
        std::error_code error;
        std::vector<std::filesystem::directory_entry> entries;
        for (std::filesystem::directory_iterator it(browse_directory,error), end;
             !error && it != end && entries.size()<256; it.increment(error)) entries.push_back(*it);
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){
            return a.path().filename().string() < b.path().filename().string(); });
        for (const auto& entry:entries) {
            if (entry.is_directory(error)) {
                const std::string label="[Folder] "+entry.path().filename().string();
                if (ImGui::Selectable(label.c_str())) browse_directory=entry.path();
            } else {
                const auto ext=entry.path().extension().string();
                if (ext==".z64")
                    if (ImGui::Selectable(entry.path().filename().string().c_str())) {
                        const auto native_utf8=entry.path().u8string();
                        const std::string selected(native_utf8.begin(),native_utf8.end());
                        std::snprintf(rom_path,sizeof(rom_path),"%s",selected.c_str());
                        browse_open=false;
                    }
            }
        }
        if (error) ImGui::TextWrapped("Cannot read this folder: %s",error.message().c_str());
        ImGui::EndChild();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Turn off active cheat effects");
    ImGui::TextWrapped("If cheats are enabled in a save file, use a button below to turn off their active effects.\n\nPermanent unlocks, such as learned moves and opened stations, will remain. A backup is created first.");
    for (int slot=1;slot<=3;++slot) {
        ImGui::PushID(slot);
        if (ImGui::Button(("Disable File "+std::to_string(slot)+" Cheats").c_str(),ImVec2(210*ui_scale,0))) {
            pending_reset_slot=slot;
        }
        ImGui::PopID();
    }
    if (pending_reset_slot) ImGui::OpenPopup("Confirm active cheat reset");
    prepare_modal();
    if (ImGui::BeginPopupModal("Confirm active cheat reset",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Turn off active cheat effects in file %d? A backup is kept. Permanent progress and other files are unchanged. Start Game afterward.",pending_reset_slot);
        if (ImGui::Button("Turn Off",ImVec2(120,0))) {
            enqueue({TaskKind::Reset,{},pending_reset_slot});
            pending_reset_slot=0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(120,0))) {
            pending_reset_slot=0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    }
    ImGui::Separator();
    if (ImGui::Button("Quit Application")) pending_quit=true;
    if (pending_quit) ImGui::OpenPopup("Quit Banjo-Tooie");
    prepare_modal(430.0f);
    if (ImGui::BeginPopupModal("Quit Banjo-Tooie",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Quit Banjo-Tooie: Recompiled? Unsaved game progress will be lost. Saved games and settings remain.");
        if (ImGui::Button("Quit",ImVec2(110,0))) {
            enqueue({TaskKind::Quit,{}});
            pending_quit=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) { pending_quit=false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void draw_lifecycle_confirmations() {
    if (pending_restart) ImGui::OpenPopup("Restart Game?");
    prepare_modal(430.0f);
    if (ImGui::BeginPopupModal("Restart Game?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(practice_process
            ? "Restart this private practice session at the title screen? Progress not captured in the practice state slot will be lost. Normal save files remain unchanged."
            : "Restart to the title screen? Unsaved progress will be lost. Your saved games and settings will stay.");
        if (ImGui::Button("Restart",ImVec2(110,0))) {
            enqueue({TaskKind::Restart,{}});
            pending_restart=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) {
            pending_restart=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (pending_return) ImGui::OpenPopup("Return to Launcher?");
    prepare_modal(450.0f);
    if (ImGui::BeginPopupModal("Return to Launcher?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(practice_process
            ? "Return to the normal launcher? Private-session progress not captured in the practice state slot will be lost. Normal save files stay unchanged."
            : "Return to the launcher? Unsaved progress will be lost. Your saved games, selected ROM, controller profiles, and settings will stay.");
        if (ImGui::Button("Return",ImVec2(110,0))) {
            enqueue({TaskKind::ReturnToLauncher,{}});
            pending_return=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) {
            pending_return=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    if (pending_practice) ImGui::OpenPopup("Start Private Practice?");
    prepare_modal(470.0f);
    if (ImGui::BeginPopupModal("Start Private Practice?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This closes the current session and starts a separate private practice process. Unsaved progress in this session is lost. Practice progress is NOT written to your normal game save; use the fixed Save State slot in Tools > Practice. States require the same build, ROM, gameplay settings and render settings; updates may invalidate them. This is experimental.");
        if (ImGui::Button("Start Private Practice",ImVec2(185,0))) {
            enqueue({TaskKind::StartPractice,{}});
            pending_practice=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) {
            pending_practice=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (practice_process && pending_save_state) ImGui::OpenPopup("Overwrite Practice State?");
    prepare_modal(470.0f);
    if (ImGui::BeginPopupModal("Overwrite Practice State?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Capture the current game into the one private practice state slot? Any earlier state in that slot will be replaced. Your normal game save will not be written.");
        if (ImGui::Button("Save State",ImVec2(125,0))) {
            const auto path=profile_root/"private-practice.tooie-state";
            if (tooie::persistent_state::controller::request(
                    tooie::persistent_state::controller::Action::Capture,path))
                tooie::menu::set_open(false);
            else tooie::menu::show_notice("Save State Unavailable",
                tooie::persistent_state::controller::status().message,false);
            pending_save_state=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) {
            pending_save_state=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (practice_process && pending_load_state) ImGui::OpenPopup("Load Practice State?");
    prepare_modal(470.0f);
    if (ImGui::BeginPopupModal("Load Practice State?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Restore the one private practice state slot? Current unsaved practice progress will be lost. This does not load or write your normal game save. The state must match this build, ROM, gameplay settings and render settings.");
        if (ImGui::Button("Load State",ImVec2(125,0))) {
            const auto path=profile_root/"private-practice.tooie-state";
            if (tooie::persistent_state::controller::request(
                    tooie::persistent_state::controller::Action::Restore,path))
                tooie::menu::set_open(false);
            else tooie::menu::show_notice("Load State Unavailable",
                tooie::persistent_state::controller::status().message,false);
            pending_load_state=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel",ImVec2(110,0))) {
            pending_load_state=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
#endif
}

void draw_general() {
    const char* groups[] = {"Input", "Camera", "Exploration", "Diagnostics"};
    for (int i=0;i<4;++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Selectable(groups[i],general_category==i,0,
            ImVec2(i==3?95*ui_scale:100*ui_scale,0))) {
            general_category=i;
            tooie::menu::set_int("tooie_general_category",i);
        }
    }
    ImGui::Separator();
    if (general_category==0) {
        ImGui::TextUnformatted("Controller and input");
        auto controllers=tooie::input::controllers();
        const auto preferred=tooie::input::preferred_device_id();
        const bool auto_selection=tooie::input::controller_auto_selection();
        const auto selected_instance=tooie::input::selected_controller_instance();
        const auto selected=std::find_if(controllers.begin(),controllers.end(),
            [selected_instance](const auto& entry){ return entry.instance==selected_instance; });
        const std::string selected_name=selected==controllers.end()
            ? (auto_selection?"Auto (no controller connected)":
                (preferred.empty()?"Choose a controller":"Saved controller is not connected"))
            : selected->name+(auto_selection?" (Auto)":"");
        if (ImGui::BeginCombo("Preferred Controller",selected_name.c_str())) {
            if (ImGui::Selectable("Auto (first available)",auto_selection))
                tooie::input::select_controller(-1);
            for (const auto& controller:controllers) {
                std::string label=controller.name;
                if (controller.friendly_name!=controller.name)
                    label+="  (name: "+controller.friendly_name+")";
                label+="##general"+std::to_string(controller.instance);
                if (ImGui::Selectable(label.c_str(),controller.selected))
                    tooie::input::select_controller(controller.instance);
            }
            ImGui::EndCombo();
        }
        if (controllers.empty()) ImGui::TextDisabled("No controller connected. Keyboard remains available.");
        else for (const auto& controller:controllers) {
            std::string label=controller.name;
            if (controller.friendly_name!=controller.name)
                label+=" (name: "+controller.friendly_name+")";
            if (controller.selected) label+=" (active)";
            ImGui::TextUnformatted(label.c_str());
        }
        int deadzone=int(tooie::menu::get_number("joystick_deadzone",5.0));
        if (ImGui::SliderInt("Stick Deadzone",&deadzone,0,50,"%d%%"))
            tooie::menu::set_number("joystick_deadzone",double(deadzone));
        int rumble=int(tooie::menu::get_number("rumble_strength",100.0));
        if (ImGui::SliderInt("Rumble Strength",&rumble,0,100,"%d%%"))
            tooie::menu::set_number("rumble_strength",double(rumble));
        bool background=tooie::menu::get_bool("background_input_mode",true);
        if (ImGui::Checkbox("Allow input when window is unfocused",&background))
            tooie::menu::set_bool("background_input_mode",background);
        bool cursor=tooie::menu::get_bool("tooie_show_cursor_when_opening_settings",true);
        if (ImGui::Checkbox("Show cursor when opening settings",&cursor))
            tooie::menu::set_bool("tooie_show_cursor_when_opening_settings",cursor);
    } else if (general_category==1) {
        ImGui::TextUnformatted("Camera");
        bool enabled=tooie::menu::get_bool("tooie_analog_camera_enabled",false);
        if (ImGui::Checkbox("Analog Camera",&enabled)) {
            tooie::menu::set_bool("tooie_analog_camera_enabled",enabled);
            tooie::camera::set_analog_enabled(enabled);
        }
        ImGui::TextWrapped("Right stick turns eligible third-person cameras and supports original first-person aiming limits.");
        bool invert_x=tooie::menu::get_bool("tooie_camera_invert_x",false);
        if (ImGui::Checkbox("Invert Horizontal Camera",&invert_x)) {
            tooie::menu::set_bool("tooie_camera_invert_x",invert_x);
            tooie::camera::set_horizontal_inverted(invert_x);
        }
        bool invert_y=tooie::menu::get_bool("tooie_camera_invert_y",false);
        if (ImGui::Checkbox("Invert Vertical Camera",&invert_y)) {
            tooie::menu::set_bool("tooie_camera_invert_y",invert_y);
            tooie::camera::set_vertical_inverted(invert_y);
        }
        ImGui::Separator();
        tooie::frontend::cheats::draw_free_camera();
    } else if (general_category==2) {
        ImGui::TextUnformatted("Exploration");
        int rate=tooie::menu::get_int("tooie_fast_forward_rate",2);
        if (choose("Fast Forward Hold Rate","tooie_fast_forward_rate",rate,{"2x","4x","6x","8x"},{2,4,6,8}))
            tooie::timing::set_fast_forward_rate(unsigned(rate));
        ImGui::TextWrapped("Hold your Fast Forward binding to accelerate guest time; audio is muted while active.");
    } else {
        ImGui::TextUnformatted("Diagnostics");
        int mode=tooie::menu::get_int("tooie_diagnostics_overlay",0);
        if (choose("Overlay","tooie_diagnostics_overlay",mode,{"Off","FPS","Detailed","Practice"}))
            tooie::diagnostics::overlay::set_mode(static_cast<tooie::diagnostics::overlay::Mode>(mode));
        int corner=tooie::menu::get_int("tooie_diagnostics_corner",1);
        if (choose("Corner","tooie_diagnostics_corner",corner,
            {"Top Left","Top Right","Bottom Left","Bottom Right"}))
            tooie::diagnostics::overlay::set_corner(static_cast<tooie::diagnostics::overlay::Corner>(corner));
        ImGui::TextWrapped("F3 cycles the overlay; F4 records an issue marker and a short diagnostics snapshot in this profile's logs.");
        if (ImGui::Button("Mark Graphics Issue Now"))
            tooie::frontend::cheats::trigger_graphics_issue_marker();
        ImGui::Separator();
        bool mapped_legend=tooie::menu::get_bool("tooie_mapped_prompt_legend",false);
        if (ImGui::Checkbox("Mapped Button Legend",&mapped_legend)) {
            tooie::menu::set_bool("tooie_mapped_prompt_legend",mapped_legend);
            tooie::frontend::prompt::set_visible(mapped_legend);
        }
        int prompt_corner=tooie::menu::get_int("tooie_mapped_prompt_corner",3);
        if (choose("Legend Corner","tooie_mapped_prompt_corner",prompt_corner,
            {"Top Left","Top Right","Bottom Left","Bottom Right"}))
            tooie::frontend::prompt::set_corner(static_cast<tooie::frontend::prompt::Corner>(prompt_corner));
        ImGui::TextWrapped("Optional host mapping for A, B, Z and Start; original game prompts are unchanged.");
    }
}

void draw_graphics() {
    const char* groups[]={"Quality","Framing","Pacing","Display"};
    for (int i=0;i<4;++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Selectable(groups[i],graphics_category==i,0,ImVec2(110*ui_scale,0))) {
            graphics_category=i;
            tooie::menu::set_int("tooie_graphics_category",i);
        }
    }
    ImGui::Separator();
    if (graphics_category==0) {
        int resolution=tooie::menu::get_int("res_option",0);
        choose("Render Output Scale","res_option",resolution,
            {"Auto (match output)","1x (320 x 240)","2x (640 x 480)","4x (1280 x 960)","8x (2560 x 1920)"},
            {0,1,2,4,8});
        ImGui::TextWrapped("Auto follows the output. Fixed scale sets the image size after any downsampling; window size is separate.");
        int down=tooie::menu::get_int("tooie_downsample_quality",1);
        if (resolution==0) ImGui::BeginDisabled();
        choose("Downsampling","tooie_downsample_quality",down,{"Off","2x internal","4x internal"},{1,2,4});
        if (resolution==0) ImGui::EndDisabled();
        ImGui::TextWrapped("Renders internally above the selected fixed output scale, then reduces to it. Internal scale is capped at 8x.");
        const auto capabilities=tooie::native_host::graphics_capabilities();
        int msaa=tooie::menu::get_int("msaa_option",0);
        const bool supported=capabilities.programmable_sample_positions &&
            msaa<=int(capabilities.max_msaa_samples);
        const std::string current_msaa=msaa==0?"None":std::to_string(msaa)+"x";
        ImGui::SetNextItemWidth(std::min(360.0f*ui_scale,ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("MS Anti-Aliasing",current_msaa.c_str())) {
            for (int sample:{0,2,4,8}) {
                if (sample && (!capabilities.programmable_sample_positions ||
                    sample>int(capabilities.max_msaa_samples))) continue;
                const std::string label=sample==0?"None":std::to_string(sample)+"x";
                if (ImGui::Selectable(label.c_str(),sample==msaa)) {
                    msaa=sample;
                    tooie::menu::set_int("msaa_option",msaa);
                }
            }
            ImGui::EndCombo();
        }
        if (!supported && msaa!=0)
            ImGui::TextWrapped("%s is unavailable on this renderer; None is applied until you choose a supported value.",current_msaa.c_str());
        else if (!capabilities.programmable_sample_positions)
            ImGui::TextDisabled("Current renderer does not support programmable sample positions.");
        int actors=tooie::menu::get_int("tooie_actor_draw_distance",0);
        choose("Actor Draw Distance","tooie_actor_draw_distance",actors,{"Original","Extended actors (2x)"});
        ImGui::TextWrapped("Draw distance changes take effect after restarting the game. Terrain and fog remain original.");
    } else if (graphics_category==1) {
        int wide=tooie::menu::get_int("tooie_game_widescreen",0);
        choose("Game Widescreen","tooie_game_widescreen",wide,{"Original (4:3)","On (selected aspect)"});
        int aspect=tooie::menu::get_int("tooie_native_aspect",0);
        choose("Widescreen Aspect","tooie_native_aspect",aspect,
            {"16:9","21:9","32:9","43:18 (3440 x 1440)"});
        int counters=tooie::menu::get_int("tooie_counter_layout",0);
        choose("HUD Counter Placement","tooie_counter_layout",counters,{"Centered","Expanded"});
        int hud_proportions=tooie::menu::get_int("tooie_hud_proportions",0);
        choose("HUD Proportions","tooie_hud_proportions",hud_proportions,
            {"Original (4:3 elements)","Stretch with screen"});
        ImGui::TextWrapped("Original keeps HUD art and text at native proportions over a widescreen world. Stretch restores the game's unadjusted 2D projection.");
        int cutscene=tooie::menu::get_int("tooie_cutscene_aspect",1);
        choose("Cutscene Aspect","tooie_cutscene_aspect",cutscene,{"Original 4:3","Follow game"});
        int motion=tooie::menu::get_int("tooie_cutscene_motion",0);
        choose("Cutscene Motion","tooie_cutscene_motion",motion,{"Interpolated","Original motion"});
        ImGui::TextWrapped("Framing options are latched when the game starts. Restart the game to apply changes.");
    } else if (graphics_category==2) {
        int preset=tooie::menu::get_int("tooie_pacing_preset",0);
        choose("Pacing Comparison","tooie_pacing_preset",preset,
            {"Custom","Console (N64 timing)","Display + Present Early","Display + Console"});
        if (preset==1) ImGui::TextWrapped("Console: VSync on, original output rate; presentation follows game framebuffer swaps at the guest cadence.");
        else if (preset==2) ImGui::TextWrapped("Display + Present Early: VSync on, display output target, smooth generated frames.");
        else if (preset==3) ImGui::TextWrapped("Display + Console: VSync on, display output target; presentation still follows game framebuffer swaps.");
        else {
            bool vsync=tooie::menu::get_bool("tooie_vsync",false);
            if (ImGui::Checkbox("VSync (Recommended: Off)",&vsync)) tooie::menu::set_bool("tooie_vsync",vsync);
            int rate=tooie::menu::get_int("tooie_output_rate_mode",0);
            choose("Output Rate","tooie_output_rate_mode",rate,{"Original","Display (Recommended)","Custom"});
            if (rate==2) {
                int hz=tooie::menu::get_int("tooie_custom_output_rate",60);
                if (ImGui::SliderInt("Custom Hz",&hz,20,240)) tooie::menu::set_int("tooie_custom_output_rate",hz);
            }
            int presentation=tooie::menu::get_int("tooie_presentation_mode",1);
            choose("Presentation","tooie_presentation_mode",presentation,{"Console (guest cadence)","Present Early (Recommended)"});
            ImGui::TextWrapped("Recommended starting point: VSync off, Display output rate, and Present Early for smooth generated frames. Choose Original output rate for the game's original cadence.");
        }
        ImGui::TextWrapped("A Display output target may still run at the original game rate in Console mode; Present Early enables smooth generated frames.");
        if (ImGui::Button("Restore Recommended Settings")) {
            tooie::menu::set_int("tooie_pacing_preset",0);
            tooie::menu::set_bool("tooie_vsync",false);
            tooie::menu::set_int("tooie_output_rate_mode",1);
            tooie::menu::set_int("tooie_custom_output_rate",60);
            tooie::menu::set_int("tooie_presentation_mode",1);
            apply_graphics();
            const bool saved=tooie::frontend::settings::flush();
            tooie::menu::show_notice(saved?"Recommended Graphics Restored":"Graphics Save Failed",
                saved ? "Pacing is set to VSync off, Display output rate, and Present Early. Controls, output display, and save progress were not changed."
                      : "The recommended pacing was applied for this session but could not be saved.",saved);
        }
        ImGui::TextWrapped("Apply the selection below. Output cadence depends on the actual display and renderer.");
    } else {
        int mode=tooie::menu::get_int("tooie_output_mode",0);
        choose("Output Mode","tooie_output_mode",mode,
            {"Windowed","Borderless desktop","Exclusive fullscreen"});
        int display=tooie::menu::get_int("tooie_output_display",0);
        choose("Output Display","tooie_output_display",display,
            {"Current display","Display 1","Display 2","Display 3","Display 4"});
        const auto caps=tooie::native_host::display_capabilities();
        for (const auto& entry:caps.displays)
            ImGui::TextDisabled("Display %u: %s (%u x %u)",unsigned(entry.id),entry.name.c_str(),
                entry.desktop_width,entry.desktop_height);
        int resolution=tooie::menu::get_int("tooie_output_resolution",6);
        choose("Output Resolution","tooie_output_resolution",resolution,
            {"Desktop native","1280 x 720","1920 x 1080","2560 x 1440",
             "3440 x 1440","3840 x 2160","Default window (960 x 720)"});
        ImGui::TextWrapped("Output mode and resolution apply when starting the game. The launcher stays windowed at 960 x 720.");
    }
    ImGui::Separator();
    if (ImGui::Button("Apply Graphics",ImVec2(170*ui_scale,0))) {
        apply_graphics();
        const bool saved=tooie::frontend::settings::flush();
        tooie::menu::show_notice(saved?"Graphics Applied":"Graphics Save Failed",
            saved ? "Live renderer choices apply now. Framing and output window changes may require game restart."
                  : "The change was requested for this session, but profile settings could not be saved.",saved);
    }
}

void draw_controls() {
    const auto controllers=tooie::input::controllers();
    const auto selected_id=tooie::input::selected_device_id();
    const auto preferred_id=tooie::input::preferred_device_id();
    const bool auto_selection=tooie::input::controller_auto_selection();
    const auto selected_instance=tooie::input::selected_controller_instance();
    const auto selected=std::find_if(controllers.begin(),controllers.end(),
        [selected_instance](const auto& entry){ return entry.instance==selected_instance; });
    const std::string selected_label=selected==controllers.end()
        ? (auto_selection?"Auto (no controller connected)":
            (preferred_id.empty()?"Choose a controller":"Saved controller is not connected"))
        : selected->name+(auto_selection?" (Auto)":"");
    ImGui::TextUnformatted("Controller used for gameplay and binding capture");
    ImGui::SetNextItemWidth(std::min(430.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Device",selected_label.c_str())) {
        if (ImGui::Selectable("Auto (first available)",auto_selection))
            tooie::input::select_controller(-1);
        for (const auto& controller:controllers) {
            std::string label=controller.name;
            if (controller.friendly_name!=controller.name) label+="  (name: "+controller.friendly_name+")";
            if (controller.ambiguous) label+="  [identical device]";
            label+="##"+std::to_string(controller.instance);
            if (ImGui::Selectable(label.c_str(),controller.selected))
                tooie::input::select_controller(controller.instance);
        }
        ImGui::EndCombo();
    }
    if (controllers.empty()) ImGui::TextDisabled("No controller connected. Keyboard bindings remain available.");
    else if (selected==controllers.end() && !preferred_id.empty())
        ImGui::TextWrapped("The saved controller is missing or duplicated. Choose a connected device; its mappings have not been reassigned.");
    else if (selected!=controllers.end()) {
        ImGui::TextWrapped("Hardware device: %s",selected->name.c_str());
        if (!selected->mapping_name.empty())
            ImGui::TextDisabled("SDL mapping label: %s",selected->mapping_name.c_str());
        ImGui::TextDisabled("VID:%04X  PID:%04X",unsigned(selected->vendor),unsigned(selected->product));
        ImGui::TextDisabled("SDL GUID");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##sdl-guid",const_cast<char*>(selected->sdl_guid.c_str()),
            selected->sdl_guid.size()+1,ImGuiInputTextFlags_ReadOnly);
        if (selected->ambiguous)
            ImGui::TextWrapped("This device has an identical identity. Its name, type, profile, and mappings are session-only and cannot be restored reliably.");
    }

    const auto controller_id=selected_id.empty()?preferred_id:selected_id;
    if (edited_controller_id!=controller_id) {
        edited_controller_id=controller_id;
        std::snprintf(controller_friendly_name,sizeof(controller_friendly_name),"%s",
            tooie::input::selected_friendly_name().c_str());
    }
    ImGui::SetNextItemWidth(std::min(300.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("Friendly name","My N64 Controller",controller_friendly_name,
        sizeof(controller_friendly_name));
    ImGui::SameLine();
    if (ImGui::Button("Save Name")) tooie::input::set_selected_friendly_name(controller_friendly_name);

    auto type=tooie::input::selected_controller_type();
    int type_index=int(type);
    const char* type_names[]={"N64","DualShock / DualSense","Xbox","Nintendo Pro Controller","Steam Controller","Other"};
    ImGui::SetNextItemWidth(std::min(300.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Controller Type",&type_index,type_names,int(sizeof(type_names)/sizeof(type_names[0]))))
        tooie::input::set_selected_controller_type(static_cast<tooie::input::ControllerType>(type_index));

    const auto profiles=tooie::input::controller_profiles();
    const auto active_profile=tooie::input::active_controller_profile();
    ImGui::SetNextItemWidth(std::min(300.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Mapping Profile",active_profile.empty()?"Default":active_profile.c_str())) {
        for (const auto& profile:profiles)
            if (ImGui::Selectable(profile.c_str(),profile==active_profile))
                tooie::input::select_controller_profile(profile);
        ImGui::EndCombo();
    }
    if (edited_profile_name!=active_profile) {
        edited_profile_name=active_profile;
        std::snprintf(controller_profile_name,sizeof(controller_profile_name),"%s",active_profile.c_str());
    }
    ImGui::SetNextItemWidth(std::min(240.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##profile_name","Profile name",controller_profile_name,
        sizeof(controller_profile_name));
    if (ImGui::Button("Save New Profile")) {
        const bool saved=tooie::input::create_controller_profile(controller_profile_name);
        tooie::menu::show_notice(saved?"Profile Saved":"Profile Not Saved",
            saved?"The new controller mapping profile is selected.":"Use a unique, non-empty profile name.",saved);
    }
    if (ImGui::Button("Rename Active Profile")) {
        const bool renamed=tooie::input::rename_controller_profile(controller_profile_name);
        tooie::menu::show_notice(renamed?"Profile Renamed":"Profile Not Renamed",
            renamed?"The active controller mapping profile was renamed.":"Use a unique, non-empty profile name.",renamed);
    }
    if (ImGui::Button("Reset Profile")) ImGui::OpenPopup("Reset controller profile");
    prepare_modal(480.0f);
    if (ImGui::BeginPopupModal("Reset controller profile",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Restore the default controller bindings for profile '%s'? Keyboard bindings are not changed.",active_profile.c_str());
        if (ImGui::Button("Reset")) { tooie::input::reset_active_controller_profile(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::TextWrapped("Select a binding, then press a key or a control on the selected controller. Keyboard and controller mappings are independent.");
    if (ImGui::Button("Restore Default Controls")) ImGui::OpenPopup("Reset bindings");
    prepare_modal(460.0f);
    if (ImGui::BeginPopupModal("Reset bindings",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Restore the default keyboard and controller bindings?");
        if (ImGui::Button("Restore")) { tooie::input::reset_bindings(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if (ImGui::BeginTable("bindings",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Keyboard"); ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (int i=0;i<int(tooie::input::Action::Count);++i) {
            const auto action=static_cast<tooie::input::Action>(i);
            ImGui::PushID(i);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s",tooie::input::action_name(action));
            auto draw_slots=[&](bool keyboard) {
                std::string labels[2];
                for (unsigned slot=0;slot<2;++slot) {
                    if (keyboard) {
                        const auto key=tooie::input::get_binding(action,tooie::input::Device::Keyboard,slot);
                        if (key.code>=0) labels[slot]=tooie::input::binding_label(key);
                    } else {
                        for (auto device:{tooie::input::Device::Button,tooie::input::Device::AxisPositive,
                            tooie::input::Device::AxisNegative}) {
                            const auto binding=tooie::input::get_binding(action,device,slot);
                            if (binding.code<0) continue;
                            if (!labels[slot].empty()) labels[slot]+=" / ";
                            labels[slot]+=tooie::input::binding_label_for_type(binding,
                                tooie::input::selected_controller_type());
                        }
                    }
                }
                const float available=ImGui::GetContentRegionAvail().x;
                const float gap=4.0f*ui_scale;
                const float alternate_width=labels[1].empty() ? 28.0f*ui_scale :
                    std::min(available*0.45f,ImGui::CalcTextSize(labels[1].c_str()).x+
                        2.0f*ImGui::GetStyle().FramePadding.x);
                for (unsigned slot=0;slot<2;++slot) {
                    const std::string& full_label=labels[slot];
                    const std::string button_label=(full_label.empty() ? (slot==0?"Unbound":"+") : full_label)+
                        "##"+(keyboard?"keyboard":"controller")+std::to_string(slot);
                    const float width=slot==0 ? std::max(1.0f,available-alternate_width-gap) : alternate_width;
                    if (ImGui::Button(button_label.c_str(),ImVec2(width,0)))
                        tooie::input::begin_capture(action,keyboard?tooie::input::Device::Keyboard:
                            tooie::input::Device::Button,slot);
                    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("%s: %s",slot==0?"Primary":"Alternate",
                            full_label.empty()?"Unbound - select to add a binding":full_label.c_str());
                        ImGui::EndTooltip();
                    }
                    if (slot==0) ImGui::SameLine(0,gap);
                }
            };
            ImGui::TableSetColumnIndex(1);
            draw_slots(true);
            ImGui::TableSetColumnIndex(2);
            draw_slots(false);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto capture=tooie::input::capture_status();
    if (capture.active) ImGui::OpenPopup("Bind Input");
    prepare_modal(460.0f);
    if (ImGui::BeginPopupModal("Bind Input",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!capture.active) ImGui::CloseCurrentPopup();
        else {
            ImGui::Text("Action: %s",tooie::input::action_name(capture.action));
            ImGui::TextWrapped("Input device: %s",capture.device==tooie::input::Device::Keyboard
                ? "Keyboard" : selected_label.c_str());
            ImGui::Separator();
            if (capture.arming) ImGui::TextWrapped("Release the control that opened this dialog. Capture begins from neutral.");
            else ImGui::TextWrapped("Press the new input.");
            ImGui::Text("%d seconds remaining.",capture.remaining_seconds);
            if (ImGui::Button("Cancel",ImVec2(110,0))) tooie::input::cancel_capture();
        }
        ImGui::EndPopup();
    }
    ImGui::TextWrapped(practice_process
        ? "Save Progress / F5 requests the game's in-game save manager, but this private session keeps its EEPROM in memory only. It does not create a durable state; use Tools > Practice > Save State."
        : "Save Progress requests the game's normal save manager; open its original pause menu first. It is not a host save state.");
}

void draw_sound() {
    ImGui::TextUnformatted("Volume");
    int main_volume=int(tooie::menu::get_number("main_volume",100.0));
    if (ImGui::SliderInt("Main Volume",&main_volume,0,100,"%d%%"))
        tooie::menu::set_number("main_volume",double(main_volume));
    int music_volume=int(tooie::menu::get_number("tooie_music_volume",100.0));
    if (ImGui::SliderInt("Jukebox Music Volume",&music_volume,0,100,"%d%%")) {
        tooie::menu::set_number("tooie_music_volume",double(music_volume));
        tooie::music::set_percent(unsigned(music_volume));
    }
    ImGui::TextWrapped("Jukebox volume controls the game's original music list. Other sequence cues and ordinary sound effects may use different paths.");
}

void draw_tools() {
    const char* groups[]={"Cheats","Travel","Moves","World Entrances","Collectibles","Player","Bosses","Practice"};
    if (tools_category!=7) {
        tooie::frontend::cheats::draw_access_gate();
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Tools category");
    ImGui::SetNextItemWidth(std::min(330.0f*ui_scale,ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##tools_category",&tools_category,groups,8))
        tooie::menu::set_int("tooie_tools_category",tools_category);
    ImGui::Separator();
    if (tools_category==7) {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        if (practice_process) {
            const auto state_status=tooie::persistent_state::controller::status();
            ImGui::TextColored(ImVec4(1.0f,.72f,.24f,1.0f),
                "PRIVATE SAVE-STATE PRACTICE");
            ImGui::TextWrapped("Normal game progress is not written in this session. Use the fixed state slot below; returning to the launcher discards progress not captured there. States require the same build, ROM, gameplay settings and render settings, and an update may invalidate them. Experimental: verify important results in game.");
            const auto slot=profile_root/"private-practice.tooie-state";
            std::error_code slot_error;
            const bool slot_exists=std::filesystem::is_regular_file(slot,slot_error) && !slot_error;
            ImGui::TextWrapped("State slot: %s",slot.filename().string().c_str());
            ImGui::BeginDisabled(!state_status.practice_enabled || state_status.busy ||
                !ultramodern::is_game_started());
            if (ImGui::Button("Save State...")) pending_save_state=true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state_status.practice_enabled || state_status.busy ||
                !ultramodern::is_game_started() || !slot_exists);
            if (ImGui::Button("Load State...")) pending_load_state=true;
            ImGui::EndDisabled();
            if (!slot_exists) ImGui::TextDisabled("No state in this slot yet.");
            if (state_status.busy) ImGui::TextWrapped("State request in progress: %s",
                state_status.message.c_str());
            else if (state_status.outcome!="idle" && !state_status.outcome.empty())
                ImGui::TextWrapped("Last state request: %s - %s",
                    state_status.outcome.c_str(),state_status.message.c_str());
        } else {
            ImGui::TextWrapped("Save-state practice runs in a separate private session. Normal progress saving is unchanged here.");
            if (ImGui::Button("Start Private Practice...")) pending_practice=true;
        }
        ImGui::Separator();
#endif
        const auto state=tooie::practice::snapshot();
        static std::size_t selected_destination=0;
        ImGui::TextUnformatted("Warp-pad and silo entrances");
        const auto* chosen=tooie::practice::travel::destination(selected_destination);
        char chosen_label[160]{};
        if (chosen) std::snprintf(chosen_label,sizeof(chosen_label),"%s / %s / %s %u (entrance %u)",
            chosen->world,chosen->area,chosen->silo?"silo":"pad",
            unsigned(chosen->pad_number),unsigned(chosen->entrance_id));
        if (ImGui::BeginCombo("World / Area / Entrance",chosen?chosen_label:"Select entrance")) {
            const char* last_world=nullptr;
            for (std::size_t i=0;i<tooie::practice::travel::destination_count();++i) {
                const auto* option=tooie::practice::travel::destination(i);
                if (!last_world || std::string_view(option->world)!=last_world) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s",option->world);
                    last_world=option->world;
                }
                char label[160]{};
                std::snprintf(label,sizeof(label),"%s / %s %u (map %03X, entrance %u)##practice_destination_%u",
                    option->area,option->silo?"silo":"pad",unsigned(option->pad_number),unsigned(option->map_id),
                    unsigned(option->entrance_id),unsigned(i));
                if (ImGui::Selectable(label,selected_destination==i)) selected_destination=i;
            }
            ImGui::EndCombo();
        }
        if (chosen) ImGui::TextWrapped("Selected: %s / %s / %s %u; map %03X, entrance %u.",
            chosen->world,chosen->area,chosen->silo?"silo":"pad",
            unsigned(chosen->pad_number),unsigned(chosen->map_id),unsigned(chosen->entrance_id));
        const auto travel_status=tooie::practice::travel::status();
        const bool warp_pending=travel_status.outcome==tooie::practice::travel::Outcome::Queued ||
            travel_status.outcome==tooie::practice::travel::Outcome::TransitionRequested;
        ImGui::BeginDisabled(!state.current.valid || warp_pending ||
            tooie::practice::transition_owner()!=tooie::practice::TransitionOwner::None);
        if (ImGui::Button("Warp to Selected Entrance") &&
            !tooie::practice::travel::request(selected_destination))
            tooie::menu::show_notice("Practice Warp Unavailable",
                "Warp requires active gameplay outside a cutscene or transition.",false);
        ImGui::EndDisabled();
        switch (travel_status.outcome) {
            case tooie::practice::travel::Outcome::Queued: ImGui::TextDisabled("Warp queued for guest update."); break;
            case tooie::practice::travel::Outcome::TransitionRequested: ImGui::TextDisabled("Transition requested; awaiting arrival."); break;
            case tooie::practice::travel::Outcome::Arrived: ImGui::TextDisabled("Arrival observed at selected map."); break;
            case tooie::practice::travel::Outcome::Rejected: ImGui::TextDisabled("Warp rejected by gameplay guard."); break;
            case tooie::practice::travel::Outcome::TimedOut: ImGui::TextDisabled("Warp timed out; arrival not observed."); break;
            default: break;
        }
        ImGui::TextWrapped(practice_process
            ? "Uses original warp-pad and Isle o' Hags silo destinations. Private-session progress is not written to the normal save; capture a state before risky transitions."
            : "Uses original warp-pad and Isle o' Hags silo destinations. Use a disposable practice save profile: transitions may affect normal progress or saves, and unsaved progress is not preserved.");
        ImGui::Separator();
        ImGui::TextUnformatted("Experimental transformation reload");
        static std::size_t selected_form=0;
        const auto* form=tooie::practice::forms::form(selected_form);
        if (ImGui::BeginCombo("Form",form?form->name:"Select form")) {
            for (std::size_t i=0;i<tooie::practice::forms::form_count();++i) {
                const auto* option=tooie::practice::forms::form(i);
                if (ImGui::Selectable(option->name,selected_form==i)) selected_form=i;
            }
            ImGui::EndCombo();
        }
        const auto form_status=tooie::practice::forms::status();
        ImGui::BeginDisabled(!state.current.valid || !form ||
            tooie::practice::transition_owner()!=tooie::practice::TransitionOwner::None);
        if (ImGui::Button("Reload Current Area as Form") && form &&
            !tooie::practice::forms::request(form->id))
            tooie::menu::show_notice("Form Reload Unavailable",
                "Use active single-player gameplay outside a cutscene or transition.",false);
        ImGui::EndDisabled();
        switch (form_status.outcome) {
            case tooie::practice::forms::Outcome::Queued: ImGui::TextDisabled("Form reload queued."); break;
            case tooie::practice::forms::Outcome::ReloadRequested: ImGui::TextDisabled("Area reload requested; awaiting form setup."); break;
            case tooie::practice::forms::Outcome::Applied:
                ImGui::TextWrapped("Form %u observed after reload (behavior state 0x%X); verify movement and assets in game.",
                    unsigned(form_status.observed_form),unsigned(form_status.observed_behavior));
                break;
            case tooie::practice::forms::Outcome::Rejected:
                ImGui::TextWrapped("Form reload rejected: %s.",
                    tooie::practice::forms::failure_label(form_status.failure));
                break;
            case tooie::practice::forms::Outcome::TimedOut: ImGui::TextDisabled("Form reload timed out; result not established."); break;
            default: break;
        }
        ImGui::TextWrapped(practice_process
            ? "Experimental: reloads this area at its current entrance, not in place. Transient progress may be lost; capture a private state first. Forms, movement, assets and cross-area behavior are not gameplay-verified."
            : "Experimental: reloads this same area at its current entrance, not in place. Transient or unsaved progress may be lost, and normal save state can change. Use a disposable practice profile. Forms are not world-filtered; special and split-character forms, movement, assets and cross-area behavior are not gameplay-verified.");
        ImGui::Separator();
        ImGui::TextUnformatted("Alignment target");
        ImGui::TextWrapped("Mark your current position and facing to compare them with the Practice overlay. The target lasts for this session only.");
        if (!state.current.valid) ImGui::TextDisabled("Waiting for a player position in game.");
        ImGui::BeginDisabled(!state.current.valid);
        if (ImGui::Button("Mark Alignment")) {
            if (!tooie::practice::mark_alignment())
                tooie::menu::show_notice("Alignment Not Marked",
                    "A current player position is not available yet.",false);
        }
        ImGui::EndDisabled();
        if (state.target_valid) {
            ImGui::SameLine();
            if (ImGui::Button("Clear Alignment")) tooie::practice::clear_alignment();
            ImGui::TextWrapped("Target marked in %s.",state.target_same_map?"this area":"another area");
        } else ImGui::TextDisabled("No alignment target marked.");
        ImGui::TextWrapped("Choose Practice in General > Diagnostics > Overlay, or press F3 to cycle to it.");
    } else tooie::frontend::cheats::draw_tools(tools_category);
}

void draw_about() {
    ImGui::TextUnformatted("Banjo-Tooie: Recompiled");
    ImGui::Text("Version %s",tooie::build_identity::version);
    ImGui::TextWrapped("An independent native recompilation. Bring your own supported NTSC-U 1.0 ROM.");
    ImGui::Separator();
    ImGui::TextWrapped("Mouse, keyboard, D-pad and left stick navigate settings. Enter or controller A confirms. Escape or controller Back opens and closes settings in game.");
    ImGui::TextWrapped(practice_process
        ? "F3 cycles diagnostics, F4 marks a graphics issue, F5 requests an in-game save that remains private in memory, F6 holds fast forward, and F7 requests the original intro skip. Use Tools > Practice > Save State for a durable practice state. Controls can be rebound."
        : "F3 cycles diagnostics, F4 writes a log issue marker, F5 requests an ordinary in-game save, F6 holds fast forward, and F7 requests the original supported intro skip. Controls can be rebound.");
    const auto logs=(profile_root/"logs").u8string();
    ImGui::TextWrapped("Logs: %s",reinterpret_cast<const char*>(logs.c_str()));
    ImGui::TextWrapped("When sharing logs, remove personal paths and never include ROMs or saves.");
    ImGui::Separator();
    float scale=ui_scale;
    if (ImGui::SliderFloat("UI Scale",&scale,.85f,1.35f,"%.2fx")) {
        ui_scale=scale;
        tooie::menu::set_number("tooie_ui_scale",double(scale));
    }
    ImGui::TextWrapped("Adjusts text and controls for this display. The menu stays opaque over gameplay.");
}

void draw_notice() {
    Notice current;
    {
        std::lock_guard lock(queue_mutex);
        if (notices.empty()) return;
        current=notices.front();
        if (!notice_open) {
            notice_open=true;
            notice_deadline=current.seconds>0.0
                ? std::chrono::steady_clock::now()+std::chrono::milliseconds(int(current.seconds*1000.0))
                : std::chrono::steady_clock::time_point{};
        }
    }
    if (notice_open) ImGui::OpenPopup("Notice");
    prepare_modal(570.0f);
    if (ImGui::BeginPopupModal("Notice",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(current.title.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s",current.text.c_str());
        bool close=ImGui::Button("OK",ImVec2(110*ui_scale,0));
        if (notice_deadline!=std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now()>notice_deadline) close=true;
        if (close) {
            std::lock_guard lock(queue_mutex);
            if (!notices.empty()) notices.pop_front();
            notice_active.store(!notices.empty(),std::memory_order_release);
            notice_open=false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
}

namespace tooie::menu {
void initialize(const std::filesystem::path& root, Log log,
    bool start_game, bool persistent_practice) {
    relaunch_mode=RelaunchMode::None;
    practice_process=persistent_practice;
    pending_practice=false;
    pending_save_state=false;
    pending_load_state=false;
    profile_root=root;
    logger=std::move(log);
    frontend::settings::initialize(root/"config");
    general_category=std::clamp(get_int("tooie_general_category",0),0,3);
    graphics_category=std::clamp(get_int("tooie_graphics_category",0),0,3);
    tools_category=std::clamp(get_int("tooie_tools_category",0),0,7);
    std::u8string game_id=u8"bt.n64.us.1.0";
    rom_ready.store(recomp::is_rom_valid(game_id),std::memory_order_release);
    apply_graphics();
    camera::set_analog_enabled(get_bool("tooie_analog_camera_enabled",false));
    camera::set_horizontal_inverted(get_bool("tooie_camera_invert_x",false));
    camera::set_vertical_inverted(get_bool("tooie_camera_invert_y",false));
    music::set_percent(unsigned(get_number("tooie_music_volume",100.0)));
    timing::set_fast_forward_rate(unsigned(get_int("tooie_fast_forward_rate",2)));
    // Trail Radar is preserved in source/config for future work but disabled in
    // the pre-alpha UI even when an older profile saved it as enabled.
    minimap::set_mode(minimap::Mode::Off);
    minimap::set_corner(static_cast<minimap::Corner>(get_int("tooie_minimap_corner",0)));
    minimap::set_heading(static_cast<minimap::Heading>(get_int("tooie_minimap_heading",0)));
    diagnostics::overlay::set_mode(static_cast<diagnostics::overlay::Mode>(get_int("tooie_diagnostics_overlay",0)));
    diagnostics::overlay::set_corner(static_cast<diagnostics::overlay::Corner>(get_int("tooie_diagnostics_corner",1)));
    frontend::prompt::set_visible(get_bool("tooie_mapped_prompt_legend",false));
    frontend::prompt::set_corner(static_cast<frontend::prompt::Corner>(get_int("tooie_mapped_prompt_corner",3)));
    diagnostics::overlay::set_issue_marker_sink([log=logger](std::string_view summary) {
        if (!log) return false;
        log("issue_marker",{{"summary",std::string(summary)}});
        return true;
    });
    frontend::cheats::initialize();
    menu_open.store(true,std::memory_order_release);
    if (start_game) enqueue({TaskKind::Start,{}});
}

RelaunchMode relaunch_requested() noexcept { return relaunch_mode; }

bool shutdown() {
    const bool settings_saved=frontend::settings::flush();
    diagnostics::overlay::shutdown();
    minimap::shutdown();
    logger={};
    return settings_saved;
}

void tick() {
    std::deque<Task> local;
    { std::lock_guard lock(queue_mutex); local.swap(tasks); }
    std::u8string game_id=u8"bt.n64.us.1.0";
    for (Task task:local) {
#ifdef _WIN32
        if (task.kind==TaskKind::Browse) {
            if (ultramodern::is_game_started()) continue;
            try { task.path=native_host::choose_rom_file(); }
            catch (const std::exception& error) {
                show_notice("Could Not Open File Picker",error.what(),false);
                continue;
            }
            if (task.path.empty()) continue;
            task.kind=TaskKind::Import;
        }
#endif
        if (task.kind==TaskKind::Import) {
            if (ultramodern::is_game_started()) { show_notice("ROM import","Game already started.",false); continue; }
            const auto error=recomp::select_rom(task.path,game_id);
            if (error==recomp::RomValidationError::Good) {
                rom_ready.store(true,std::memory_order_release);
                show_notice("ROM Ready","Supported NTSC-U 1.0 ROM validated and stored in this profile.");
                if (logger) logger("launcher_rom_import",{{"success",true}});
            } else {
                std::string explanation="Could not import this ROM. Check that the path exists and the file is a supported NTSC-U 1.0 ROM.";
                if (error==recomp::RomValidationError::FailedToOpen) explanation="Could not open the selected file. Check its path and permissions.";
                else if (error==recomp::RomValidationError::NotARom) explanation="The selected file is not a valid N64 ROM.";
                else if (error==recomp::RomValidationError::IncorrectVersion) explanation="This ROM is a different version. NTSC-U 1.0 is required.";
                else if (error==recomp::RomValidationError::IncorrectRom) explanation="This is a different game ROM. Banjo-Tooie NTSC-U 1.0 is required.";
                else if (error==recomp::RomValidationError::NotYet) explanation="This ROM version is not supported yet. NTSC-U 1.0 is required.";
                else if (error==recomp::RomValidationError::OtherError) explanation="The ROM could not be validated. Check the file and try again.";
                show_notice("ROM Not Accepted",explanation,false);
                if (logger) logger("launcher_rom_import",{{"success",false},{"error",int(error)}});
            }
        } else if (task.kind==TaskKind::Start) {
            if (!ultramodern::is_game_started() && rom_ready.load(std::memory_order_acquire)) {
                apply_graphics();
                recomp::start_game(game_id,"");
                menu_open.store(false,std::memory_order_release);
                if (logger) logger("launcher_start",{{"game_id","bt.n64.us.1.0"}});
            } else show_notice("Cannot Start","Import a supported ROM first.",false);
        } else if (task.kind==TaskKind::Reset) {
            if (ultramodern::is_game_started()) { show_notice("Reset Canceled","The game has already started. No save was changed.",false); continue; }
            const auto save_path=profile_root/"saves"/"bt.n64.us.1.0.bin";
            const auto result=cheat_save_reset::disable_active_cheats(save_path,task.slot);
            if (logger) logger("launcher_cheat_reset",{{"slot",task.slot},{"success",result.success},
                {"changed",result.changed},{"reason",result.message},{"backup",result.backup_path.string()}});
            if (!result.success) {
                show_notice("Active Cheats Were Not Turned Off",result.message+" Cheat access remains as it was.",false);
            } else {
                const bool saved=frontend::cheats::revoke_access_from_launcher();
                std::string text=result.message;
                if (!result.backup_path.empty()) text+=" Backup: "+result.backup_path.string()+".";
                text+=saved ? " Permanent progress and other files remain. Start Game to use this file with active effects off."
                    : " Effects are off now, but cheat access could not be saved for the next launch.";
                show_notice(saved?"Active Cheats Turned Off":"Permission Save Failed",text,saved);
            }
        } else if (task.kind==TaskKind::Restart || task.kind==TaskKind::ReturnToLauncher ||
            task.kind==TaskKind::StartPractice || task.kind==TaskKind::Quit) {
            if ((task.kind==TaskKind::Restart || task.kind==TaskKind::ReturnToLauncher) &&
                !ultramodern::is_game_started()) continue;
            if (task.kind==TaskKind::StartPractice &&
                (!rom_ready.load(std::memory_order_acquire) || practice_process)) {
                show_notice("Practice Session Unavailable",
                    "Select a supported ROM before starting a new private practice session.",false);
                continue;
            }
            SDL_Event quit{}; quit.type=SDL_QUIT;
            if (SDL_PushEvent(&quit)!=1) {
                show_notice("Could Not Close Game","Please try again. The game is still running.",false);
                continue;
            }
            if (task.kind==TaskKind::Restart)
                relaunch_mode=practice_process?RelaunchMode::PracticeGame:RelaunchMode::RestartGame;
            else if (task.kind==TaskKind::ReturnToLauncher) relaunch_mode=RelaunchMode::ReturnToLauncher;
            else if (task.kind==TaskKind::StartPractice) relaunch_mode=RelaunchMode::PracticeGame;
            else relaunch_mode=RelaunchMode::None;
            if (logger && relaunch_mode!=RelaunchMode::None)
                logger(relaunch_mode==RelaunchMode::PracticeGame
                    ? "launcher_private_practice_requested"
                    : relaunch_mode==RelaunchMode::RestartGame
                        ? "launcher_restart_requested" : "launcher_return_requested",{});
            break;
        }
    }
    frontend::settings::flush();
}

void handle_event(const SDL_Event& event) {
    if (event.type==SDL_DROPFILE && event.drop.file) {
        const std::string path=event.drop.file;
        SDL_free(event.drop.file);
        if (!ultramodern::is_game_started()) {
            const auto utf8=std::u8string(reinterpret_cast<const char8_t*>(path.c_str()));
            enqueue({TaskKind::Import,std::filesystem::path(utf8)});
        }
    }
}
bool is_open() noexcept { return menu_open.load(std::memory_order_acquire) || notice_active.load(std::memory_order_acquire); }
void set_open(bool open) noexcept { menu_open.store(open,std::memory_order_release); }
void show_notice(std::string title,std::string text,bool success,double seconds) {
    std::lock_guard lock(queue_mutex);
    notices.push_back({std::move(title),std::move(text),success,seconds});
    notice_active.store(true,std::memory_order_release);
}

void draw() {
    diagnostics::overlay::poll_input();
    const auto size=ImGui::GetIO().DisplaySize;
    ui_scale=std::clamp(float(get_number("tooie_ui_scale",1.0)),.85f,1.35f);
    ImGui::GetIO().FontGlobalScale=ui_scale;
    const bool game_started=ultramodern::is_game_started();
    const bool open=menu_open.load(std::memory_order_acquire) || !game_started;
    // The guest remains visible beneath the host menu, but diagnostics must
    // never draw over the menu or a confirmation/notice dialog.
    if (game_started && !is_open()) {
        diagnostics::overlay::render(int(size.x),int(size.y));
        frontend::prompt::render(int(size.x),int(size.y),
            diagnostics::overlay::mode()==diagnostics::overlay::Mode::Practice &&
                diagnostics::overlay::visible(),
            static_cast<frontend::prompt::Corner>(diagnostics::overlay::corner()));
    }
    if (open) {
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.075f,.10f,.12f,.98f));
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.157f,.094f,.812f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.337f,.651f,1.0f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,ImVec4(.592f,.102f,.239f,1));
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.157f,.094f,.812f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.337f,.651f,1.0f,1));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(1.0f,.094f,.169f,1));
        ImGui::Begin("Banjo-Tooie: Recompiled",nullptr,
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextColored(ImVec4(.337f,.651f,1.0f,1),"BANJO-TOOIE  /  RECOMPILED");
        if (game_started) {
            const auto& style=ImGui::GetStyle();
            const float actions_width=ImGui::CalcTextSize("Resume Game").x+
                ImGui::CalcTextSize("Restart Game").x+
                ImGui::CalcTextSize("Return to Launcher").x+
                6.0f*style.FramePadding.x+2.0f*style.ItemSpacing.x;
            const float right_x=std::max(style.WindowPadding.x,
                ImGui::GetWindowWidth()-style.WindowPadding.x-actions_width);
            const float brand_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
            if (brand_end+style.ItemSpacing.x<=right_x) ImGui::SameLine();
            ImGui::SetCursorPosX(right_x);
            if (ImGui::Button("Resume Game")) set_open(false);
            ImGui::SameLine();
            if (ImGui::Button("Restart Game")) pending_restart=true;
            ImGui::SameLine();
            if (ImGui::Button("Return to Launcher")) pending_return=true;
        }
        if (practice_process)
            ImGui::TextColored(ImVec4(1.0f,.72f,.24f,1.0f),
                "PRIVATE PRACTICE - NORMAL SAVES OFF - Tools > Practice: Save/Load State");
        const auto recovery = frontend::settings::recovery_notice();
        if (!recovery.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,.72f,.24f,1.0f));
            ImGui::TextWrapped("%.*s", int(recovery.size()), recovery.data());
            ImGui::PopStyleColor();
        }
        ImGui::SetNextItemWidth(std::clamp(size.x*.23f,150.0f,230.0f));
        ImGui::InputTextWithHint("##settings_search","Search settings",search_text,sizeof(search_text));
        ImGui::Separator();
        const float sidebar=std::clamp(size.x*.20f,150.0f*ui_scale,190.0f*ui_scale);
        ImGui::BeginChild("categories",ImVec2(sidebar,0),true);
        const std::array<std::pair<Page,const char*>,7> pages{{
            {Page::Home,"Play"},{Page::General,"General"},{Page::Graphics,"Graphics"},
            {Page::Controls,"Controls"},{Page::Sound,"Sound"},{Page::Tools,"Tools"},
            {Page::About,"Help & About"}}};
        for (const auto& [target,label]:pages)
            if (ImGui::Selectable(label,page==target,0,ImVec2(0,35*ui_scale))) page=target;
        ImGui::Separator();
        ImGui::TextWrapped("Cheats: %s",features::cheats_access_enabled()?"Enabled":"Hidden");
        ImGui::Separator();
        ImGui::TextDisabled("v%s",tooie::build_identity::version);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("page",ImVec2(0,0),true);
        const char* title="Play";
        switch(page) { case Page::General:title="General";break; case Page::Graphics:title="Graphics";break;
            case Page::Controls:title="Controls";break; case Page::Sound:title="Sound";break;
            case Page::Tools:title="Tools";break;case Page::About:title="Help & About";break;default:break; }
        ImGui::TextColored(ImVec4(.337f,.651f,1.0f,1),"%s",title);
        ImGui::Separator();
        if (search_text[0]) draw_search_results();
        else switch(page) {
            case Page::Home:draw_home();break;
            case Page::General:draw_general();break;
            case Page::Graphics:draw_graphics();break;
            case Page::Controls:draw_controls();break;
            case Page::Sound:draw_sound();break;
            case Page::Tools:draw_tools();break;
            case Page::About:draw_about();break;
        }
        ImGui::EndChild();
        draw_lifecycle_confirmations();
        ImGui::End();
        ImGui::PopStyleColor(7);
    }
    draw_notice();
}

bool get_bool(std::string_view id,bool fallback) { return frontend::settings::get_bool(id,fallback); }
int get_int(std::string_view id,int fallback) { return frontend::settings::get_int(id,fallback); }
double get_number(std::string_view id,double fallback) { return frontend::settings::get_number(id,fallback); }
std::string get_string(std::string_view id,std::string_view fallback) { return frontend::settings::get_string(id,fallback); }
void set_bool(std::string_view id,bool value) { frontend::settings::set_bool(id,value); }
void set_int(std::string_view id,int value) { frontend::settings::set_int(id,value); }
void set_number(std::string_view id,double value) { frontend::settings::set_number(id,value); }
void set_string(std::string_view id,std::string value) { frontend::settings::set_string(id,std::move(value)); }
}
