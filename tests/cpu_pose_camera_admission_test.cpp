#include "model_camera_policy.hpp"

#include <cassert>

int main() {
    using tooie::model_interpolation::CameraPolicyContext;
    using tooie::model_interpolation::interpolate_cpu_pose_camera;
    const CameraPolicyContext gameplay{.level = 2, .save_slot = 0,
        .game_type = 0, .map_available = true};
    assert(interpolate_cpu_pose_camera(gameplay));

    auto context = gameplay;
    context.title_character_draw = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.cutscene = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.scene_activation = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.map_available = false;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.save_slot = -1;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.game_type = 3;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.level = 0x11;
    assert(!interpolate_cpu_pose_camera(context));
    context = gameplay;
    context.level = 0x1C;
    assert(!interpolate_cpu_pose_camera(context));

    // A recorded in-world attract demo has no active player save slot, but
    // uses the same CPU pose and moving camera as ordinary gameplay.
    const CameraPolicyContext demo{.level = 2, .save_slot = -1,
        .game_type = 0, .frontend_mode = 2, .replay_record_available = true,
        .map_available = true};
    assert(interpolate_cpu_pose_camera(demo));
    context = demo;
    context.replay_record_available = false;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.frontend_mode = 1;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.title_character_draw = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.cutscene = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.scene_activation = true;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.map_available = false;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.game_type = 3;
    assert(!interpolate_cpu_pose_camera(context));
    context = demo;
    context.level = 0x11;
    assert(!interpolate_cpu_pose_camera(context));
}
