#include "draw_distance.hpp"

#include <cassert>
#include <limits>

int main() {
    using tooie::draw_distance::ActorDistance;

    assert(tooie::draw_distance::adjust_actor_distance(1200.0f) == 1200.0f);
    tooie::draw_distance::configure_actor_distance(ActorDistance::Extended2x);
    // Config changes remain restart-latched.
    assert(tooie::draw_distance::adjust_actor_distance(1200.0f) == 1200.0f);
    tooie::draw_distance::latch_for_game_start();
    assert(tooie::draw_distance::adjust_actor_distance(1200.0f) == 2400.0f);
    assert(tooie::draw_distance::adjust_actor_distance(0.0f) == 0.0f);
    assert(tooie::draw_distance::adjust_actor_distance(-2.0f) == -2.0f);
    const float infinity = std::numeric_limits<float>::infinity();
    assert(tooie::draw_distance::adjust_actor_distance(infinity) == infinity);

    tooie::draw_distance::configure_actor_distance(ActorDistance::Original);
    tooie::draw_distance::latch_for_game_start();
    assert(tooie::draw_distance::adjust_actor_distance(1200.0f) == 1200.0f);
    return 0;
}
