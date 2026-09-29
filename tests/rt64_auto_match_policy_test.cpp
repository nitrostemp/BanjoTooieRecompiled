#include "rt64_auto_match_policy.hpp"

#include <cassert>

int main() {
    using tooie::rt64_match::allow_world_pair;
    using tooie::rt64_match::guard_auto_skip_quad;
    assert(guard_auto_skip_quad(true, true, 4, 4));
    assert(!guard_auto_skip_quad(true, true, 5, 5));
    assert(!guard_auto_skip_quad(true, true, 4, 512));
    assert(!guard_auto_skip_quad(false, true, 4, 4));
    assert(!guard_auto_skip_quad(true, false, 4, 4));
    // Actual fire capture: workload 4180 world 158 was paired with previous
    // workload 4179 world 163, a distinct emitter about 3,684 units away.
    assert(!allow_world_pair(guard_auto_skip_quad(true, true, 4, 4), 5264.0f, -976.0f, 6126.0f,
        1642.0f, -817.0f, 6780.0f));
    // Ordinary adjacent particle motion remains eligible for interpolation.
    assert(allow_world_pair(guard_auto_skip_quad(true, true, 4, 4), 1652.0f, -823.0f, 6784.0f,
        1642.0f, -817.0f, 6780.0f));
    // Explicitly identified transforms retain their existing matching policy.
    assert(allow_world_pair(guard_auto_skip_quad(false, true, 4, 4), 5264.0f, -976.0f, 6126.0f,
        1642.0f, -817.0f, 6780.0f));
    // Nonquad AUTO/SKIP draws are outside this particle safeguard.
    assert(allow_world_pair(guard_auto_skip_quad(true, true, 512, 512), 5264.0f, -976.0f, 6126.0f,
        1642.0f, -817.0f, 6780.0f));
}
