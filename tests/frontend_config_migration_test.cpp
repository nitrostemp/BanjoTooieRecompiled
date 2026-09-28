#include "frontend_config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::string read_file(const std::filesystem::path& file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_file(const std::filesystem::path& file, const char* contents) {
    std::ofstream output(file);
    output << contents;
    assert(output.good());
}
}

int main() {
    namespace config = tooie::frontend::settings;
    const auto root = std::filesystem::temp_directory_path() /
        ("tooie-imgui-config-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto folder = root / "legacy";
    const auto fresh = root / "fresh";
    const auto partial = root / "partial";
    const auto damaged = root / "damaged";
    std::filesystem::create_directories(folder);
    std::filesystem::create_directories(fresh);
    std::filesystem::create_directories(partial);
    std::filesystem::create_directories(damaged);
    write_file(folder / "general.json", R"({"rumble_strength":25,"tooie_analog_camera_enabled":true,"tooie_preferred_controller_guid":"test-controller"})");
    write_file(folder / "graphics.json", R"JSON({"tooie_vsync":true,"tooie_custom_output_rate":60,"res_option":"4x","msaa_option":"2x","tooie_output_display":"Display 2","tooie_output_resolution":"Default window (960 x 720)","tooie_pacing_preset":"Observed smooth: Display + Present Early (experimental)"})JSON");
    write_file(folder / "sound.json", R"({"main_volume":0,"tooie_music_volume":37})");
    const auto old_general = read_file(folder / "general.json");
    const auto old_graphics = read_file(folder / "graphics.json");
    const auto old_sound = read_file(folder / "sound.json");

    config::initialize(folder);
    assert(config::get_number("rumble_strength", 0) == 25);
    assert(config::get_bool("tooie_analog_camera_enabled", false));
    assert(config::get_string("tooie_preferred_controller_guid") == "test-controller");
    assert(config::get_int("main_volume", 100) == 0);
    assert(config::get_int("tooie_music_volume", 100) == 37);
    assert(config::get_int("res_option", 0) == 4);
    assert(config::get_int("msaa_option", 0) == 2);
    assert(config::get_int("tooie_output_display", 0) == 2);
    assert(config::get_int("tooie_output_resolution", 0) == 6);
    assert(config::get_int("tooie_pacing_preset", 0) == 2);
    config::set_bool("tooie_vsync", false);
    config::set_int("tooie_custom_output_rate", 75);
    assert(config::flush());
    config::initialize(folder);
    assert(!config::get_bool("tooie_vsync", true));
    assert(config::get_int("tooie_custom_output_rate", 60) == 75);
    assert(config::get_int("main_volume", 100) == 0);
    assert(read_file(folder / "general.json") == old_general);
    assert(read_file(folder / "graphics.json") == old_graphics);
    assert(read_file(folder / "sound.json") == old_sound);

    config::initialize(fresh);
    assert(config::get_int("tooie_pacing_preset", -1) == 0);
    assert(!config::get_bool("tooie_vsync", true));
    assert(config::get_int("tooie_output_rate_mode", -1) == 1);
    assert(config::get_int("tooie_presentation_mode", -1) == 1);
    assert(config::recovery_notice().empty());

    write_file(partial / "graphics.json", R"({"tooie_presentation_mode":"Console","tooie_vsync":false})");
    config::initialize(partial);
    assert(config::get_int("tooie_pacing_preset", -1) == 0);
    assert(!config::get_bool("tooie_vsync", true));
    assert(config::get_int("tooie_presentation_mode", -1) == 0);

    write_file(damaged / "tooie_imgui.json", "{ damaged canonical settings");
    write_file(damaged / "tooie_imgui.json.bak", R"({"main_volume":41})");
    const auto damaged_primary = read_file(damaged / "tooie_imgui.json");
    const auto damaged_backup = read_file(damaged / "tooie_imgui.json.bak");
    config::initialize(damaged);
    assert(!config::recovery_notice().empty());
    config::set_int("main_volume", 12);
    assert(!config::flush());
    assert(read_file(damaged / "tooie_imgui.json") == damaged_primary);
    assert(read_file(damaged / "tooie_imgui.json.bak") == damaged_backup);

    std::filesystem::remove_all(root);
    std::cout << "Legacy settings preserved; new profiles receive recommended Display/Early pacing; explicit settings survive migration.\n";
}
