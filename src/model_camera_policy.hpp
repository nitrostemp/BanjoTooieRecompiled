#pragma once

#include <cstdint>

namespace tooie::model_interpolation {

// These are source-observed guest values, captured before a graphics task is
// submitted. Missing scene context and title-character provenance fail closed.
struct CameraPolicyContext {
    std::uint8_t level = 0;
    std::int8_t save_slot = -1;
    std::uint8_t game_type = 0;
    std::uint8_t frontend_mode = 0;
    bool replay_record_available = false;
    bool map_available = false;
    bool scene_activation = false;
    bool cutscene = false;
    bool original_cutscene_motion = false;
    bool title_character_draw = false;
};

constexpr bool interpolate_cpu_pose_camera(CameraPolicyContext context) noexcept {
    const bool gameplay_level = context.level < 0x0FU ||
        (context.level < 0x1CU && context.level != 0x11U);
    // Mode 2 routes through gsattract. A valid glrecord sample distinguishes
    // an in-world recorded demo from its title/intro sequence; the sample is
    // reset at each input record boundary and on EOF.
    const bool recorded_demo = context.frontend_mode == 2 && context.replay_record_available;
    return gameplay_level && (context.save_slot >= 0 || recorded_demo) &&
        context.game_type != 3 &&
        context.map_available && !context.scene_activation &&
        // In-world cinematics share the scene camera just like gameplay.
        // Only an explicit original-motion cutscene keeps its guest camera.
        !(context.cutscene && context.original_cutscene_motion) &&
        !context.title_character_draw;
}

} // namespace tooie::model_interpolation
