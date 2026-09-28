#include "widescreen.hpp"
#include "visibility.hpp"

#include <cassert>

int main() {
    using namespace tooie::widescreen;
    assert(!profile_override_active());
    assert(resolve_global_setting(0) == 0);
    assert(resolve_global_setting(7) == 7);

    configure_profile(true);
    tooie::visibility::configure_profile(true);
    assert(!profile_override_active());
    assert(resolve_global_setting(0) == 0);
    latch_for_game_start();
    tooie::visibility::latch_for_game_start();
    assert(profile_override_active());
    assert(latched_enabled());
    assert(resolve_global_setting(0) == 1);
    // Tooie's native widescreen path updates both its projection matrix and
    // CPU frustum. RT64 must therefore preserve that projection rather than
    // widening it a second time, regardless of the retired diagnostic value.
    assert(tooie::visibility::task_projection_override_active());
    assert(tooie::visibility::perspective_adjustment_readout().native_projection_latched);
    tooie::visibility::record_task_applied();
    assert(tooie::visibility::perspective_adjustment_readout().applied_tasks == 1);

    configure_profile(false);
    assert(latched_enabled());
    assert(resolve_global_setting(1) == 1);

    latch_for_game_start();
    tooie::visibility::latch_for_game_start();
    assert(!latched_enabled());
    assert(!tooie::visibility::task_projection_override_active());
    assert(!tooie::visibility::perspective_adjustment_readout().native_projection_latched);
}
