#include "model_draw_trace_window.hpp"

#include <cassert>

int main() {
    using tooie::model_interpolation::DrawTraceWindow;
    DrawTraceWindow window;
    assert(window.request(0) == 0);
    assert(window.request(1) == 1);
    assert(window.active);
    assert(window.record_draw());
    assert(window.request(2) == 2); // A distinct F4 starts a fresh bounded window.
    assert(window.active);
    assert(window.draws == 0);
    assert(window.tasks == 0);
    assert(window.record_draw());
    assert(window.request(2) == 0);
    assert(window.request(1) == 0); // Stale requests cannot truncate a newer file.
    assert(window.request(0) == 0);
    assert(window.draws == 1);
    for (std::uint32_t i = 1; i < window.max_tasks; ++i)
        assert(!window.record_task());
    assert(window.record_task());
    assert(!window.active);
    assert(window.request(2) == 0); // No delayed duplicate capture.
    assert(window.request(41) == 41); // Generation gaps keep their actual ID.
    for (std::uint32_t i = 0; i < window.max_draws; ++i)
        assert(window.record_draw());
    assert(!window.record_draw());
    assert(window.record_task()); // Flush the last draw at its submission.
    assert(window.request(42) == 42);
    window.stop();
    assert(!window.record_draw());
    assert(window.request(43) == 43);
    window.cancel(44); // A pending F4 at reset is consumed, not replayed.
    assert(!window.active);
    assert(window.request(44) == 0);
    assert(window.request(45) == 45);
    for (std::uint32_t generation = 46; generation <= 65; ++generation) {
        assert(window.request(generation) == generation);
        assert(window.active);
        assert(window.draws == 0);
        assert(window.tasks == 0);
    }
    assert(window.request(65) == 0); // No wrap or restart after 16 captures.
    window.stop();
    assert(window.request(2) == 0); // Stale after completion stays ignored.
    assert(window.request(66) == 66);
}
