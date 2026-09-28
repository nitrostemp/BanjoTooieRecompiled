#include "frontend_diagnostics_overlay.hpp"

#include "frontend_diagnostics.hpp"
#include "frontend_input_preference.hpp"
#include "frontend_config.hpp"
#include "imgui_menu.hpp"
#include "practice_state.hpp"
#include "practice_travel.hpp"
#include "practice_forms.hpp"
#include "virtual_clock.hpp"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace tooie;
std::atomic_uint selected_mode{0};
std::atomic_uint selected_corner{1};
std::function<bool(std::string_view)> issue_sink;
std::chrono::steady_clock::time_point marker_until{};
bool marker_saved=false;

std::string bounded_number(float value, unsigned decimals, bool signed_value = false) {
    if (!std::isfinite(value)) return "--";
    char buffer[32]{};
    // Preserve useful setup precision near ordinary map coordinates, while
    // scientific notation bounds the width for extreme but finite guest data.
    if (std::fabs(value) >= 100000.0f)
        std::snprintf(buffer, sizeof(buffer), signed_value ? "%+.2e" : "%.2e", value);
    else
        std::snprintf(buffer, sizeof(buffer), signed_value ? "%+.*f" : "%.*f",
            static_cast<int>(decimals), value);
    return buffer;
}

std::string held_group(const practice::Sample& sample,
    std::initializer_list<std::pair<unsigned, const char*>> buttons) {
    if (!sample.valid || !sample.buttons_valid) return "--";
    std::string value;
    for (const auto& [bit, name] : buttons) {
        if ((sample.held_buttons & (1U << bit)) == 0) continue;
        if (!value.empty()) value += ' ';
        value += name;
    }
    return value.empty() ? "none" : value;
}

struct PracticeRow { const char* label; std::string value; };

void render_practice_rows(bool marker, int viewport_height) {
    const auto state = practice::snapshot();
    const bool current = state.current.valid;
    const bool camera = current && state.current.camera_valid;
    const bool delta = current && state.displacement_valid;
    const bool stick = current && state.current.stick_valid;
    const bool target = current && state.target_same_map;
    const std::string unavailable = "--";
    const std::array<PracticeRow, 27> rows{{
        {"Guest update", std::to_string(state.observed_updates)},
        {"Map", current ? std::to_string(state.current.map_id) : unavailable},
        {"X (game units)", current ? bounded_number(state.current.position.x, 4) : unavailable},
        {"Y (game units)", current ? bounded_number(state.current.position.y, 4) : unavailable},
        {"Z (game units)", current ? bounded_number(state.current.position.z, 4) : unavailable},
        {"Facing (deg)", current ? bounded_number(state.current.facing_degrees, 3) : unavailable},
        {"Camera yaw (deg)", camera ? bounded_number(state.current.camera_yaw_degrees, 3) : unavailable},
        {"Camera pitch (deg)", camera ? bounded_number(state.current.camera_pitch_degrees, 3) : unavailable},
        {"Delta X / update", delta ? bounded_number(state.displacement_per_update.x, 3, true) : unavailable},
        {"Delta Y / update", delta ? bounded_number(state.displacement_per_update.y, 3, true) : unavailable},
        {"Delta Z / update", delta ? bounded_number(state.displacement_per_update.z, 3, true) : unavailable},
        {"Delta XZ / update", delta ? bounded_number(state.horizontal_distance_per_update, 3) : unavailable},
        {"Move heading (deg)", current && state.moving_angle_valid ? bounded_number(state.moving_angle_degrees, 3) : unavailable},
        {"Physics vertical", current && state.current.vertical_velocity_valid ? bounded_number(state.current.vertical_velocity, 3) : unavailable},
        {"Stick X (prev)", stick ? bounded_number(state.current.stick_x, 3, true) : unavailable},
        {"Stick Y (prev)", stick ? bounded_number(state.current.stick_y, 3, true) : unavailable},
        {"Held main (prev)", held_group(state.current, {{0,"Start"},{1,"Z"},{8,"A"},{9,"B"}})},
        {"Held shoulder", held_group(state.current, {{2,"L"},{3,"R"}})},
        {"Held D-pad", held_group(state.current, {{4,"Up"},{5,"Down"},{6,"Left"},{7,"Right"}})},
        {"Held C", held_group(state.current, {{10,"Left"},{11,"Down"},{12,"Up"},{13,"Right"}})},
        {"Target map", state.target_valid ? std::to_string(state.target.map_id) : unavailable},
        {"Target X delta", target ? bounded_number(state.target_delta.x, 4, true) : unavailable},
        {"Target Y delta", target ? bounded_number(state.target_delta.y, 4, true) : unavailable},
        {"Target Z delta", target ? bounded_number(state.target_delta.z, 4, true) : unavailable},
        {"Target facing", target ? bounded_number(state.target_facing_delta_degrees, 3, true) : unavailable},
        {"Fast forward", timing::fast_forward_active() ? std::to_string(timing::rate()) + "x (audio muted)" : "off"},
        {"Issue marker", marker ? (marker_saved ? "saved" : "write failed") : "--"},
    }};
    // Derive a fixed (value-independent) local scale from the immutable label
    // set, worst-case bounded value and 28 one-line text slots. The enclosing
    // menu may use up to 1.35x text, but this panel still fits 960x720.
    float widest_label = 0.0f;
    for (const auto& row : rows)
        widest_label = std::max(widest_label, ImGui::CalcTextSize(row.label).x);
    const float widest_value = std::max({
        ImGui::CalcTextSize("18446744073709551615").x,
        ImGui::CalcTextSize("Up Down Left Right").x,
        ImGui::CalcTextSize("-9.9999e+38").x});
    const float horizontal_factor = ImGui::GetContentRegionAvail().x /
        (widest_label + widest_value + 32.0f);
    constexpr float lines = 28.0f; // 27 fixed data rows and one fixed title.
    const float vertical_budget = float(viewport_height - 36) -
        ImGui::GetStyle().WindowPadding.y * 2.0f - 12.0f;
    const float vertical_factor = (vertical_budget - lines * 2.0f) /
        (lines * ImGui::GetFontSize());
    ImGui::SetWindowFontScale(std::clamp(std::min(horizontal_factor, vertical_factor),
        0.70f, 1.0f));
    ImGui::TextUnformatted("Practice / previous guest update input");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 1.0f));
    widest_label = 0.0f;
    for (const auto& row : rows)
        widest_label = std::max(widest_label, ImGui::CalcTextSize(row.label).x);
    if (ImGui::BeginTable("##practice_rows", 2,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed,
            widest_label + 12.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.label);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.value.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(1.0f);
}
}

namespace tooie::diagnostics::overlay {
void set_mode(Mode mode) noexcept { selected_mode.store(unsigned(mode),std::memory_order_release); }
void set_corner(Corner corner) noexcept { selected_corner.store(unsigned(corner),std::memory_order_release); }
void set_issue_marker_sink(std::function<bool(std::string_view)> sink) { issue_sink=std::move(sink); }
Mode mode() noexcept { return static_cast<Mode>(selected_mode.load(std::memory_order_acquire)); }
Corner corner() noexcept { return static_cast<Corner>(selected_corner.load(std::memory_order_acquire)); }
bool visible() noexcept { return mode()!=Mode::Off || timing::fast_forward_active() ||
    std::chrono::steady_clock::now()<marker_until; }

void poll_input() noexcept {
    if (input::consume_diagnostics_toggle()) {
        const auto next=(unsigned(mode())+1U)%4U;
        set_mode(static_cast<Mode>(next));
        menu::set_int("tooie_diagnostics_overlay",int(next));
    }
    if (input::consume_issue_marker()) {
        marker_saved=false;
        try {
            if (issue_sink) marker_saved=issue_sink(
                diagnostics::issue_summary(diagnostics::snapshot(native_host::pacing_readout())));
        } catch (...) {}
        marker_until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    }
}

void render(int width,int height) {
    if (!visible()) return;
    const bool marker=std::chrono::steady_clock::now()<marker_until;
    const bool compact=mode()==Mode::Fps || menu::is_open();
    const bool practice_full=mode()==Mode::Practice && !compact;
    std::vector<std::string> lines;
    if (!practice_full) {
        lines=marker ? std::vector<std::string>{marker_saved?"Issue marker saved":"Unable to write issue marker"}
            : mode()==Mode::Practice ? std::vector<std::string>{"Practice / guest update " +
                std::to_string(practice::snapshot().observed_updates)}
            : diagnostics::lines(diagnostics::snapshot(native_host::pacing_readout()));
        if (!marker && compact && lines.size()>1) lines.resize(1);
        if (!marker && timing::fast_forward_active()) lines.emplace_back("Fast forward "+std::to_string(timing::rate())+"x (audio muted)");
        if (lines.empty()) return;
    }
    const auto corner=static_cast<Corner>(selected_corner.load(std::memory_order_acquire));
    const bool right=corner==Corner::TopRight||corner==Corner::BottomRight;
    const bool bottom=corner==Corner::BottomLeft||corner==Corner::BottomRight;
    ImGui::SetNextWindowPos(ImVec2(right?float(width)-18.0f:18.0f,bottom?float(height)-18.0f:18.0f),
        ImGuiCond_Always,ImVec2(right?1.0f:0.0f,bottom?1.0f:0.0f));
    if (mode()==Mode::Practice) {
        // A fixed row set and pinned width keep changing values from altering
        // either the panel's height or its text measurement.
        const float practice_width=float(std::max(180,std::min(width-36,450)));
        ImGui::SetNextWindowSizeConstraints(ImVec2(practice_width,0.0f),
            ImVec2(practice_width,float(std::max(height-36,120))));
    }
    ImGui::SetNextWindowBgAlpha(.78f);
    if (ImGui::Begin("##tooie_diagnostics",nullptr,
        ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (practice_full) render_practice_rows(marker, height);
        else for (const auto& line:lines) ImGui::TextUnformatted(line.c_str());
    }
    ImGui::End();
}
void shutdown() noexcept { issue_sink={}; marker_until={}; marker_saved=false; practice::travel::reset(); practice::forms::reset(); practice::reset(); }
}

extern "C" bool tooie_diagnostics_overlay_visible() noexcept { return tooie::diagnostics::overlay::visible(); }
extern "C" void tooie_diagnostics_overlay_poll_input() noexcept { tooie::diagnostics::overlay::poll_input(); }
extern "C" void tooie_diagnostics_overlay_render(int width,int height) { tooie::diagnostics::overlay::render(width,height); }
extern "C" void tooie_diagnostics_overlay_shutdown() noexcept { tooie::diagnostics::overlay::shutdown(); }
