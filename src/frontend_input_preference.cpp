#include "frontend_input_preference.hpp"
#include "imgui_menu.hpp"
#include "frontend_cheats.hpp"
#include "cutscene_tools.hpp"
#include "free_camera.hpp"
#include "save_progress.hpp"
#include "virtual_clock.hpp"
#include "ultramodern/ultramodern.hpp"
#include "json/json.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
using namespace tooie::input;
#if defined(_WIN32)
std::string hid_product_name(const char* path) {
    if (!path || !*path) return {};
    const int path_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (path_length <= 0) return {};
    std::wstring wide_path(static_cast<size_t>(path_length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
            wide_path.data(), path_length)) return {};

    // SDL's HID path identifies this exact controller, including duplicate VID/PID devices.
    // Request metadata only; no input or output report is sent to the device.
    HANDLE device = CreateFileW(wide_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE) return {};
    HMODULE hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using GetProductString = BOOLEAN (WINAPI*)(HANDLE, PVOID, ULONG);
    auto get_product_string = hid ? reinterpret_cast<GetProductString>(
        GetProcAddress(hid, "HidD_GetProductString")) : nullptr;
    wchar_t product[128]{};
    const bool found = get_product_string &&
        get_product_string(device, product, sizeof(product)) && product[0] != L'\0';
    if (hid) FreeLibrary(hid);
    CloseHandle(device);
    if (!found) return {};
    product[sizeof(product) / sizeof(product[0]) - 1] = L'\0';
    const int name_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        product, -1, nullptr, 0, nullptr, nullptr);
    if (name_length <= 0) return {};
    std::string name(static_cast<size_t>(name_length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, product, -1,
            name.data(), name_length, nullptr, nullptr)) return {};
    name.pop_back(); // Omit the converted terminator.
    return name;
}
#endif
constexpr size_t count = static_cast<size_t>(Action::Count);
constexpr std::array<const char*, count> names{{
    "a","b","z","start","l","r","c_up","c_down","c_left","c_right",
    "d_up","d_down","d_left","d_right","move_up","move_down","move_left","move_right",
    "menu","save_progress","diagnostics","issue_marker","fast_forward","skip_cutscene",
    "free_camera_reset","free_camera_left","free_camera_right","free_camera_up",
    "free_camera_down","free_camera_forward","free_camera_back"
}};
constexpr std::array<const char*, count> labels{{
    "A", "B", "Z", "Start", "L", "R", "C Up", "C Down", "C Left", "C Right",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "Move Up", "Move Down", "Move Left", "Move Right",
    "Open Menu", "Save Progress", "Toggle Diagnostics", "Mark Graphics Issue",
    "Fast Forward (Hold)", "Skip Cutscene", "Reset Free Camera",
    "Free Camera Left", "Free Camera Right", "Free Camera Up", "Free Camera Down",
    "Free Camera Forward", "Free Camera Back"
}};
constexpr unsigned index(Device device, unsigned slot) { return unsigned(device) * 2 + slot; }
using BindingTable = std::array<std::array<Binding, 8>, count>;
std::mutex binding_mutex;
BindingTable bindings{};
std::atomic_bool scanning{false};
std::atomic_bool suppress_input{false};
std::mutex capture_mutex;
Action scanned_action = Action::A;
Device scanned_device = Device::Keyboard;
unsigned scanned_slot = 0;
bool capture_armed = false;
bool capture_releasing = false;
std::atomic<SDL_JoystickID> capture_instance{-1};
std::chrono::steady_clock::time_point capture_deadline{}, release_neutral_since{};
constexpr auto capture_timeout = std::chrono::seconds(8);
constexpr auto release_cooldown = std::chrono::milliseconds(250);
struct OpenController { SDL_GameController* handle = nullptr; ControllerInfo info; };
std::mutex controller_mutex;
std::vector<OpenController> opened;
std::string wanted_guid;
std::string wanted_device_id;
SDL_JoystickID manual_instance = -1;
bool manual_required = false;
bool wanted_auto = false;
std::atomic<SDL_JoystickID> chosen{-1};
struct DeviceRecord {
    std::string friendly_name;
    ControllerType type = ControllerType::Other;
    std::string active = "Default";
    std::map<std::string, BindingTable> profiles;
};
std::map<std::string, DeviceRecord> device_records;
std::string active_device_id;
std::string active_profile_key;
BindingTable legacy_template{};
std::string legacy_preferred_guid;
std::map<std::string, BindingTable> legacy_profile_tables;
std::map<std::string, std::string> legacy_assignments;
std::atomic_uint64_t published_buttons{0};
std::atomic_uint32_t transient_buttons{0};
std::atomic_int published_x{0}, published_y{0}, published_rx{0}, published_ry{0};
std::atomic_bool published_enabled{false}, published_physical{false}, published_rumble{false};
std::atomic_bool desired_rumble{false};
bool applied_rumble = false;
SDL_JoystickID applied_rumble_device = -1;
std::chrono::steady_clock::time_point last_rumble_update{};
MenuGamepadState published_menu_pad{};
bool last_fast_forward = false;
bool last_free_camera_reset = false;
bool last_menu_binding = false;
bool last_focused = false;
bool focus_transition_pending = false;
SDL_Window* frontend_window = nullptr; // SDL/UI thread only.
bool background_hint_initialized = false;
bool last_background_hint = false;
bool was_menu_open = true;
std::uint16_t suppressed_buttons = 0;
std::uint16_t suppressed_keyboard_buttons = 0;
bool suppress_move = false, suppress_camera = false;
bool suppress_keyboard_move = false;
bool suppress_fast_forward = false, suppress_free_camera_reset = false;
std::array<bool, count> last_utility_binding{};
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
std::atomic_bool persistent_input_reset_pending{false};
bool persistent_input_quarantine = false; // SDL/UI thread only.
#endif
std::atomic_bool diagnostics_toggle_pending{false}, issue_marker_pending{false};
std::atomic_uint64_t total_keydowns{0}, relevant_keydowns{0}, escape_keydowns{0}, return_keydowns{0},
    function_keydowns{0}, accepted_save_progress{0}, accepted_diagnostics{0}, accepted_issue_markers{0},
    accepted_fast_forward{0}, accepted_cutscene_skips{0};
std::atomic_uint32_t last_scancode{0};
std::atomic_bool last_repeat{false}, last_game_started{false}, last_keyboard_focus{false},
    last_all_input_disabled{false}, last_context_capture{false}, last_binding_scan{false}, last_skip_events{false};

void sync_background_joystick_events(bool enabled) {
    if (!background_hint_initialized || last_background_hint != enabled) {
        // SDL's default is off. This must be set before polling/updating the
        // controller; publishing its cached state alone cannot restore input.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, enabled ? "1" : "0");
        last_background_hint = enabled;
        background_hint_initialized = true;
    }
}

std::string setting_key(Action action, Device device, unsigned slot) {
    return std::string("input.") + names[static_cast<size_t>(action)] + "." +
        std::to_string(unsigned(device)) + "." + std::to_string(slot);
}
void put(BindingTable& table, Action action, Device device, int code, unsigned slot = 0) {
    table[static_cast<size_t>(action)][index(device, slot)] = {device, code};
}
BindingTable defaults() {
    BindingTable table{};
    for (auto& row : table) for (unsigned i = 0; i < row.size(); ++i) row[i] = {Device(i / 2), -1};
    put(table, Action::A, Device::Keyboard, SDL_SCANCODE_J);
    put(table, Action::B, Device::Keyboard, SDL_SCANCODE_K);
    put(table, Action::Z, Device::Keyboard, SDL_SCANCODE_SPACE);
    put(table, Action::Start, Device::Keyboard, SDL_SCANCODE_RETURN);
    put(table, Action::L, Device::Keyboard, SDL_SCANCODE_Q);
    put(table, Action::R, Device::Keyboard, SDL_SCANCODE_E);
    put(table, Action::CUp, Device::Keyboard, SDL_SCANCODE_UP);
    put(table, Action::CDown, Device::Keyboard, SDL_SCANCODE_DOWN);
    put(table, Action::CLeft, Device::Keyboard, SDL_SCANCODE_LEFT);
    put(table, Action::CRight, Device::Keyboard, SDL_SCANCODE_RIGHT);
    put(table, Action::DUp, Device::Keyboard, SDL_SCANCODE_T);
    put(table, Action::DDown, Device::Keyboard, SDL_SCANCODE_G);
    put(table, Action::DLeft, Device::Keyboard, SDL_SCANCODE_F);
    put(table, Action::DRight, Device::Keyboard, SDL_SCANCODE_H);
    put(table, Action::MoveUp, Device::Keyboard, SDL_SCANCODE_W);
    put(table, Action::MoveDown, Device::Keyboard, SDL_SCANCODE_S);
    put(table, Action::MoveLeft, Device::Keyboard, SDL_SCANCODE_A);
    put(table, Action::MoveRight, Device::Keyboard, SDL_SCANCODE_D);
    put(table, Action::Menu, Device::Keyboard, SDL_SCANCODE_ESCAPE);
    put(table, Action::Diagnostics, Device::Keyboard, SDL_SCANCODE_F3);
    put(table, Action::IssueMarker, Device::Keyboard, SDL_SCANCODE_F4);
    put(table, Action::SaveProgress, Device::Keyboard, SDL_SCANCODE_F5);
    put(table, Action::FastForward, Device::Keyboard, SDL_SCANCODE_F6);
    put(table, Action::SkipCutscene, Device::Keyboard, SDL_SCANCODE_F7);
    put(table, Action::FreeCameraReset, Device::Keyboard, SDL_SCANCODE_KP_5);
    put(table, Action::FreeCameraLeft, Device::Keyboard, SDL_SCANCODE_KP_4);
    put(table, Action::FreeCameraRight, Device::Keyboard, SDL_SCANCODE_KP_6);
    put(table, Action::FreeCameraUp, Device::Keyboard, SDL_SCANCODE_KP_9);
    put(table, Action::FreeCameraDown, Device::Keyboard, SDL_SCANCODE_KP_3);
    put(table, Action::FreeCameraForward, Device::Keyboard, SDL_SCANCODE_KP_8);
    put(table, Action::FreeCameraBack, Device::Keyboard, SDL_SCANCODE_KP_2);
    put(table, Action::A, Device::Button, SDL_CONTROLLER_BUTTON_A);
    put(table, Action::B, Device::Button, SDL_CONTROLLER_BUTTON_X);
    put(table, Action::Start, Device::Button, SDL_CONTROLLER_BUTTON_START);
    put(table, Action::L, Device::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    put(table, Action::CLeft, Device::Button, SDL_CONTROLLER_BUTTON_Y);
    put(table, Action::CRight, Device::Button, SDL_CONTROLLER_BUTTON_B);
    put(table, Action::CUp, Device::Button, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    put(table, Action::CDown, Device::Button, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    put(table, Action::DUp, Device::Button, SDL_CONTROLLER_BUTTON_DPAD_UP);
    put(table, Action::DDown, Device::Button, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    put(table, Action::DLeft, Device::Button, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    put(table, Action::DRight, Device::Button, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    put(table, Action::Menu, Device::Button, SDL_CONTROLLER_BUTTON_BACK);
    put(table, Action::MoveUp, Device::AxisNegative, SDL_CONTROLLER_AXIS_LEFTY);
    put(table, Action::MoveDown, Device::AxisPositive, SDL_CONTROLLER_AXIS_LEFTY);
    put(table, Action::MoveLeft, Device::AxisNegative, SDL_CONTROLLER_AXIS_LEFTX);
    put(table, Action::MoveRight, Device::AxisPositive, SDL_CONTROLLER_AXIS_LEFTX);
    put(table, Action::Z, Device::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    put(table, Action::R, Device::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    put(table, Action::CLeft, Device::AxisNegative, SDL_CONTROLLER_AXIS_RIGHTX);
    put(table, Action::CRight, Device::AxisPositive, SDL_CONTROLLER_AXIS_RIGHTX);
    put(table, Action::CUp, Device::AxisNegative, SDL_CONTROLLER_AXIS_RIGHTY);
    put(table, Action::CDown, Device::AxisPositive, SDL_CONTROLLER_AXIS_RIGHTY);
    return table;
}
void copy_controller_bindings(BindingTable& to, const BindingTable& from) {
    for (size_t a = 0; a < count; ++a)
        for (unsigned i = 2; i < 8; ++i) to[a][i] = from[a][i];
}
DeviceRecord& record_for(const std::string& id) {
    auto& record = device_records[id];
    if (record.profiles.empty()) record.profiles.emplace("Default", legacy_template);
    if (!record.profiles.contains(record.active)) record.active = record.profiles.begin()->first;
    return record;
}
void save_profiles() {
    nlohmann::json devices = nlohmann::json::object();
    for (const auto& [id, record] : device_records) {
        if (id.find("|live-instance:") != std::string::npos) continue;
        nlohmann::json profiles = nlohmann::json::object();
        for (const auto& [name, table] : record.profiles) {
            nlohmann::json rows = nlohmann::json::array();
            for (const auto& row : table) {
                nlohmann::json codes = nlohmann::json::array();
                for (unsigned i = 2; i < 8; ++i) codes.push_back(row[i].code);
                rows.push_back(std::move(codes));
            }
            profiles[name] = std::move(rows);
        }
        devices[id] = {{"friendly_name", record.friendly_name},
            {"type", unsigned(record.type)}, {"active", record.active}, {"profiles", std::move(profiles)}};
    }
    tooie::menu::set_string("tooie_input_profiles_v1", nlohmann::json({{"devices", devices}}).dump());
}
void load_profiles() {
    device_records.clear();
    const auto source = tooie::menu::get_string("tooie_input_profiles_v1", "");
    if (source.empty()) return;
    try {
        const auto doc = nlohmann::json::parse(source);
        if (!doc.contains("devices") || !doc["devices"].is_object()) return;
        for (auto it = doc["devices"].begin(); it != doc["devices"].end(); ++it) {
            if (!it.value().is_object()) continue;
            DeviceRecord record;
            record.friendly_name = it.value().value("friendly_name", std::string{});
            const auto type = it.value().value("type", unsigned(ControllerType::Other));
            if (type <= unsigned(ControllerType::Other)) record.type = ControllerType(type);
            record.active = it.value().value("active", std::string("Default"));
            const auto profiles = it.value().value("profiles", nlohmann::json::object());
            if (profiles.is_object()) for (auto profile = profiles.begin(); profile != profiles.end(); ++profile) {
                if (!profile.value().is_array() || profile.value().size() != count) continue;
                BindingTable table = legacy_template;
                bool valid = true;
                for (size_t a = 0; a < count && valid; ++a) {
                    const auto& row = profile.value()[a];
                    if (!row.is_array() || row.size() != 6) { valid = false; break; }
                    for (unsigned i = 2; i < 8; ++i) {
                        if (!row[i - 2].is_number_integer()) { valid = false; break; }
                        const int code = row[i - 2].get<int>();
                        if (code < -1 || code > 255) { valid = false; break; }
                        table[a][i].code = code;
                    }
                }
                if (valid && !profile.key().empty()) record.profiles.emplace(profile.key(), std::move(table));
            }
            if (!record.profiles.empty()) device_records.emplace(it.key(), std::move(record));
        }
    } catch (const std::exception&) { device_records.clear(); }
}
void activate_device(const ControllerInfo* info) {
    std::lock_guard lock(binding_mutex);
    active_device_id = info ? info->stable_id : "";
    active_profile_key = active_device_id;
    if (info && info->ambiguous) {
        active_profile_key += "|live-instance:" + std::to_string(info->instance);
        if (!device_records.contains(active_profile_key))
            device_records.emplace(active_profile_key, record_for(active_device_id));
    }
    if (!active_profile_key.empty()) {
        auto& record = record_for(active_profile_key);
        copy_controller_bindings(bindings, record.profiles.at(record.active));
    }
    else copy_controller_bindings(bindings, legacy_template);
}
void choose_controller() {
    std::sort(opened.begin(), opened.end(), [](const auto& a, const auto& b) {
        return std::tie(a.info.guid, a.info.instance) < std::tie(b.info.guid, b.info.instance);
    });
    for (auto& entry : opened)
        entry.info.ambiguous = std::count_if(opened.begin(), opened.end(), [&](const auto& other) {
            return other.info.stable_id == entry.info.stable_id;
        }) > 1;
    auto it = manual_instance < 0 ? opened.end() :
        std::find_if(opened.begin(), opened.end(), [](const auto& entry) { return entry.info.instance == manual_instance; });
    if (it == opened.end() && !manual_required && !wanted_device_id.empty()) {
        const auto matches = std::count_if(opened.begin(), opened.end(), [](const auto& entry) {
            return entry.info.stable_id == wanted_device_id;
        });
        if (matches == 1) it = std::find_if(opened.begin(), opened.end(), [](const auto& entry) {
            return entry.info.stable_id == wanted_device_id;
        });
    }
    if (it == opened.end() && !manual_required && wanted_device_id.empty() && !wanted_guid.empty()) {
        const auto matches = std::count_if(opened.begin(), opened.end(), [](const auto& entry) {
            return entry.info.guid == wanted_guid;
        });
        if (matches == 1) it = std::find_if(opened.begin(), opened.end(), [](const auto& entry) {
            return entry.info.guid == wanted_guid;
        });
    }
    if (it == opened.end() && !manual_required && wanted_device_id.empty() && wanted_guid.empty() && opened.size() == 1)
        it = opened.begin();
    if (it == opened.end() && !manual_required && wanted_auto) {
        const auto previous = chosen.load(std::memory_order_acquire);
        it = std::find_if(opened.begin(), opened.end(), [previous](const auto& entry) {
            return entry.info.instance == previous;
        });
        if (it == opened.end() && !opened.empty()) it = opened.begin();
    }
    const auto selected = it == opened.end() ? -1 : it->info.instance;
    chosen.store(selected, std::memory_order_release);
    for (auto& entry : opened) entry.info.selected = entry.info.instance == selected;
    published_physical.store(selected >= 0, std::memory_order_release);
    published_rumble.store(it != opened.end() && it->info.rumble, std::memory_order_release);
    activate_device(it == opened.end() ? nullptr : &it->info);
}
void connect(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    auto* handle = SDL_GameControllerOpen(device_index);
    if (!handle) return;
    auto* joystick = SDL_GameControllerGetJoystick(handle);
    if (!joystick) { SDL_GameControllerClose(handle); return; }
    const auto instance = SDL_JoystickInstanceID(joystick);
    if (std::any_of(opened.begin(), opened.end(), [instance](const auto& e) { return e.info.instance == instance; })) {
        SDL_GameControllerClose(handle); return;
    }
    Uint16 vendor = 0, product = 0, version = 0, crc16 = 0;
    SDL_GetJoystickGUIDInfo(SDL_JoystickGetGUID(joystick), &vendor, &product, &version, &crc16);
    const char* serial = SDL_JoystickGetSerial(joystick);
    const char* path = SDL_JoystickPath(joystick);
    char guid_text[33]{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid_text, sizeof(guid_text));
    // Keep the saved preferred-controller identity format used by older profiles.
    const std::string guid = "SERIAL_" + std::string(serial ? serial : "") +
        "_VID_" + std::to_string(vendor) + "_PID_" + std::to_string(product) +
        "_VERSION_" + std::to_string(version) + "_CRC16_" + std::to_string(crc16);
    const char* hardware_name = SDL_JoystickName(joystick);
    const char* mapping_name = SDL_GameControllerName(handle);
    const std::string serial_text = serial ? serial : "";
    const std::string path_text = path ? path : "";
    std::string device_name;
#if defined(_WIN32)
    device_name = hid_product_name(path);
#endif
    if (device_name.empty()) device_name = hardware_name ? hardware_name : "Controller";
    const std::string stable = std::string(guid_text) + ":" + std::to_string(vendor) + ":" +
        std::to_string(product) + (serial_text.empty() ? (path_text.empty() ? "" : ":path:" + path_text)
            : ":serial:" + serial_text);
    ControllerInfo info{instance, guid, std::move(device_name), false,
        SDL_JoystickHasRumble(joystick) == SDL_TRUE};
    info.stable_id = stable; info.serial = serial_text; info.path = path_text;
    info.mapping_name = mapping_name ? mapping_name : "";
    info.sdl_guid = guid_text;
    info.vendor = vendor; info.product = product;
    {
        std::lock_guard lock(binding_mutex);
        if (!device_records.contains(stable) && !legacy_profile_tables.empty()) {
            DeviceRecord migrated;
            BindingTable initial=legacy_template;
            // Only the old preferred device receives the canonical input.*
            // overlay. Other known devices retain their own old assignment (or
            // the shared mapping), rather than inheriting the preferred pad.
            if (!legacy_preferred_guid.empty() && guid!=legacy_preferred_guid) {
                const auto assignment=legacy_assignments.find(guid);
                const auto selected=assignment==legacy_assignments.end()
                    ? legacy_profile_tables.end():legacy_profile_tables.find(assignment->second);
                const auto shared=legacy_profile_tables.find("controller_sp");
                initial=selected!=legacy_profile_tables.end()?selected->second:
                    shared!=legacy_profile_tables.end()?shared->second:defaults();
            }
            migrated.profiles.emplace("Default",std::move(initial));
            for (const auto& [key, table] : legacy_profile_tables) {
                if (key == "controller_sp") continue;
                migrated.profiles.emplace(key == "Default" ? "Default (legacy)" : key, table);
            }
            device_records.emplace(stable, std::move(migrated));
            save_profiles();
        }
        const auto& configured_name = record_for(stable).friendly_name;
        info.friendly_name = configured_name.empty() ? info.name : configured_name;
    }
    opened.push_back({handle, std::move(info)});
    choose_controller();
}
SDL_GameController* selected_controller() {
    const auto wanted = chosen.load(std::memory_order_acquire);
    const auto it = std::find_if(opened.begin(), opened.end(), [wanted](const auto& e) { return e.info.instance == wanted; });
    return it == opened.end() ? nullptr : it->handle;
}
bool down(const BindingTable& table, Action action, const Uint8* keys, SDL_GameController* controller) {
    const auto& row = table[static_cast<size_t>(action)];
    for (const Binding binding : row) {
        if (binding.code < 0) continue;
        switch (binding.device) {
        case Device::Keyboard:
            if (keys && binding.code < SDL_NUM_SCANCODES && keys[binding.code]) return true;
            break;
        case Device::Button:
            if (controller && binding.code < SDL_CONTROLLER_BUTTON_MAX &&
                SDL_GameControllerGetButton(controller, SDL_GameControllerButton(binding.code))) return true;
            break;
        case Device::AxisPositive:
        case Device::AxisNegative:
            if (controller && binding.code < SDL_CONTROLLER_AXIS_MAX) {
                const auto value = SDL_GameControllerGetAxis(controller, SDL_GameControllerAxis(binding.code));
                if (binding.device == Device::AxisPositive ? value > 16000 : value < -16000) return true;
            }
            break;
        }
    }
    return false;
}
float axis(SDL_GameController* controller, SDL_GameControllerAxis which, float deadzone) {
    if (!controller) return 0;
    const float value = std::clamp(float(SDL_GameControllerGetAxis(controller, which)) / 32767.0f, -1.0f, 1.0f);
    return std::abs(value) < deadzone ? 0.0f : value;
}
float direction(const BindingTable& table, Action action, const Uint8* keys,
    SDL_GameController* controller, float deadzone) {
    float amount = 0;
    for (const auto binding : table[static_cast<size_t>(action)]) {
        if (binding.code < 0) continue;
        if (binding.device == Device::Keyboard && keys && binding.code < SDL_NUM_SCANCODES && keys[binding.code])
            amount = 1;
        else if (binding.device == Device::Button && controller && binding.code < SDL_CONTROLLER_BUTTON_MAX &&
            SDL_GameControllerGetButton(controller, SDL_GameControllerButton(binding.code))) amount = 1;
        else if (controller && (binding.device == Device::AxisPositive || binding.device == Device::AxisNegative) &&
            binding.code < SDL_CONTROLLER_AXIS_MAX) {
            const float value = axis(controller, SDL_GameControllerAxis(binding.code), deadzone);
            if (binding.device == Device::AxisPositive) amount = std::max(amount, value);
            else amount = std::max(amount, -value);
        }
    }
    return amount;
}
void import_legacy(const std::filesystem::path& root, const std::string& preferred, BindingTable& table) {
    std::ifstream source(root / "controls.json");
    if (!source) return;
    try {
        nlohmann::json doc; source >> doc;
        constexpr std::array<const char*, count> legacy_names{{
            "A","B","Z","START","L","R","C_UP","C_DOWN","C_LEFT","C_RIGHT",
            "DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT",
            "Y_AXIS_POS","Y_AXIS_NEG","X_AXIS_NEG","X_AXIS_POS",
            "TOGGLE_MENU","SAVE_PROGRESS","TOGGLE_DIAGNOSTICS","MARK_ISSUE","FAST_FORWARD","SKIP_CUTSCENE",
            "RESET_FREE_CAMERA","FREE_CAMERA_LEFT","FREE_CAMERA_RIGHT","FREE_CAMERA_UP",
            "FREE_CAMERA_DOWN","FREE_CAMERA_FORWARD","FREE_CAMERA_BACK"}};
        auto apply_profile = [&](const nlohmann::json& mappings, bool keyboard, BindingTable& target) {
            if (!mappings.is_object()) return;
            for (size_t a = 0; a < count; ++a) {
                auto it = mappings.find(legacy_names[a]);
                if (it == mappings.end() || !it->is_array()) continue;
                for (unsigned d = keyboard ? 0 : 1; d < (keyboard ? 1U : 4U); ++d)
                    for (unsigned s = 0; s < 2; ++s) put(target, Action(a), Device(d), -1, s);
                for (unsigned s = 0; s < std::min<size_t>(2, it->size()); ++s) {
                    const auto& field = (*it)[s];
                    if (!field.is_object()) continue;
                    const int type = field.value("input_type", 0), id = field.value("input_id", -1);
                    if (keyboard && type == 1 && id >= 0) put(target, Action(a), Device::Keyboard, id, s);
                    else if (!keyboard && type == 3 && id >= 0) put(target, Action(a), Device::Button, id, s);
                    else if (!keyboard && type == 4 && id != 0) put(target, Action(a),
                        id > 0 ? Device::AxisPositive : Device::AxisNegative, std::abs(id) - 1, s);
                }
            }
        };
        if (doc.contains("profiles") && doc["profiles"].is_array()) {
            for (const auto& profile : doc["profiles"]) {
                const auto key = profile.value("key", std::string{});
                if (key == "keyboard_sp") apply_profile(profile.value("mappings", nlohmann::json::object()), true, table);
                if (key == "controller_sp") apply_profile(profile.value("mappings", nlohmann::json::object()), false, table);
                if (key != "keyboard_sp" && !key.empty()) {
                    BindingTable migrated = defaults();
                    apply_profile(profile.value("mappings", nlohmann::json::object()), false, migrated);
                    legacy_profile_tables.emplace(key, std::move(migrated));
                }
            }
            // A device assigned its own profile in the old menu overrides the
            // shared single-player controller map for the saved preferred GUID.
            if (doc.contains("controllers") && doc["controllers"].is_array()) {
                std::string assigned_profile;
                for (const auto& controller : doc["controllers"]) {
                    if (!controller.is_object() || !controller.contains("guid") ||
                        !controller["guid"].is_object()) continue;
                    const auto& guid = controller["guid"];
                    const std::string identity = "SERIAL_" + guid.value("serial", std::string{}) +
                        "_VID_" + std::to_string(guid.value("vendor", 0)) +
                        "_PID_" + std::to_string(guid.value("product", 0)) +
                        "_VERSION_" + std::to_string(guid.value("version", 0)) +
                        "_CRC16_" + std::to_string(guid.value("crc16", 0));
                    const auto profile_key = controller.value("profile", std::string{});
                    if (!profile_key.empty()) legacy_assignments.emplace(identity, profile_key);
                    if (identity == preferred) assigned_profile = profile_key;
                }
                if (!preferred.empty() && !assigned_profile.empty())
                    for (const auto& profile : doc["profiles"])
                        if (profile.is_object() && profile.value("key", std::string{}) == assigned_profile)
                            apply_profile(profile.value("mappings", nlohmann::json::object()), false, table);
            }
        } else {
            if (doc.contains("keyboard")) apply_profile(doc["keyboard"], true, table);
            if (doc.contains("controller")) apply_profile(doc["controller"], false, table);
        }
    } catch (const std::exception&) {
        // The old file is untouched. Malformed content leaves the defaults in place.
    }
}
void dispatch(Action action) {
    switch (action) {
    case Action::SaveProgress:
        note_discrete_action(DiscreteInputAction::SaveProgress);
        tooie::save_progress::request(); break;
    case Action::Diagnostics:
        note_discrete_action(DiscreteInputAction::ToggleDiagnostics);
        post_diagnostics_toggle(); break;
    case Action::IssueMarker:
        note_discrete_action(DiscreteInputAction::MarkIssue);
        tooie::frontend::cheats::trigger_graphics_issue_marker(); break;
    case Action::SkipCutscene:
        note_discrete_action(DiscreteInputAction::SkipCutscene);
        tooie::cutscene_tools::request_skip(); break;
    case Action::FreeCameraReset: tooie::camera::request_free_camera_reset(); break;
    default: break;
    }
}
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
void apply_persistent_input_reset() noexcept {
    // This is the SDL/UI thread. Cross-thread restore only publishes the
    // request and zeroes atomic gameplay output; it never touches SDL state.
    if (!persistent_input_reset_pending.exchange(false, std::memory_order_acq_rel))
        return;
    persistent_input_quarantine = true;
    {
        std::lock_guard lock(capture_mutex);
        scanning.store(false, std::memory_order_release);
        capture_releasing = capture_armed = false;
        release_neutral_since = {};
        suppress_input.store(false, std::memory_order_release);
    }
    transient_buttons.store(0, std::memory_order_release);
    published_buttons.store(0, std::memory_order_release);
    published_x.store(0, std::memory_order_release);
    published_y.store(0, std::memory_order_release);
    published_rx.store(0, std::memory_order_release);
    published_ry.store(0, std::memory_order_release);
    published_enabled.store(false, std::memory_order_release);
    published_menu_pad = {};
    last_menu_binding = false;
    last_focused = false;
    focus_transition_pending = true;
    was_menu_open = true;
    suppressed_buttons = suppressed_keyboard_buttons = 0;
    suppress_move = suppress_camera = suppress_keyboard_move = false;
    suppress_fast_forward = suppress_free_camera_reset = true;
    last_utility_binding.fill(false);
    last_fast_forward = last_free_camera_reset = false;
    tooie::timing::set_fast_forward_held(false);
    tooie::camera::set_free_camera_input_active(false);
    tooie::camera::set_free_camera_motion(0, 0, 0);
}
#endif
}

namespace tooie::input {
const char* action_name(Action action) noexcept {
    const auto i = static_cast<size_t>(action);
    return i < labels.size() ? labels[i] : "Unknown";
}
Binding get_binding(Action action, Device device, unsigned slot) {
    if (action >= Action::Count || slot > 1) return {device, -1};
    std::lock_guard lock(binding_mutex);
    return bindings[static_cast<size_t>(action)][index(device, slot)];
}
void set_binding(Action action, Binding binding, unsigned slot) {
    if (action >= Action::Count || slot > 1) return;
    {
        std::lock_guard lock(binding_mutex);
        bindings[static_cast<size_t>(action)][index(binding.device, slot)] = binding;
        if (binding.device != Device::Keyboard && !active_profile_key.empty()) {
            auto& record = record_for(active_profile_key);
            record.profiles.at(record.active)[static_cast<size_t>(action)][index(binding.device, slot)] = binding;
            save_profiles();
        }
    }
    if (binding.device == Device::Keyboard)
        menu::set_int(setting_key(action, binding.device, slot), binding.code);
}
void reset_bindings() {
    auto next = defaults();
    {
        std::lock_guard lock(binding_mutex);
        bindings = next;
        if (!active_profile_key.empty()) {
            auto& record = record_for(active_profile_key);
            record.profiles.at(record.active) = next;
            save_profiles();
        }
    }
    for (size_t a = 0; a < count; ++a)
        for (unsigned s = 0; s < 2; ++s)
            menu::set_int(setting_key(Action(a), Device::Keyboard, s),
                next[a][index(Device::Keyboard, s)].code);
}
std::string binding_label(Binding binding) {
    if (binding.code < 0) return "Unbound";
    if (binding.device == Device::Keyboard) {
        // UI may ask from the render thread, so do not call SDL name helpers here.
        switch (binding.code) {
        case SDL_SCANCODE_ESCAPE: return "Escape";
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_SPACE: return "Space";
        case SDL_SCANCODE_LSHIFT: return "Left Shift";
        case SDL_SCANCODE_RSHIFT: return "Right Shift";
        case SDL_SCANCODE_UP: return "Up Arrow";
        case SDL_SCANCODE_DOWN: return "Down Arrow";
        case SDL_SCANCODE_LEFT: return "Left Arrow";
        case SDL_SCANCODE_RIGHT: return "Right Arrow";
        case SDL_SCANCODE_F3: return "F3";
        case SDL_SCANCODE_F4: return "F4";
        case SDL_SCANCODE_F5: return "F5";
        case SDL_SCANCODE_F6: return "F6";
        case SDL_SCANCODE_F7: return "F7";
        default: break;
        }
        if (binding.code >= SDL_SCANCODE_A && binding.code <= SDL_SCANCODE_Z)
            return std::string(1, char('A' + binding.code - SDL_SCANCODE_A));
        return "Key " + std::to_string(binding.code);
    }
    if (binding.device == Device::Button) {
        constexpr std::array<const char*, 21> labels{{
            "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
            "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
            "D-pad Right", "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"}};
        return binding.code < int(labels.size()) ? labels[binding.code] : "Button " + std::to_string(binding.code);
    }
    constexpr std::array<const char*, 6> axes{{"Left X", "Left Y", "Right X", "Right Y", "Left Trigger", "Right Trigger"}};
    return (binding.code < int(axes.size()) ? std::string(axes[binding.code]) : "Axis " + std::to_string(binding.code)) +
        (binding.device == Device::AxisPositive ? " +" : " -");
}
const char* controller_type_name(ControllerType type) noexcept {
    switch (type) {
    case ControllerType::N64: return "N64";
    case ControllerType::DualSense: return "DualShock / DualSense";
    case ControllerType::Xbox: return "Xbox";
    case ControllerType::NintendoPro: return "Nintendo Pro Controller";
    case ControllerType::Steam: return "Steam Controller";
    default: return "Other";
    }
}
std::string binding_label_for_type(Binding binding, ControllerType type) {
    if (binding.code < 0 || binding.device == Device::Keyboard) return binding_label(binding);
    const bool raw = type == ControllerType::N64 || type == ControllerType::Other;
    if (binding.device != Device::Button) {
        const auto sign = binding.device == Device::AxisPositive ? " +" : " -";
        if (raw) return "SDL Axis " + std::to_string(binding.code) + sign;
        if (binding.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
            switch (type) {
            case ControllerType::DualSense: return std::string("L2") + sign;
            case ControllerType::NintendoPro: return std::string("ZL") + sign;
            default: return std::string("LT") + sign;
            }
        }
        if (binding.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            switch (type) {
            case ControllerType::DualSense: return std::string("R2") + sign;
            case ControllerType::NintendoPro: return std::string("ZR") + sign;
            default: return std::string("RT") + sign;
            }
        }
        return binding_label(binding);
    }
    if (raw) return "SDL Button " + std::to_string(binding.code);
    constexpr std::array<const char*, 4> dualsense{{"Cross", "Circle", "Square", "Triangle"}};
    constexpr std::array<const char*, 4> xbox{{"A", "B", "X", "Y"}};
    constexpr std::array<const char*, 4> nintendo{{"B (south)", "A (east)", "Y (west)", "X (north)"}};
    constexpr std::array<const char*, 4> steam{{"A (south)", "B (east)", "X (west)", "Y (north)"}};
    if (binding.code <= SDL_CONTROLLER_BUTTON_Y) {
        const auto i = size_t(binding.code);
        switch (type) {
        case ControllerType::DualSense: return dualsense[i];
        case ControllerType::Xbox: return xbox[i];
        case ControllerType::NintendoPro: return nintendo[i];
        case ControllerType::Steam: return steam[i];
        default: break;
        }
    }
    switch (binding.code) {
    case SDL_CONTROLLER_BUTTON_BACK:
        switch (type) {
        case ControllerType::DualSense: return "Create / Share";
        case ControllerType::Xbox: return "View";
        case ControllerType::NintendoPro: return "Minus";
        case ControllerType::Steam: return "Back";
        default: break;
        }
        break;
    case SDL_CONTROLLER_BUTTON_GUIDE:
        switch (type) {
        case ControllerType::DualSense: return "PS";
        case ControllerType::Xbox: return "Xbox";
        case ControllerType::NintendoPro: return "Home";
        case ControllerType::Steam: return "Steam";
        default: break;
        }
        break;
    case SDL_CONTROLLER_BUTTON_START:
        switch (type) {
        case ControllerType::DualSense: return "Options";
        case ControllerType::Xbox: return "Menu";
        case ControllerType::NintendoPro: return "Plus";
        case ControllerType::Steam: return "Start";
        default: break;
        }
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return type == ControllerType::DualSense ? "L3" : type == ControllerType::Xbox ? "LS" : "Left Stick Click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return type == ControllerType::DualSense ? "R3" : type == ControllerType::Xbox ? "RS" : "Right Stick Click";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        switch (type) {
        case ControllerType::DualSense: return "L1";
        case ControllerType::Xbox: return "LB";
        case ControllerType::NintendoPro: return "L";
        case ControllerType::Steam: return "Left Bumper";
        default: break;
        }
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        switch (type) {
        case ControllerType::DualSense: return "R1";
        case ControllerType::Xbox: return "RB";
        case ControllerType::NintendoPro: return "R";
        case ControllerType::Steam: return "Right Bumper";
        default: break;
        }
        break;
    default: break;
    }
    return binding_label(binding);
}
void begin_capture(Action action, Device device, unsigned slot) {
    if (action >= Action::Count || slot > 1) return;
    const auto instance = chosen.load(std::memory_order_acquire);
    if (device != Device::Keyboard && instance < 0) return;
    std::lock_guard lock(capture_mutex);
    scanned_action = action; scanned_device = device; scanned_slot = slot;
    capture_instance = instance;
    capture_armed = false;
    capture_releasing = false;
    capture_deadline = std::chrono::steady_clock::now() + capture_timeout;
    release_neutral_since = {};
    suppress_input.store(true, std::memory_order_release);
    scanning.store(true, std::memory_order_release);
}
void cancel_capture() noexcept {
    std::lock_guard lock(capture_mutex);
    scanning.store(false, std::memory_order_release);
    capture_releasing = true;
    release_neutral_since = {};
}
bool capture_active() noexcept { return scanning.load(std::memory_order_acquire); }
CaptureStatus capture_status() noexcept {
    std::lock_guard lock(capture_mutex);
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        capture_deadline - std::chrono::steady_clock::now()).count();
    return {scanning.load(std::memory_order_acquire), !capture_armed,
        scanning.load(std::memory_order_acquire) ? int(std::max<std::int64_t>(0, remaining + 1)) : 0,
        scanned_action, scanned_device, scanned_slot};
}
bool input_suppressed() noexcept { return suppress_input.load(std::memory_order_acquire); }
std::vector<ControllerInfo> controllers() {
    std::vector<ControllerInfo> out;
    std::lock_guard lock(controller_mutex);
    for (const auto& e : opened) out.push_back(e.info);
    return out;
}
SDL_JoystickID selected_controller_instance() noexcept { return chosen.load(std::memory_order_acquire); }
bool select_controller(SDL_JoystickID instance) {
    if (instance < 0) { set_preferred_guid(""); return true; }
    std::lock_guard lock(controller_mutex);
    const auto it = std::find_if(opened.begin(), opened.end(), [instance](const auto& item) {
        return item.info.instance == instance;
    });
    if (it == opened.end()) return false;
    manual_instance = instance;
    manual_required = false;
    wanted_auto = false;
    wanted_device_id = it->info.stable_id;
    wanted_guid = it->info.guid;
    choose_controller();
    // The live instance is only a routing hint. An indistinguishable pair needs
    // another explicit selection after restart rather than a guessed restore.
    menu::set_string("tooie_preferred_controller_id", it->info.ambiguous ? "" : wanted_device_id);
    menu::set_string("tooie_preferred_controller_guid", it->info.ambiguous ? "" : wanted_guid);
    menu::set_bool("tooie_controller_auto", false);
    return true;
}
std::string selected_device_id() {
    std::lock_guard lock(binding_mutex);
    return active_device_id;
}
std::string preferred_device_id() {
    std::lock_guard lock(controller_mutex);
    return wanted_device_id;
}
bool controller_auto_selection() noexcept {
    std::lock_guard lock(controller_mutex);
    return wanted_auto;
}
std::string selected_friendly_name() {
    std::lock_guard lock(controller_mutex);
    const auto selected = chosen.load(std::memory_order_acquire);
    const auto it = std::find_if(opened.begin(), opened.end(), [selected](const auto& entry) {
        return entry.info.instance == selected;
    });
    return it == opened.end() ? "" : it->info.friendly_name;
}
void set_selected_friendly_name(std::string name) {
    if (name.size() > 64) name.resize(64);
    std::string id;
    {
        std::lock_guard lock(binding_mutex);
        if (active_device_id.empty()) return;
        id = active_profile_key;
        record_for(id).friendly_name = name;
        save_profiles();
    }
    std::lock_guard controller_lock(controller_mutex);
    for (auto& entry : opened)
        if ((id.find("|live-instance:") == std::string::npos && entry.info.stable_id == id) ||
            id == entry.info.stable_id + "|live-instance:" + std::to_string(entry.info.instance))
            entry.info.friendly_name = name.empty() ? entry.info.name : name;
}
ControllerType selected_controller_type() {
    std::lock_guard lock(binding_mutex);
    return active_profile_key.empty() ? ControllerType::Other : record_for(active_profile_key).type;
}
void set_selected_controller_type(ControllerType type) {
    if (type > ControllerType::Other) return;
    std::lock_guard lock(binding_mutex);
    if (active_device_id.empty()) return;
    record_for(active_profile_key).type = type;
    save_profiles();
}
std::vector<std::string> controller_profiles() {
    std::lock_guard lock(binding_mutex);
    std::vector<std::string> out;
    if (!active_profile_key.empty()) for (const auto& [name, table] : record_for(active_profile_key).profiles)
        out.push_back(name);
    return out;
}
std::string active_controller_profile() {
    std::lock_guard lock(binding_mutex);
    return active_profile_key.empty() ? "" : record_for(active_profile_key).active;
}
bool create_controller_profile(std::string name) {
    if (name.empty() || name.size() > 48) return false;
    std::lock_guard lock(binding_mutex);
    if (active_device_id.empty()) return false;
    auto& record = record_for(active_profile_key);
    if (record.profiles.contains(name)) return false;
    record.profiles.emplace(name, bindings);
    record.active = name;
    save_profiles();
    return true;
}
bool select_controller_profile(std::string name) {
    std::lock_guard lock(binding_mutex);
    if (active_device_id.empty()) return false;
    auto& record = record_for(active_profile_key);
    const auto it = record.profiles.find(name);
    if (it == record.profiles.end()) return false;
    record.active = name;
    copy_controller_bindings(bindings, it->second);
    save_profiles();
    return true;
}
bool rename_controller_profile(std::string name) {
    if (name.empty() || name.size() > 48) return false;
    std::lock_guard lock(binding_mutex);
    if (active_device_id.empty()) return false;
    auto& record = record_for(active_profile_key);
    if (record.profiles.contains(name)) return false;
    auto node = record.profiles.extract(record.active);
    node.key() = name;
    record.profiles.insert(std::move(node));
    record.active = name;
    save_profiles();
    return true;
}
void reset_active_controller_profile() {
    std::lock_guard lock(binding_mutex);
    if (active_device_id.empty()) return;
    auto& record = record_for(active_profile_key);
    record.profiles.at(record.active) = defaults();
    copy_controller_bindings(bindings, record.profiles.at(record.active));
    save_profiles();
}
std::string preferred_guid() {
    std::lock_guard lock(controller_mutex);
    return wanted_guid;
}
void set_preferred_guid(std::string guid) {
    {
        std::lock_guard lock(controller_mutex);
        wanted_guid = guid;
        wanted_device_id.clear();
        manual_instance = -1;
        manual_required = false;
        wanted_auto = guid.empty();
        choose_controller();
    }
    menu::set_string("tooie_preferred_controller_guid", guid);
    menu::set_string("tooie_preferred_controller_id", "");
    menu::set_bool("tooie_controller_auto", guid.empty());
}
void initialize_settings(const std::filesystem::path& config_root) {
    sync_background_joystick_events(menu::get_bool("background_input_mode", true));
    auto next = defaults();
    const auto saved_guid = menu::get_string("tooie_preferred_controller_guid", "");
    legacy_preferred_guid=saved_guid;
    const bool profiles_present = !menu::get_string("tooie_input_profiles_v1", "").empty();
    legacy_profile_tables.clear();
    legacy_assignments.clear();
    import_legacy(config_root, saved_guid, next);
    for (size_t a = 0; a < count; ++a)
        for (unsigned d = 0; d < 4; ++d)
            for (unsigned s = 0; s < 2; ++s) {
                if (d != unsigned(Device::Keyboard) && profiles_present) continue;
                auto& b = next[a][index(Device(d), s)];
                b.code = menu::get_int(setting_key(Action(a), Device(d), s), b.code);
            }
    {
        std::lock_guard lock(binding_mutex);
        bindings = next;
        legacy_template = next;
        load_profiles();
    }
    {
        std::lock_guard lock(controller_mutex);
        wanted_guid = saved_guid;
        wanted_device_id = menu::get_string("tooie_preferred_controller_id", "");
        wanted_auto = menu::get_bool("tooie_controller_auto",
            wanted_device_id.empty() && wanted_guid.empty());
        manual_required = !wanted_auto && wanted_device_id.empty() && wanted_guid.empty();
        manual_instance = -1;
        for (int i = 0, n = SDL_NumJoysticks(); i < n; ++i) connect(i);
        choose_controller();
    }
    tooie::timing::set_fast_forward_rate(menu::get_int("tooie_fast_forward_rate", 2));
}
void shutdown() noexcept {
    std::lock_guard lock(controller_mutex);
    for (auto& e : opened) SDL_GameControllerClose(e.handle);
    opened.clear(); chosen = -1;
    published_enabled.store(false); published_physical.store(false); published_rumble.store(false);
    transient_buttons.store(0); scanning.store(false); suppress_input.store(false);
    capture_releasing = capture_armed = false;
    last_menu_binding = false; last_focused = false; focus_transition_pending = false;
    frontend_window = nullptr;
    background_hint_initialized = false;
    was_menu_open = true; suppressed_buttons = suppressed_keyboard_buttons = 0;
    suppress_move = suppress_camera = suppress_keyboard_move = false;
    suppress_fast_forward = suppress_free_camera_reset = false;
    last_utility_binding.fill(false); last_fast_forward = last_free_camera_reset = false;
    tooie::timing::set_fast_forward_held(false);
}
void handle_event(const SDL_Event& event) {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    apply_persistent_input_reset();
    if (persistent_input_quarantine && event.type != SDL_CONTROLLERDEVICEADDED &&
        event.type != SDL_CONTROLLERDEVICEREMOVED && event.type != SDL_WINDOWEVENT)
        return;
#endif
    if (event.type == SDL_WINDOWEVENT &&
        (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
         event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
        focus_transition_pending = true;
        // Loss discards old foreground edges. A selected-controller gameplay
        // edge received while unfocused must survive a subsequent gain in the
        // same poll batch; keyboard/capture/utility events never enter that path.
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            !menu::get_bool("background_input_mode", true))
            transient_buttons.store(0, std::memory_order_release);
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        std::lock_guard lock(controller_mutex);
        connect(event.cdevice.which);
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (event.cdevice.which == capture_instance.load(std::memory_order_acquire) && capture_active()) cancel_capture();
        std::lock_guard lock(controller_mutex);
        if (event.cdevice.which == chosen.load(std::memory_order_acquire)) {
            const auto removed = std::find_if(opened.begin(), opened.end(), [&](const auto& entry) {
                return entry.info.instance == event.cdevice.which;
            });
            manual_required = removed != opened.end() && removed->info.ambiguous;
            manual_instance = -1;
        }
        std::erase_if(opened, [&](auto& e) {
            if (e.info.instance != event.cdevice.which) return false;
            SDL_GameControllerClose(e.handle); return true;
        });
        choose_controller();
        return;
    }
    const bool key = event.type == SDL_KEYDOWN && !event.key.repeat;
    const bool button = event.type == SDL_CONTROLLERBUTTONDOWN;
    const bool axis_event = event.type == SDL_CONTROLLERAXISMOTION && std::abs(int(event.caxis.value)) > 22000;
    if (!key && !button && !axis_event) return;
    const bool focused = frontend_window && SDL_GetKeyboardFocus() == frontend_window;
    if (!focused) {
        // Background mode is gameplay-only. Keep short controller taps that
        // arrive between ticks, but never admit menu, utility, or capture here.
        if ((button || axis_event) && menu::get_bool("background_input_mode", true) &&
            ultramodern::is_game_started() && !menu::is_open() &&
            !input_suppressed() &&
            (button ? event.cbutton.which : event.caxis.which) == chosen.load()) {
            BindingTable table;
            { std::lock_guard lock(binding_mutex); table = bindings; }
            constexpr std::array<std::pair<Action, std::uint16_t>, 14> game_buttons{{
                {Action::A,0x8000},{Action::B,0x4000},{Action::Z,0x2000},{Action::Start,0x1000},
                {Action::DUp,0x0800},{Action::DDown,0x0400},{Action::DLeft,0x0200},{Action::DRight,0x0100},
                {Action::L,0x0020},{Action::R,0x0010},{Action::CUp,0x0008},{Action::CDown,0x0004},
                {Action::CLeft,0x0002},{Action::CRight,0x0001}}};
            for (const auto [action, mask] : game_buttons)
                for (const Binding binding : table[static_cast<size_t>(action)])
                    if ((button && binding.device == Device::Button &&
                            binding.code == event.cbutton.button) ||
                        (axis_event && binding.code == event.caxis.axis &&
                            binding.device == (event.caxis.value > 0
                                ? Device::AxisPositive : Device::AxisNegative)))
                        transient_buttons.fetch_or(mask, std::memory_order_release);
        }
        return;
    }
    if (focus_transition_pending) return;
    if (input_suppressed()) {
        if (key && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { cancel_capture(); return; }
        std::lock_guard capture_lock(capture_mutex);
        if (!scanning.load(std::memory_order_acquire) || !capture_armed) return;
        Binding found{scanned_device, -1};
        if (key && scanned_device == Device::Keyboard) found.code = event.key.keysym.scancode;
        if (button && scanned_device != Device::Keyboard && event.cbutton.which == capture_instance.load()) found.code = event.cbutton.button;
        if (axis_event && event.caxis.which == capture_instance.load() &&
            scanned_device != Device::Keyboard) {
            found.device = event.caxis.value > 0 ? Device::AxisPositive : Device::AxisNegative;
            found.code = event.caxis.axis;
        }
        if (found.code >= 0) {
            if (scanned_device != Device::Keyboard)
                for (Device device : {Device::Button, Device::AxisPositive, Device::AxisNegative})
                    set_binding(scanned_action, {device, -1}, scanned_slot);
            set_binding(scanned_action, found, scanned_slot);
            scanning.store(false, std::memory_order_release);
            capture_releasing = true;
            release_neutral_since = {};
        }
        return;
    }
    BindingTable table;
    { std::lock_guard lock(binding_mutex); table = bindings; }
    const auto matches = [&](Action action) {
        const auto& row = table[static_cast<size_t>(action)];
        for (const auto b : row) {
            if (key && b.device == Device::Keyboard && b.code == event.key.keysym.scancode) return true;
            if (button && b.device == Device::Button && b.code == event.cbutton.button &&
                event.cbutton.which == chosen.load()) return true;
            if (axis_event && b.code == event.caxis.axis &&
                b.device == (event.caxis.value > 0 ? Device::AxisPositive : Device::AxisNegative) &&
                event.caxis.which == chosen.load()) return true;
        }
        return false;
    };
    if (!axis_event && matches(Action::Menu)) {
        last_menu_binding = true;
        transient_buttons.store(0, std::memory_order_release);
        menu::set_open(!menu::is_open()); return;
    }
    if (!ultramodern::is_game_started() || menu::is_open()) return;
    constexpr std::array<std::pair<Action, std::uint16_t>, 14> n64_buttons{{
        {Action::A,0x8000},{Action::B,0x4000},{Action::Z,0x2000},{Action::Start,0x1000},
        {Action::DUp,0x0800},{Action::DDown,0x0400},{Action::DLeft,0x0200},{Action::DRight,0x0100},
        {Action::L,0x0020},{Action::R,0x0010},{Action::CUp,0x0008},{Action::CDown,0x0004},
        {Action::CLeft,0x0002},{Action::CRight,0x0001}}};
    for (const auto [action, mask] : n64_buttons)
        if (matches(action)) transient_buttons.fetch_or(mask, std::memory_order_release);
    for (Action action : {Action::SaveProgress, Action::Diagnostics, Action::IssueMarker,
                          Action::SkipCutscene, Action::FreeCameraReset})
        if (!axis_event && matches(action)) {
            last_utility_binding[static_cast<size_t>(action)] = true;
            dispatch(action);
        }
}
void tick(SDL_Window* window, bool gameplay, bool menu_open) {
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    apply_persistent_input_reset();
#endif
    (void)menu_open;
    frontend_window = window;
    const bool background_controller = menu::get_bool("background_input_mode", true);
    sync_background_joystick_events(background_controller);
    SDL_GameControllerUpdate();
    const bool focused = window && SDL_GetKeyboardFocus() == window;
    BindingTable table;
    { std::lock_guard lock(binding_mutex); table = bindings; }
    SDL_GameController* controller;
    {
        std::lock_guard lock(controller_mutex);
        controller = selected_controller();
        const bool wanted = desired_rumble.load(std::memory_order_acquire);
        const auto strength = std::clamp(menu::get_number("rumble_strength", 100.0), 0.0, 100.0);
        const auto intensity = static_cast<Uint16>(std::lround(strength * 655.35));
        const auto now = std::chrono::steady_clock::now();
        if (controller && (wanted != applied_rumble || chosen.load() != applied_rumble_device ||
            (wanted && now - last_rumble_update > std::chrono::milliseconds(500)))) {
            SDL_GameControllerRumble(controller, wanted ? intensity : 0, wanted ? intensity : 0, 1000);
            applied_rumble = wanted; applied_rumble_device = chosen.load(); last_rumble_update = now;
        }
        published_menu_pad = {};
        if (focused && controller) {
            published_menu_pad.connected = true;
            for (unsigned b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < 32; ++b)
                if (SDL_GameControllerGetButton(controller, SDL_GameControllerButton(b)))
                    published_menu_pad.buttons |= 1U << b;
            published_menu_pad.left_x = float(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX)) / 32767.0f;
            published_menu_pad.left_y = float(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY)) / 32767.0f;
        }
    }
    const Uint8* keys = focused ? SDL_GetKeyboardState(nullptr) : nullptr;
    {
        bool neutral = true;
        if (keys) for (int i = 0; i < SDL_NUM_SCANCODES; ++i)
            if (keys[i]) { neutral = false; break; }
        if (controller) {
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
                if (SDL_GameControllerGetButton(controller, SDL_GameControllerButton(i))) neutral = false;
            for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
                if (std::abs(int(SDL_GameControllerGetAxis(controller, SDL_GameControllerAxis(i)))) > 16000)
                    neutral = false;
        }
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(capture_mutex);
        if (scanning.load(std::memory_order_acquire)) {
            if (now >= capture_deadline ||
                (scanned_device != Device::Keyboard && capture_instance.load() != chosen.load(std::memory_order_acquire))) {
                scanning.store(false, std::memory_order_release);
                capture_releasing = true;
                release_neutral_since = {};
            } else if (neutral) capture_armed = true;
        }
        if (capture_releasing) {
            if (!neutral) release_neutral_since = {};
            else if (release_neutral_since == std::chrono::steady_clock::time_point{})
                release_neutral_since = now;
            else if (now - release_neutral_since >= release_cooldown) {
                capture_releasing = false;
                suppress_input.store(false, std::memory_order_release);
            }
        }
    }
    if (input_suppressed()) published_menu_pad = {};
    auto is_down = [&](Action action) { return down(table, action, keys, controller); };
    const bool focus_regained = focused && (!last_focused || focus_transition_pending);
    last_focused = focused;
    focus_transition_pending = false;
    if (!focused && background_controller) {
        // Foreground menu/focus suppression must not interrupt a controller
        // that was deliberately left active while this window was unfocused.
        suppressed_buttons = 0;
        suppress_move = suppress_camera = false;
    }
    const bool menu_pressed = is_down(Action::Menu);
    if (focused && !focus_regained && menu_pressed && !last_menu_binding && !input_suppressed()) {
        transient_buttons.store(0, std::memory_order_release);
        menu::set_open(!menu::is_open());
    }
    last_menu_binding = menu_pressed;
    const bool enabled = gameplay && !menu::is_open() && !input_suppressed() &&
        (focused || background_controller);
    const bool utility_allowed = gameplay && focused && !focus_regained &&
        !menu::is_open() && !input_suppressed();
    for (Action action : {Action::SaveProgress, Action::Diagnostics, Action::IssueMarker,
                          Action::SkipCutscene, Action::FreeCameraReset}) {
        const bool pressed = is_down(action);
        auto& previous = last_utility_binding[static_cast<size_t>(action)];
        if (utility_allowed && pressed && !previous) dispatch(action);
        previous = pressed;
    }
    const float deadzone = std::clamp(float(menu::get_number("joystick_deadzone", 5.0)) / 100.0f, 0.0f, 0.9f);
    const bool current_menu_open = menu::is_open();
    const bool menu_closed = was_menu_open && !current_menu_open;
    if (menu_closed || (focus_regained && !background_controller)) {
        constexpr std::array<std::pair<Action, std::uint16_t>, 14> game_buttons{{
            {Action::A,0x8000},{Action::B,0x4000},{Action::Z,0x2000},{Action::Start,0x1000},
            {Action::DUp,0x0800},{Action::DDown,0x0400},{Action::DLeft,0x0200},{Action::DRight,0x0100},
            {Action::L,0x0020},{Action::R,0x0010},{Action::CUp,0x0008},{Action::CDown,0x0004},
            {Action::CLeft,0x0002},{Action::CRight,0x0001}}};
        for (const auto [action, mask] : game_buttons)
            if (is_down(action)) suppressed_buttons |= mask;
        suppress_move = direction(table, Action::MoveRight, keys, controller, deadzone) > 0 ||
            direction(table, Action::MoveLeft, keys, controller, deadzone) > 0 ||
            direction(table, Action::MoveUp, keys, controller, deadzone) > 0 ||
            direction(table, Action::MoveDown, keys, controller, deadzone) > 0;
        suppress_camera = std::abs(axis(controller, SDL_CONTROLLER_AXIS_RIGHTX, deadzone)) > 0 ||
            std::abs(axis(controller, SDL_CONTROLLER_AXIS_RIGHTY, deadzone)) > 0;
    } else if (focus_regained) {
        // Suppress keys held while typing in another app, but do not suppress
        // the simultaneously held physical controller source for that action.
        constexpr std::array<std::pair<Action, std::uint16_t>, 14> game_buttons{{
            {Action::A,0x8000},{Action::B,0x4000},{Action::Z,0x2000},{Action::Start,0x1000},
            {Action::DUp,0x0800},{Action::DDown,0x0400},{Action::DLeft,0x0200},{Action::DRight,0x0100},
            {Action::L,0x0020},{Action::R,0x0010},{Action::CUp,0x0008},{Action::CDown,0x0004},
            {Action::CLeft,0x0002},{Action::CRight,0x0001}}};
        for (const auto [action, mask] : game_buttons)
            if (down(table, action, keys, nullptr)) suppressed_keyboard_buttons |= mask;
        suppress_keyboard_move =
            direction(table, Action::MoveRight, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveLeft, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveUp, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveDown, keys, nullptr, deadzone) > 0;
    }
    if (focus_regained) {
        suppress_fast_forward = is_down(Action::FastForward);
        suppress_free_camera_reset = is_down(Action::FreeCameraReset);
    }
    was_menu_open = current_menu_open;
    std::uint16_t bits = 0;
    float x = 0, y = 0, rx = 0, ry = 0;
    if (enabled) {
        constexpr std::array<std::pair<Action, std::uint16_t>, 14> buttons{{
            {Action::A,0x8000},{Action::B,0x4000},{Action::Z,0x2000},{Action::Start,0x1000},
            {Action::DUp,0x0800},{Action::DDown,0x0400},{Action::DLeft,0x0200},{Action::DRight,0x0100},
            {Action::L,0x0020},{Action::R,0x0010},{Action::CUp,0x0008},{Action::CDown,0x0004},
            {Action::CLeft,0x0002},{Action::CRight,0x0001}
        }};
        std::uint16_t keyboard_bits = 0, controller_bits = 0;
        for (const auto [action, mask] : buttons) {
            if (down(table, action, keys, nullptr)) keyboard_bits |= mask;
            if (down(table, action, nullptr, controller)) controller_bits |= mask;
        }
        suppressed_keyboard_buttons &= keyboard_bits;
        bits = (keyboard_bits & ~suppressed_keyboard_buttons) | controller_bits;
        suppressed_buttons &= bits;
        bits &= ~suppressed_buttons;
        const bool keyboard_move_held =
            direction(table, Action::MoveRight, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveLeft, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveUp, keys, nullptr, deadzone) > 0 ||
            direction(table, Action::MoveDown, keys, nullptr, deadzone) > 0;
        if (!keyboard_move_held) suppress_keyboard_move = false;
        const Uint8* move_keys = suppress_keyboard_move ? nullptr : keys;
        x = direction(table, Action::MoveRight, move_keys, controller, deadzone) -
            direction(table, Action::MoveLeft, move_keys, controller, deadzone);
        y = direction(table, Action::MoveUp, move_keys, controller, deadzone) -
            direction(table, Action::MoveDown, move_keys, controller, deadzone);
        const float length = std::hypot(x, y);
        if (length > 1.0f) { x /= length; y /= length; }
        if (suppress_move) {
            if (x == 0 && y == 0) suppress_move = false;
            else x = y = 0;
        }
        rx = axis(controller, SDL_CONTROLLER_AXIS_RIGHTX, deadzone);
        ry = -axis(controller, SDL_CONTROLLER_AXIS_RIGHTY, deadzone);
        if (suppress_camera) {
            if (rx == 0 && ry == 0) suppress_camera = false;
            else rx = ry = 0;
        }
    }
    published_buttons.store(bits, std::memory_order_release);
    if (!enabled) transient_buttons.store(0, std::memory_order_release);
    published_x.store(int(std::lround(x * 32767)), std::memory_order_release);
    published_y.store(int(std::lround(y * 32767)), std::memory_order_release);
    published_rx.store(int(std::lround(rx * 32767)), std::memory_order_release);
    published_ry.store(int(std::lround(ry * 32767)), std::memory_order_release);
    published_enabled.store(enabled, std::memory_order_release);
    const bool fast_pressed = is_down(Action::FastForward);
    if (!fast_pressed) suppress_fast_forward = false;
    const bool fast = utility_allowed && fast_pressed && !suppress_fast_forward;
    if (fast && !last_fast_forward) note_discrete_action(DiscreteInputAction::FastForward);
    last_fast_forward = fast;
    tooie::timing::set_fast_forward_rate(menu::get_int("tooie_fast_forward_rate", 2));
    tooie::timing::set_fast_forward_held(fast);
    const bool camera_enabled = enabled && focused && !focus_regained &&
        tooie::camera::free_camera_enabled();
    tooie::camera::set_free_camera_input_active(camera_enabled);
    tooie::camera::set_free_camera_motion(
        camera_enabled ? float(is_down(Action::FreeCameraRight)) - float(is_down(Action::FreeCameraLeft)) : 0,
        camera_enabled ? float(is_down(Action::FreeCameraUp)) - float(is_down(Action::FreeCameraDown)) : 0,
        camera_enabled ? float(is_down(Action::FreeCameraForward)) - float(is_down(Action::FreeCameraBack)) : 0);
    const bool reset_pressed = is_down(Action::FreeCameraReset);
    if (!reset_pressed) suppress_free_camera_reset = false;
    const bool reset = camera_enabled && reset_pressed && !suppress_free_camera_reset;
    if (reset && !last_free_camera_reset) tooie::camera::request_free_camera_reset();
    last_free_camera_reset = reset;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    persistent_input_quarantine = false;
    // A restore may have arrived while this poll was publishing old physical
    // state. Re-apply its atomic neutral gate before returning to the guest.
    apply_persistent_input_reset();
#endif
}
Snapshot snapshot() noexcept {
    return {static_cast<std::uint16_t>(published_buttons.load(std::memory_order_acquire)),
        published_x.load(std::memory_order_acquire) / 32767.0f,
        published_y.load(std::memory_order_acquire) / 32767.0f,
        published_rx.load(std::memory_order_acquire) / 32767.0f,
        published_ry.load(std::memory_order_acquire) / 32767.0f,
        published_enabled.load(std::memory_order_acquire)};
}
std::uint16_t consume_transient_buttons() noexcept {
    return static_cast<std::uint16_t>(transient_buttons.exchange(0, std::memory_order_acq_rel));
}
MenuGamepadState menu_gamepad_state() noexcept { return published_menu_pad; }
bool game_input_disabled() noexcept { return !published_enabled.load(std::memory_order_acquire); }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
void persistent_reset_transients() noexcept {
    persistent_input_reset_pending.store(true, std::memory_order_release);
    // The UI tick will observe this and send a zero-strength update to stop
    // any pre-restore pulse; the next guest rumble command may enable it again.
    desired_rumble.store(false, std::memory_order_release);
    transient_buttons.store(0, std::memory_order_release);
    published_buttons.store(0, std::memory_order_release);
    published_x.store(0, std::memory_order_release);
    published_y.store(0, std::memory_order_release);
    published_rx.store(0, std::memory_order_release);
    published_ry.store(0, std::memory_order_release);
    published_enabled.store(false, std::memory_order_release);
}
#endif
void get_right_analog(int port, float* x, float* y) noexcept {
    const auto s = snapshot();
    if (x) *x = port == 0 && s.game_enabled ? s.right_x : 0;
    if (y) *y = port == 0 && s.game_enabled ? s.right_y : 0;
}
void set_rumble(int port, bool enabled) noexcept {
    if (port == 0) desired_rumble.store(enabled, std::memory_order_release);
}
bool physical_controller() noexcept { return published_physical.load(std::memory_order_acquire); }
bool rumble_capable() noexcept { return published_rumble.load(std::memory_order_acquire); }
void post_diagnostics_toggle() noexcept { diagnostics_toggle_pending.store(true, std::memory_order_release); }
bool consume_diagnostics_toggle() noexcept { return diagnostics_toggle_pending.exchange(false, std::memory_order_acq_rel); }
void post_issue_marker() noexcept { issue_marker_pending.store(true, std::memory_order_release); }
bool consume_issue_marker() noexcept { return issue_marker_pending.exchange(false, std::memory_order_acq_rel); }
void note_relevant_keydown(std::uint32_t scancode, bool repeat, bool game_started,
    bool keyboard_focus, bool all_input_disabled, bool context_capture, bool binding_scan, bool skip_events) noexcept {
    total_keydowns.fetch_add(1, std::memory_order_relaxed);
    const bool escape = scancode == SDL_SCANCODE_ESCAPE;
    const bool enter = scancode == SDL_SCANCODE_RETURN;
    const bool function = scancode >= SDL_SCANCODE_F3 && scancode <= SDL_SCANCODE_F7;
    if (!escape && !enter && !function) return;
    relevant_keydowns.fetch_add(1); if (escape) escape_keydowns.fetch_add(1);
    if (enter) return_keydowns.fetch_add(1); if (function) function_keydowns.fetch_add(1);
    last_scancode.store(scancode); last_repeat.store(repeat); last_game_started.store(game_started);
    last_keyboard_focus.store(keyboard_focus); last_all_input_disabled.store(all_input_disabled);
    last_context_capture.store(context_capture); last_binding_scan.store(binding_scan);
    last_skip_events.store(skip_events);
}
void note_discrete_action(DiscreteInputAction action) noexcept {
    switch (action) {
    case DiscreteInputAction::SaveProgress: accepted_save_progress.fetch_add(1); break;
    case DiscreteInputAction::ToggleDiagnostics: accepted_diagnostics.fetch_add(1); break;
    case DiscreteInputAction::MarkIssue: accepted_issue_markers.fetch_add(1); break;
    case DiscreteInputAction::FastForward: accepted_fast_forward.fetch_add(1); break;
    case DiscreteInputAction::SkipCutscene: accepted_cutscene_skips.fetch_add(1); break;
    }
}
InputDebugSnapshot input_debug_snapshot() noexcept {
    return {total_keydowns.load(), relevant_keydowns.load(), escape_keydowns.load(),
        return_keydowns.load(), function_keydowns.load(), accepted_save_progress.load(),
        accepted_diagnostics.load(), accepted_issue_markers.load(), accepted_fast_forward.load(),
        accepted_cutscene_skips.load(), last_scancode.load(), last_repeat.load(),
        last_game_started.load(), last_keyboard_focus.load(), last_all_input_disabled.load(),
        last_context_capture.load(), last_binding_scan.load(), last_skip_events.load()};
}
} // namespace tooie::input
