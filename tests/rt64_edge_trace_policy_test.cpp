#include "rt64_edge_trace_policy.hpp"

#include <cassert>
#include <limits>

int main() {
    using namespace tooie::rt64_edge_trace;
    assert(sample_enabled(0));
    assert(sample_enabled(7));
    assert(sample_enabled(413));
    assert(!sample_enabled(1));
    assert(!sample_enabled(420));
    assert(scan_count(0, 1) == 0);
    assert(scan_count(100, 2) == 100);
    assert(scan_count(4096, 2) == 512);
    assert(scan_first(0, 512, 4096) == 0);
    assert(scan_first(1, 512, 4096) == 512);
    assert(scan_first(9, 512, 4096) == 512);

    TopEdges<4, int> top;
    top.add(2.0f, 2);
    top.add(4.0f, 4);
    top.add(1.0f, 1);
    top.add(3.0f, 3);
    top.add(5.0f, 5);
    top.add(-1.0f, -1);
    top.add(std::numeric_limits<float>::infinity(), 100);
    assert(top.count == 4);
    assert(top.values[0].value == 5);
    assert(top.values[1].value == 4);
    assert(top.values[2].value == 3);
    assert(top.values[3].value == 2);
    assert(max_rows_per_sample == 8);
    assert(max_file_bytes == (1U << 20));
}
