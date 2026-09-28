#include "widescreen.hpp"

#include <cassert>
#include <cmath>

namespace {
bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}
}

int main() {
    using tooie::widescreen::NativeAspect;

    tooie::widescreen::configure_profile(true);
    tooie::widescreen::configure_native_aspect(NativeAspect::Ratio16x9);
    tooie::widescreen::latch_for_game_start();
    assert(tooie::widescreen::adjust_projection_aspect(1.75f, true) == 1.75f);

    tooie::widescreen::configure_native_aspect(NativeAspect::Ratio21x9);
    tooie::widescreen::latch_for_game_start();
    assert(near(tooie::widescreen::adjust_projection_aspect(16.0f / 9.0f, true), 21.0f / 9.0f));
    // The live original flag prevents a scripted 4:3 projection from widening.
    assert(tooie::widescreen::adjust_projection_aspect(4.0f / 3.0f, false) == 4.0f / 3.0f);

    tooie::widescreen::configure_native_aspect(NativeAspect::Ratio32x9);
    tooie::widescreen::latch_for_game_start();
    assert(near(tooie::widescreen::adjust_projection_aspect(16.0f / 9.0f, true), 32.0f / 9.0f));

    tooie::widescreen::configure_native_aspect(NativeAspect::Ratio43x18);
    tooie::widescreen::latch_for_game_start();
    assert(near(tooie::widescreen::adjust_projection_aspect(16.0f / 9.0f, true), 43.0f / 18.0f));

    tooie::widescreen::configure_profile(false);
    tooie::widescreen::latch_for_game_start();
    assert(tooie::widescreen::adjust_projection_aspect(1.25f, true) == 1.25f);
    return 0;
}
