#include "rt64_trace_window.hpp"
#include <cassert>

int main() {
    tooie::rt64_trace::Window window;
    assert(window.request(0) == 0);
    assert(window.request(1) == 1);
    assert(window.generation == 1);
    window.calls = 21;
    assert(window.request(2) == 2); // A new F4 starts a fresh window now.
    assert(window.calls == 0);
    assert(window.generation == 2);
    window.calls = 13;
    assert(window.request(2) == 0); // Duplicate and zero never restart it.
    assert(window.request(1) == 0); // A stale request cannot replace newer data.
    assert(window.request(0) == 0);
    assert(window.calls == 13);
    assert(window.generation == 2);
    assert(window.request(41) == 41); // A skipped generation keeps its identity.
    assert(window.calls == 0);
    for (unsigned generation = 42; generation <= 60; ++generation) {
        window.calls = 17;
        assert(window.request(generation) == generation);
        assert(window.calls == 0);
        assert(window.generation == generation);
    }
    window.calls = window.duration;
    assert(window.request(2) == 0); // Stale after completion also cannot reuse a name.
    assert(window.generation == 60);
}
