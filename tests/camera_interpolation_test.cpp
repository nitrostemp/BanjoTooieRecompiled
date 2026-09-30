#include "camera_interpolation.hpp"
#include "replay_timing.hpp"

#include <cassert>

int main() {
    using namespace tooie::camera_interpolation;

    reset();
    assert(configured_cutscene_motion() == CutsceneMotion::Interpolated);
    assert(bind_task(0x80001000U));
    assert(consume_task_interpolation(0x00001000U) == TaskInterpolation::Interpolated);

    observe_active_camera(0x80100000U);
    assert(bind_task(0x00100000U));
    assert(bind_task(0x00101000U));
    // The active-camera setter selects world/HUD/overlay camera buffers during
    // ordinary rendering. Handle churn is diagnostic only and must not be
    // interpreted as a cut.
    assert(consume_task_interpolation(0x00100000U) == TaskInterpolation::Interpolated);
    assert(consume_task_interpolation(0x00101000U) == TaskInterpolation::Interpolated);

    observe_active_camera(0x80100000U);
    assert(bind_task(0x00102000U));
    assert(consume_task_interpolation(0x00102000U) == TaskInterpolation::Interpolated);

    observe_active_camera(0x80101000U);
    request_skip();
    assert(bind_task(0x00103000U));
    assert(consume_task_interpolation(0x00103000U) == TaskInterpolation::ProjectionReset);
    assert(consumed_skip_count() == 1);

    // ncpod_entrypoint_12 invokes the C seam only after replacing its camera
    // allocation; it must preserve the same one-task projection reset policy.
    tooie_camera_interpolation_request_skip();
    assert(bind_task(0x00103080U));
    assert(consume_task_interpolation(0x00103080U) == TaskInterpolation::ProjectionReset);
    assert(consumed_skip_count() == 2);

    // Cutscene mode is captured at submission, so a later state/config change
    // cannot relabel an already queued task on the renderer thread.
    configure_cutscene_motion(CutsceneMotion::Original);
    set_cutscene_active(true);
    assert(bind_task(0x00103100U));
    set_cutscene_active(false);
    configure_cutscene_motion(CutsceneMotion::Interpolated);
    assert(consume_task_interpolation(0x00103100U) == TaskInterpolation::OriginalMotion);

    configure_cutscene_motion(CutsceneMotion::Original);
    request_skip();
    set_cutscene_active(true);
    assert(bind_task(0x00103200U));
    set_cutscene_active(false);
    // Original motion takes precedence while active, but the one-shot boundary
    // request is consumed by that exact task and cannot leak to the next one.
    assert(consume_task_interpolation(0x00103200U) == TaskInterpolation::OriginalMotion);
    assert(stats().consumed_boundary_skips == 2);
    assert(stats().consumed_original_motion_tasks == 2);

    configure_cutscene_motion(CutsceneMotion::Interpolated);

    assert(bind_task(0x00104000U));
    const auto mismatch = consume_task_metadata(0x00104008U);
    assert(mismatch.interpolation == TaskInterpolation::Interpolated);
    assert(mismatch.replay_refresh_rate == 0);
    assert(stats().disabled && stats().mismatches == 1);
    assert(!bind_task(0x00105000U));

    reset();
    tooie::replay_timing::record_divisor(3, true);
    assert(bind_task(0x003F0000U));
    tooie::replay_timing::begin_record();
    const auto replay_metadata = consume_task_metadata(0x003F0000U);
    assert(replay_metadata.interpolation == TaskInterpolation::Interpolated);
    assert(replay_metadata.replay_refresh_rate == 20);
    assert(stats().consumed_replay_rate_tasks == 1);
    assert(stats().last_replay_refresh_rate == 20);

    reset();
    // The rate is captured per submitted display list, rather than read from
    // mutable replay state on the renderer thread. A later ordinary input
    // entry must not erase either already-queued replay workload.
    tooie::replay_timing::record_divisor(2, true);
    assert(bind_task(0x003F1000U));
    tooie::replay_timing::record_divisor(3, true);
    assert(bind_task(0x003F2000U));
    tooie::replay_timing::begin_record();
    assert(bind_task(0x003F3000U));
    assert(consume_task_metadata(0x003F1000U).replay_refresh_rate == 30);
    assert(consume_task_metadata(0x003F2000U).replay_refresh_rate == 20);
    assert(consume_task_metadata(0x003F3000U).replay_refresh_rate == 0);
    assert(stats().consumed_replay_rate_tasks == 2);
    assert(stats().last_replay_refresh_rate == 20);

    reset();
    for (std::size_t i = 0; i < kTaskBindingCapacity; ++i)
        assert(bind_task(0x00200000U + static_cast<std::uint32_t>(i * 8U)));
    assert(!bind_task(0x00300000U));
    assert(stats().disabled && stats().overflows == 1 && stats().queued_tasks == 0);

    reset();
    observe_active_camera(0);
    request_skip();
    assert(bind_task(0x00400000U));
    assert(consume_skip_for_task(0x00400000U));
}
