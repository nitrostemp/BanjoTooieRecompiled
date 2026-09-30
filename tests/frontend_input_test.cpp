#include "frontend_input_preference.hpp"
#include "imgui_menu.hpp"
#include "free_camera.hpp"
#include "virtual_clock.hpp"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
bool opened = true;
bool started = true;
std::unordered_map<std::string, int> saved_ints;
std::unordered_map<std::string, bool> saved_bools;
std::unordered_map<std::string, std::string> saved_strings;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
SDL_Event key_event(Uint32 type, SDL_Scancode scan) {
    SDL_Event event{};
    event.type = type;
    event.key.type = type;
    event.key.keysym.scancode = scan;
    return event;
}
struct LegacyFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("tooie-legacy-input-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LegacyFixture() {
        std::filesystem::create_directories(root);
        std::ofstream output(root / "controls.json");
        output << R"({"profiles":[{"key":"controller_sp","mappings":{"A":[{"input_type":3,"input_id":2}]}},{"key":"aardvark","mappings":{"A":[{"input_type":3,"input_id":3}]}},{"key":"second","mappings":{"A":[{"input_type":3,"input_id":4}]}}],"controllers":[{"guid":{"serial":"","vendor":4660,"product":22136,"version":0,"crc16":0},"profile":"aardvark"},{"guid":{"serial":"","vendor":17186,"product":34662,"version":0,"crc16":0},"profile":"second"}]})";
        require(bool(output), "legacy fixture write failed");
    }
    ~LegacyFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
}

namespace tooie::menu {
bool is_open() noexcept { return opened; }
void set_open(bool value) noexcept { opened = value; }
bool get_bool(std::string_view id, bool fallback) {
    const auto it = saved_bools.find(std::string(id));
    return it == saved_bools.end() ? fallback : it->second;
}
int get_int(std::string_view id, int fallback) {
    const auto it = saved_ints.find(std::string(id));
    return it == saved_ints.end() ? fallback : it->second;
}
double get_number(std::string_view, double fallback) { return fallback; }
std::string get_string(std::string_view id, std::string_view fallback) {
    const auto it = saved_strings.find(std::string(id));
    return it == saved_strings.end() ? std::string(fallback) : it->second;
}
void set_int(std::string_view id, int value) { saved_ints[std::string(id)] = value; }
void set_bool(std::string_view id, bool value) { saved_bools[std::string(id)] = value; }
void set_string(std::string_view id, std::string value) { saved_strings[std::string(id)] = std::move(value); }
}
namespace ultramodern { bool is_game_started() { return started; } }
namespace tooie::timing {
void set_fast_forward_rate(std::uint32_t) noexcept {}
void set_fast_forward_held(bool) noexcept {}
}
namespace tooie::camera {
bool free_camera_enabled() noexcept { return false; }
void set_free_camera_input_active(bool) noexcept {}
void set_free_camera_motion(float, float, float) noexcept {}
void request_free_camera_reset() noexcept {}
}
namespace tooie::save_progress { void request() noexcept {} }
namespace tooie::cutscene_tools { void request_skip() noexcept {} }
namespace tooie::frontend::cheats { void trigger_graphics_issue_marker() noexcept {} }

int main() {
    try {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) == 0,
            "SDL dummy initialization failed");
        auto* window = SDL_CreateWindow("input test", 0, 0, 64, 64, SDL_WINDOW_SHOWN);
        require(window != nullptr && SDL_GetKeyboardFocus() == window, "dummy window lacks keyboard focus");
        const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        require(device >= 0 && SDL_IsGameController(device), "virtual SDL controller unavailable");
        auto* virtual_pad = SDL_JoystickOpen(device);
        require(virtual_pad != nullptr, "virtual SDL joystick open failed");

        tooie::input::initialize_settings(std::filesystem::temp_directory_path() / "tooie-input-no-legacy");
        // Each discrete marker press must be delivered, even when the game
        // thread has not polled between the UI-thread posts.
        tooie::input::post_issue_marker();
        tooie::input::post_issue_marker();
        tooie::input::post_issue_marker();
        require(tooie::input::consume_issue_marker() &&
                tooie::input::consume_issue_marker() &&
                tooie::input::consume_issue_marker() &&
                !tooie::input::consume_issue_marker(),
            "multiple issue markers were collapsed before consumption");
        using tooie::input::Action;
        using tooie::input::Device;
        const auto automatic_instance = tooie::input::selected_controller_instance();
        require(tooie::input::controller_auto_selection() && automatic_instance >= 0,
            "no-preference startup did not select a controller in Auto mode");
        SDL_VirtualJoystickDesc hotplug_desc{};
        hotplug_desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        hotplug_desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        hotplug_desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        hotplug_desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        hotplug_desc.vendor_id = 0x2345;
        hotplug_desc.product_id = 0x6789;
        hotplug_desc.name = "Auto hotplug controller";
        const int hotplug_index = SDL_JoystickAttachVirtualEx(&hotplug_desc);
        require(hotplug_index >= 0, "Auto hotplug fixture unavailable");
        SDL_Event hotplug_added{};
        hotplug_added.type = SDL_CONTROLLERDEVICEADDED;
        hotplug_added.cdevice.which = hotplug_index;
        tooie::input::handle_event(hotplug_added);
        require(tooie::input::selected_controller_instance() == automatic_instance,
            "Auto mode dropped or changed the routed controller on hotplug");
        const auto hotplug_instance = SDL_JoystickGetDeviceInstanceID(hotplug_index);
        SDL_JoystickDetachVirtual(hotplug_index);
        SDL_Event hotplug_removed{};
        hotplug_removed.type = SDL_CONTROLLERDEVICEREMOVED;
        hotplug_removed.cdevice.which = hotplug_instance;
        tooie::input::handle_event(hotplug_removed);
        require(tooie::input::get_binding(Action::A, Device::Keyboard).code == SDL_SCANCODE_J,
            "project A default changed");
        require(tooie::input::get_binding(Action::MoveUp, Device::AxisNegative).code == SDL_CONTROLLER_AXIS_LEFTY,
            "left-stick movement is not exposed as a binding");
        require(tooie::input::get_binding(Action::DLeft, Device::Keyboard).code != SDL_SCANCODE_J,
            "D-pad default collides with A");
        require(tooie::input::select_controller(SDL_JoystickInstanceID(virtual_pad)),
            "manual virtual-controller selection failed");
        require(!tooie::input::controller_auto_selection(), "manual selection still reports Auto mode");
        const auto first_info = tooie::input::controllers();
        const auto selected = std::find_if(first_info.begin(), first_info.end(), [](const auto& item) {
            return item.selected;
        });
        require(selected != first_info.end() && !selected->stable_id.empty() &&
            selected->instance == SDL_JoystickInstanceID(virtual_pad),
            "selected controller identity was not retained");
        require(selected->friendly_name == selected->name &&
            tooie::input::selected_friendly_name() == selected->name,
            "fresh controller has no reported-name fallback");
        require(tooie::input::select_controller(-1) && tooie::input::selected_controller_instance() >= 0 &&
            tooie::input::preferred_device_id().empty() && tooie::input::controller_auto_selection(),
            "Auto selection did not clear saved identity");
        require(tooie::input::select_controller(SDL_JoystickInstanceID(virtual_pad)) &&
            !tooie::input::preferred_device_id().empty(), "manual selection did not save stable identity");
        tooie::input::set_selected_controller_type(tooie::input::ControllerType::DualSense);
        tooie::input::set_selected_friendly_name("Blue controller");
        require(tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_A},
            tooie::input::ControllerType::DualSense) == "Cross", "DualSense label missing");
        require(tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_A},
            tooie::input::ControllerType::N64) == "SDL Button 0", "N64 adapter label guesses a button");
        require(tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_START},
            tooie::input::ControllerType::DualSense) == "Options" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_BACK},
            tooie::input::ControllerType::DualSense) == "Create / Share" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
            tooie::input::ControllerType::DualSense) == "L1" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_LEFTSTICK},
            tooie::input::ControllerType::DualSense) == "L3" &&
            tooie::input::binding_label_for_type({Device::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERLEFT},
            tooie::input::ControllerType::DualSense) == "L2 +",
            "DualSense non-face binding labels are generic");
        require(tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_START},
            tooie::input::ControllerType::NintendoPro) == "Plus" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_BACK},
            tooie::input::ControllerType::NintendoPro) == "Minus" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            tooie::input::ControllerType::NintendoPro) == "R" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
            tooie::input::ControllerType::NintendoPro) == "Right Stick Click" &&
            tooie::input::binding_label_for_type({Device::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
            tooie::input::ControllerType::NintendoPro) == "ZR +",
            "Nintendo Pro non-face binding labels are generic");
        require(tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
            tooie::input::ControllerType::Steam) == "Left Bumper" &&
            tooie::input::binding_label_for_type({Device::Button, SDL_CONTROLLER_BUTTON_START},
            tooie::input::ControllerType::Xbox) == "Menu" &&
            tooie::input::binding_label_for_type({Device::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERLEFT},
            tooie::input::ControllerType::N64) == "SDL Axis 4 +",
            "Steam/Xbox/raw adapter labels are generic or misleading");
        require(tooie::input::create_controller_profile("Alternate"), "profile creation failed");
        tooie::input::set_binding(Action::A, {Device::Button, SDL_CONTROLLER_BUTTON_B});
        require(tooie::input::select_controller_profile("Default") &&
            tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_A,
            "profile switch did not restore its own mapping");
        require(tooie::input::select_controller_profile("Alternate") &&
            tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_B,
            "profile switch lost alternate mapping");
        require(tooie::input::rename_controller_profile("Custom") &&
            tooie::input::active_controller_profile() == "Custom", "profile rename failed");
        tooie::input::reset_active_controller_profile();
        require(tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_A,
            "profile reset did not restore controller default");
        require(tooie::input::get_binding(Action::A, Device::Keyboard).code == SDL_SCANCODE_J,
            "controller profile changed keyboard bindings");
        const int duplicate = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        require(duplicate >= 0, "second virtual controller unavailable");
        auto* duplicate_pad = SDL_JoystickOpen(duplicate);
        require(duplicate_pad != nullptr, "second virtual joystick open failed");
        SDL_Event added{};
        added.type = SDL_CONTROLLERDEVICEADDED;
        added.cdevice.which = duplicate;
        tooie::input::handle_event(added);
        require(tooie::input::select_controller(SDL_JoystickInstanceID(duplicate_pad)),
            "duplicate controller manual selection failed");
        tooie::input::set_binding(Action::A, {Device::Button, SDL_CONTROLLER_BUTTON_Y});
        require(tooie::input::select_controller(SDL_JoystickInstanceID(virtual_pad)) &&
            tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_A,
            "indistinguishable devices shared live mappings");
        tooie::input::begin_capture(Action::B, Device::Button);
        tooie::input::tick(window, false, true);
        SDL_Event foreign_button{};
        foreign_button.type = SDL_CONTROLLERBUTTONDOWN;
        foreign_button.cbutton.which = SDL_JoystickInstanceID(duplicate_pad);
        foreign_button.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
        tooie::input::handle_event(foreign_button);
        require(tooie::input::capture_active(), "another controller stole binding capture");
        tooie::input::cancel_capture();
        tooie::input::tick(window, false, true);
        SDL_Delay(270);
        tooie::input::tick(window, false, true);
        SDL_JoystickClose(duplicate_pad);
        SDL_JoystickDetachVirtual(duplicate);
        SDL_Event duplicate_removed{};
        duplicate_removed.type = SDL_CONTROLLERDEVICEREMOVED;
        duplicate_removed.cdevice.which = foreign_button.cbutton.which;
        tooie::input::handle_event(duplicate_removed);
        require(tooie::input::select_controller(SDL_JoystickInstanceID(virtual_pad)),
            "unique controller selection after duplicate removal failed");
        SDL_Event selected_removed{};
        selected_removed.type = SDL_CONTROLLERDEVICEREMOVED;
        selected_removed.cdevice.which = SDL_JoystickInstanceID(virtual_pad);
        tooie::input::handle_event(selected_removed);
        require(tooie::input::selected_controller_instance() == -1,
            "removed selected device remained routed");
        int reconnect_index = -1;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == SDL_JoystickInstanceID(virtual_pad)) reconnect_index = i;
        require(reconnect_index >= 0, "selected virtual device was not available to reconnect");
        SDL_Event selected_added{};
        selected_added.type = SDL_CONTROLLERDEVICEADDED;
        selected_added.cdevice.which = reconnect_index;
        tooie::input::handle_event(selected_added);
        require(tooie::input::selected_controller_instance() == SDL_JoystickInstanceID(virtual_pad),
            "unique preferred device did not restore after reconnect");

        tooie::input::begin_capture(Action::A, Device::Keyboard);
        tooie::input::handle_event(key_event(SDL_KEYDOWN, SDL_SCANCODE_P));
        require(tooie::input::capture_active(),
            "capture accepted the input that opened it before neutral arming");
        tooie::input::tick(window, false, true);
        tooie::input::handle_event(key_event(SDL_KEYDOWN, SDL_SCANCODE_P));
        require(!tooie::input::capture_active() &&
                tooie::input::get_binding(Action::A, Device::Keyboard).code == SDL_SCANCODE_P,
            "keyboard remap capture failed");
        require(tooie::input::input_suppressed(), "capture released navigation before cooldown");
        tooie::input::tick(window, false, true);
        SDL_Delay(270);
        tooie::input::tick(window, false, true);
        require(!tooie::input::input_suppressed(), "capture did not release after neutral cooldown");
        opened = false;
        tooie::input::tick(window, true, false);
        SDL_Event pending{};
        while (SDL_PollEvent(&pending)) {} // Discard setup/hotplug events.
        auto down = key_event(SDL_KEYDOWN, SDL_SCANCODE_P);
        auto up = key_event(SDL_KEYUP, SDL_SCANCODE_P);
        require(SDL_PushEvent(&down) == 1 && SDL_PushEvent(&up) == 1,
            "SDL event queue rejected the key tap");
        while (SDL_PollEvent(&pending)) tooie::input::handle_event(pending);
        tooie::input::tick(window, true, false);
        require((tooie::input::consume_transient_buttons() & 0x8000) != 0,
            "short remapped A tap was lost before guest input");
        require(tooie::input::consume_transient_buttons() == 0, "tap was delivered more than once");

        opened = true;
        tooie::input::tick(window, true, true);
        tooie::input::handle_event(key_event(SDL_KEYDOWN, SDL_SCANCODE_P));
        require(tooie::input::consume_transient_buttons() == 0 &&
                tooie::input::snapshot().buttons == 0, "menu leaked gameplay key input");

        require(SDL_JoystickSetVirtualButton(virtual_pad, SDL_CONTROLLER_BUTTON_A, 1) == 0,
            "virtual controller button press failed");
        tooie::input::tick(window, true, true);
        opened = false;
        tooie::input::tick(window, true, false);
        require((tooie::input::snapshot().buttons & 0x8000) == 0,
            "held menu button leaked into gameplay on close");
        require(SDL_JoystickSetVirtualButton(virtual_pad, SDL_CONTROLLER_BUTTON_A, 0) == 0,
            "virtual controller button release failed");
        tooie::input::tick(window, true, false);
        require(SDL_JoystickSetVirtualButton(virtual_pad, SDL_CONTROLLER_BUTTON_A, 1) == 0,
            "second controller press failed");
        tooie::input::tick(window, true, false);
        require((tooie::input::snapshot().buttons & 0x8000) != 0,
            "fresh controller press was suppressed after release");
        SDL_JoystickSetVirtualButton(virtual_pad, SDL_CONTROLLER_BUTTON_A, 0);

        require(SDL_JoystickSetVirtualAxis(virtual_pad, SDL_CONTROLLER_AXIS_LEFTY, -22000) == 0,
            "virtual left stick update failed");
        tooie::input::tick(window, true, false);
        require(tooie::input::snapshot().y > 0.5f, "left-stick up has wrong N64 sign or magnitude");
        SDL_JoystickSetVirtualAxis(virtual_pad, SDL_CONTROLLER_AXIS_LEFTY, 0);
        tooie::input::tick(window, true, false);

        opened = true;
        tooie::input::begin_capture(Action::B, Device::Button);
        tooie::input::tick(window, false, true);
        SDL_Event axis_event{};
        axis_event.type = SDL_CONTROLLERAXISMOTION;
        axis_event.caxis.which = SDL_JoystickInstanceID(virtual_pad);
        axis_event.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTX;
        axis_event.caxis.value = 25000;
        tooie::input::handle_event(axis_event);
        require(!tooie::input::capture_active() &&
                tooie::input::get_binding(Action::B, Device::AxisPositive).code == SDL_CONTROLLER_AXIS_RIGHTX,
            "controller axis remap capture failed");

        tooie::input::shutdown();
        tooie::input::initialize_settings(std::filesystem::temp_directory_path() / "tooie-input-no-legacy");
        require(tooie::input::select_controller(SDL_JoystickInstanceID(virtual_pad)),
            "selected virtual device was not available after restart");
        require(tooie::input::selected_controller_type() == tooie::input::ControllerType::DualSense &&
            tooie::input::selected_friendly_name() == "Blue controller" &&
            tooie::input::active_controller_profile() == "Custom",
            "controller type/name/profile did not survive settings reload");
        require(tooie::input::get_binding(Action::A, Device::Keyboard).code == SDL_SCANCODE_P,
            "keyboard binding did not survive settings reload");
        tooie::input::set_binding(Action::A, {Device::Button, SDL_CONTROLLER_BUTTON_Y});
        tooie::input::shutdown();
        SDL_VirtualJoystickDesc fresh_desc{};
        fresh_desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        fresh_desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        fresh_desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        fresh_desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        fresh_desc.vendor_id = 0x4321;
        fresh_desc.product_id = 0x8765;
        fresh_desc.name = "Fresh identity controller";
        const int fresh_index = SDL_JoystickAttachVirtualEx(&fresh_desc);
        require(fresh_index >= 0, "fresh identity virtual controller unavailable");
        auto* fresh_pad = SDL_JoystickOpen(fresh_index);
        require(fresh_pad != nullptr, "fresh identity joystick open failed");
        tooie::input::initialize_settings(std::filesystem::temp_directory_path() / "tooie-input-no-legacy");
        require(tooie::input::select_controller(SDL_JoystickInstanceID(fresh_pad)),
            "fresh identity controller selection failed");
        require(tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_A,
            "new device inherited another controller's global binding");
        tooie::input::shutdown();
        SDL_JoystickClose(fresh_pad);
        SDL_JoystickDetachVirtual(fresh_index);
        SDL_JoystickClose(virtual_pad);
        SDL_JoystickDetachVirtual(device);
        saved_strings.clear(); saved_ints.clear(); saved_bools.clear();
        LegacyFixture legacy;
        SDL_VirtualJoystickDesc desc{};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.vendor_id = 0x1234;
        desc.product_id = 0x5678;
        desc.name = "Legacy fixture controller";
        const int legacy_index = SDL_JoystickAttachVirtualEx(&desc);
        require(legacy_index >= 0, "legacy fixture virtual controller unavailable");
        auto* legacy_pad = SDL_JoystickOpen(legacy_index);
        require(legacy_pad != nullptr, "legacy fixture joystick open failed");
        SDL_VirtualJoystickDesc second_desc=desc;
        second_desc.vendor_id=0x4322;
        second_desc.product_id=0x8766;
        second_desc.name="Second legacy controller";
        const int second_index=SDL_JoystickAttachVirtualEx(&second_desc);
        require(second_index>=0,"second legacy controller unavailable");
        auto* second_pad=SDL_JoystickOpen(second_index);
        require(second_pad!=nullptr,"second legacy joystick open failed");
        Uint16 first_vendor=0,first_product=0,first_version=0,first_crc=0;
        Uint16 second_vendor=0,second_product=0,second_version=0,second_crc=0;
        SDL_GetJoystickGUIDInfo(SDL_JoystickGetGUID(legacy_pad),&first_vendor,&first_product,&first_version,&first_crc);
        SDL_GetJoystickGUIDInfo(SDL_JoystickGetGUID(second_pad),&second_vendor,&second_product,&second_version,&second_crc);
        {
            std::ofstream output(legacy.root/"controls.json",std::ios::trunc);
            output << R"({"profiles":[{"key":"controller_sp","mappings":{"A":[{"input_type":3,"input_id":2}]}},{"key":"aardvark","mappings":{"A":[{"input_type":3,"input_id":3}]}},{"key":"second","mappings":{"A":[{"input_type":3,"input_id":4}]}}],"controllers":[{"guid":{"serial":"","vendor":)"
                << first_vendor << R"(,"product":)" << first_product << R"(,"version":)" << first_version
                << R"(,"crc16":)" << first_crc << R"(},"profile":"aardvark"},{"guid":{"serial":"","vendor":)"
                << second_vendor << R"(,"product":)" << second_product << R"(,"version":)" << second_version
                << R"(,"crc16":)" << second_crc << R"(},"profile":"second"}]})";
            require(bool(output),"assigned legacy fixture write failed");
        }
        // Current ImGui settings are newer than controls.json and must seed the
        // migrated Default profile; legacy named profiles remain available.
        saved_ints["input.a.1.0"] = SDL_CONTROLLER_BUTTON_B;
        saved_strings["tooie_preferred_controller_guid"] = "SERIAL__VID_"+
            std::to_string(first_vendor)+"_PID_"+std::to_string(first_product)+"_VERSION_"+
            std::to_string(first_version)+"_CRC16_"+std::to_string(first_crc);
        tooie::input::initialize_settings(legacy.root);
        require(tooie::input::select_controller(SDL_JoystickInstanceID(legacy_pad)),
            "legacy fixture controller selection failed");
        require(tooie::input::active_controller_profile() == "Default" &&
            tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_B,
            "newer ImGui mapping was replaced by stale legacy Default mapping");
        const auto legacy_profiles = tooie::input::controller_profiles();
        require(std::find(legacy_profiles.begin(), legacy_profiles.end(), "aardvark") != legacy_profiles.end() &&
            tooie::input::select_controller_profile("aardvark") &&
            tooie::input::get_binding(Action::A, Device::Button).code == SDL_CONTROLLER_BUTTON_Y,
            "named legacy mapping was not retained for explicit selection");
        require(tooie::input::select_controller(SDL_JoystickInstanceID(second_pad)) &&
            tooie::input::get_binding(Action::A,Device::Button).code==SDL_CONTROLLER_BUTTON_BACK,
            "another device inherited the preferred controller's migrated mapping");
        tooie::input::shutdown();
        SDL_JoystickClose(second_pad);
        SDL_JoystickDetachVirtual(second_index);
        SDL_JoystickClose(legacy_pad);
        SDL_JoystickDetachVirtual(legacy_index);
        saved_strings["tooie_preferred_controller_id"] = "";
        saved_strings["tooie_preferred_controller_guid"] = "";
        saved_bools["tooie_controller_auto"] = false;
        tooie::input::initialize_settings(std::filesystem::temp_directory_path() / "tooie-input-no-legacy");
        require(!tooie::input::controller_auto_selection() &&
            tooie::input::selected_controller_instance() == -1,
            "manual-required selection is falsely reported or routed as Auto");
        tooie::input::shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "frontend input bridge checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frontend input bridge check failed: " << error.what() << '\n';
        return 1;
    }
}
