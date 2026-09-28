#include "frontend_prompt.hpp"

#include "frontend_input_preference.hpp"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace {

using tooie::input::Action;
using tooie::input::Device;

struct PromptAction {
    Action action;
    const char* game_button;
};

// These are N64 actions used in ordinary gameplay and the pause menu. The
// physical label is looked up from the current Controls mapping each frame.
constexpr std::array<PromptAction, 4> actions{{
    {Action::A, "A"}, {Action::B, "B"}, {Action::Z, "Z"}, {Action::Start, "Start"},
}};

std::atomic_bool configured_visible{false};
std::atomic_uint configured_corner{static_cast<unsigned>(tooie::frontend::prompt::Corner::BottomRight)};

void append_unique(std::vector<std::string>& labels, const std::string& label) {
    if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
}

std::string physical_label(Action action) {
    std::vector<std::string> labels;
    const bool controller_selected = tooie::input::selected_controller_instance() >= 0;
    if (controller_selected) {
        const auto type = tooie::input::selected_controller_type();
        for (unsigned slot = 0; slot < 2; ++slot) {
            for (const Device device : {Device::Button, Device::AxisPositive, Device::AxisNegative}) {
                const auto binding = tooie::input::get_binding(action, device, slot);
                if (binding.code >= 0)
                    append_unique(labels, tooie::input::binding_label_for_type(binding, type));
            }
        }
    }
    // Keyboard remains an ordinary gameplay input and is the useful fallback
    // when no mapped physical control exists on the selected controller.
    if (labels.empty()) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto binding = tooie::input::get_binding(action, Device::Keyboard, slot);
            if (binding.code >= 0) append_unique(labels, tooie::input::binding_label(binding));
        }
    }
    if (labels.empty()) return "Unbound";
    std::string result;
    for (const auto& label : labels) {
        if (!result.empty()) result += " / ";
        result += label;
    }
    return result;
}

} // namespace

namespace tooie::frontend::prompt {

void set_visible(bool value) noexcept {
    configured_visible.store(value, std::memory_order_release);
}

bool visible() noexcept {
    return configured_visible.load(std::memory_order_acquire);
}

void set_corner(Corner value) noexcept {
    if (value > Corner::BottomRight) value = Corner::BottomRight;
    configured_corner.store(static_cast<unsigned>(value), std::memory_order_release);
}

Corner corner() noexcept {
    return static_cast<Corner>(configured_corner.load(std::memory_order_acquire));
}

void render(int width, int height, bool practice_visible, Corner practice_corner) {
    if (!visible() || width < 160 || height < 120) return;
    const auto selected_corner = corner();
    // When both opt-in panels request one corner, use the other side for the
    // legend if there is enough horizontal room. Keep the configured corner.
    const bool same_corner = practice_visible && selected_corner == practice_corner;
    const bool mirror_horizontally = same_corner && width >= 840;
    const bool right = (selected_corner == Corner::TopRight || selected_corner == Corner::BottomRight)
        != mirror_horizontally;
    const bool bottom = selected_corner == Corner::BottomLeft || selected_corner == Corner::BottomRight;
    constexpr float margin = 18.0f;
    ImGui::SetNextWindowPos(ImVec2(right ? float(width) - margin : margin,
        bottom ? float(height) - margin : margin), ImGuiCond_Always,
        ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    const float panel_width = std::min(300.0f, float(width) - 2.0f * margin);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel_width, 0.0f),
        ImVec2(panel_width, float(height) - 2.0f * margin));
    ImGui::SetNextWindowBgAlpha(0.78f);
    if (ImGui::Begin("##tooie_mapped_prompts", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted("Mapped controls");
        for (const auto& action : actions) {
            const std::string line = std::string(action.game_button) + "  " + physical_label(action.action);
            ImGui::TextWrapped("%s", line.c_str());
        }
    }
    ImGui::End();
}

} // namespace tooie::frontend::prompt
