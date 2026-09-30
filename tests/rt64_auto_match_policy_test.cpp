#include "rt64_auto_match_policy.hpp"

#include <cassert>

int main() {
    using tooie::rt64_match::allow_world_pair;
    using tooie::rt64_match::guard_auto_skip_mesh;
    assert(guard_auto_skip_mesh(true, true, 4, 4));
    // Captured Mini Sub Challenge meshes share geometry but are different
    // instances more than 2,700 world units apart.
    assert(guard_auto_skip_mesh(true, true, 246, 246));
    assert(guard_auto_skip_mesh(true, true, 252, 252));
    assert(!guard_auto_skip_mesh(true, true, 0, 0));
    assert(!guard_auto_skip_mesh(true, true, 4, 512));
    assert(!guard_auto_skip_mesh(false, true, 4, 4));
    assert(!guard_auto_skip_mesh(true, false, 4, 4));
    assert(!guard_auto_skip_mesh(true, false, 246, 246));
    // Actual fire capture: workload 4180 world 158 was paired with previous
    // workload 4179 world 163, a distinct emitter about 3,684 units away.
    assert(!allow_world_pair(guard_auto_skip_mesh(true, true, 4, 4), 5264.0f, -976.0f, 6126.0f,
        1642.0f, -817.0f, 6780.0f));
    // Ordinary adjacent particle motion remains eligible for interpolation.
    assert(allow_world_pair(guard_auto_skip_mesh(true, true, 4, 4), 1652.0f, -823.0f, 6784.0f,
        1642.0f, -817.0f, 6780.0f));
    // Explicitly identified transforms retain their existing matching policy.
    assert(allow_world_pair(guard_auto_skip_mesh(false, true, 4, 4), 5264.0f, -976.0f, 6126.0f,
        1642.0f, -817.0f, 6780.0f));
    assert(!allow_world_pair(guard_auto_skip_mesh(true, true, 246, 246),
        1845.0f, -928.0f, -2677.0f, -268.0f, 298.0f, -3873.0f));
    assert(!allow_world_pair(guard_auto_skip_mesh(true, true, 252, 252),
        1845.0f, -927.8f, -2677.0f, -268.0f, 300.2f, -3873.0f));
    assert(allow_world_pair(guard_auto_skip_mesh(true, true, 246, 246),
        1845.0f, -928.0f, -2677.0f, 1845.0f, -927.8f, -2677.0f));
    assert(allow_world_pair(guard_auto_skip_mesh(true, true, 246, 246),
        256.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    assert(!allow_world_pair(guard_auto_skip_mesh(true, true, 246, 246),
        257.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    assert(allow_world_pair(guard_auto_skip_mesh(false, true, 246, 246),
        1845.0f, -928.0f, -2677.0f, -268.0f, 298.0f, -3873.0f));
}
