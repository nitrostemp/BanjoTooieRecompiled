#include "persistent_state_frontend.hpp"

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "camera_interpolation.hpp"
#include "cutscene_tools.hpp"
#include "free_camera.hpp"
#include "frontend_input_preference.hpp"
#include "game_features.hpp"
#include "model_interpolation.hpp"
#include "practice_forms.hpp"
#include "practice_state.hpp"
#include "practice_travel.hpp"
#include "runtime_dp.hpp"
#include "scene_observer.hpp"

namespace tooie::persistent_state::frontend {
namespace {
void set_reason(std::string& reason, const char* text) noexcept {
    try { reason = text; } catch (...) {}
}
bool optional_idle(std::string& reason) noexcept {
    if (scene::snapshot().activation_active) {
        set_reason(reason, "scene activation is active");
        return false;
    }
    if (practice::transition_owner() != practice::TransitionOwner::None) {
        set_reason(reason, "practice transition is pending");
        return false;
    }
    if (!features::persistent_requests_idle()) {
        set_reason(reason, "cheat or progression request is pending");
        return false;
    }
    if (!camera::persistent_neutral()) {
        set_reason(reason, "free camera has native detached state");
        return false;
    }
    const auto camera_tasks = camera_interpolation::stats();
    const auto model_tasks = model_interpolation::stats();
    if (camera_tasks.queued_tasks != 0 || model_tasks.queued_tasks != 0 ||
        model_tasks.pending_ranges != 0) {
        set_reason(reason, "interpolation task metadata is pending");
        return false;
    }
    return true;
}
}

bool capture_frozen(HostState& output, std::string& reason) noexcept {
    if (!optional_idle(reason)) return false;
    output.dp_status = dp::persistent_status();
    output.cutscene_active = features::cutscene_active();
    return true;
}

bool validate_restore(const HostState&, std::string& reason) noexcept {
    // Pure preflight before old native stacks unwind. A UI request may be
    // discarded at commit, but a transition already in flight is unsafe.
    if (scene::snapshot().activation_active ||
        practice::transition_owner() != practice::TransitionOwner::None) {
        set_reason(reason, "live scene or practice transition is active");
        return false;
    }
    return true;
}

void restore_epoch(const HostState& input) noexcept {
    camera::persistent_reset_epoch();
    tooie::input::persistent_reset_transients();
    practice::travel::reset();
    practice::forms::reset();
    practice::reset();
    cutscene_tools::reset();
    camera_interpolation::reset();
    model_interpolation::reset();
    features::persistent_restore_epoch(input.cutscene_active);
    camera_interpolation::request_skip();
    dp::persistent_restore_status(input.dp_status);
}
} // namespace tooie::persistent_state::frontend
#endif
