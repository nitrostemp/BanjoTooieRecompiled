#include "pacing_render_work.hpp"

#include <cassert>
#include <chrono>
#include <cmath>

namespace {
bool near(double actual, double expected) {
    return std::abs(actual - expected) < 0.0001;
}
}

int main() {
    using namespace std::chrono_literals;
    using tooie::pacing::RenderWorkPhase;

    tooie::pacing::reset_render_work();
    auto empty = tooie::pacing::render_work_snapshot();
    assert(empty.process_display_lists.samples == 0);
    assert(empty.process_display_lists.recent_samples == 0);

    tooie::pacing::record_render_work(RenderWorkPhase::ProcessDisplayLists, 2ms);
    tooie::pacing::record_render_work(RenderWorkPhase::ProcessDisplayLists, 4ms);
    tooie::pacing::record_render_work(RenderWorkPhase::ProcessDisplayLists, 60ms);
    tooie::pacing::record_render_work(RenderWorkPhase::UpdateScreen, 1ms);
    tooie::pacing::record_render_work(RenderWorkPhase::UpdateScreen, 0ns);

    auto initial = tooie::pacing::render_work_snapshot();
    assert(near(initial.process_display_lists.last_ms, 60.0));
    assert(near(initial.process_display_lists.max_ms, 60.0));
    assert(near(initial.process_display_lists.recent_average_ms, 22.0));
    assert(near(initial.process_display_lists.recent_max_ms, 60.0));
    assert(initial.process_display_lists.recent_samples == 3);
    assert(initial.process_display_lists.samples == 3);
    assert(initial.process_display_lists.samples_over_50ms == 1);
    assert(initial.update_screen.samples == 1);

    tooie::pacing::record_display_list_cpu(3.25, 2.5);
    auto cpu = tooie::pacing::render_work_snapshot();
    assert(near(cpu.display_list_cpu_last_ms, 3.25));
    assert(near(cpu.display_list_cpu_average_ms, 2.5));
    tooie::pacing::record_display_list_cpu(-1.0, 9.0);
    cpu = tooie::pacing::render_work_snapshot();
    assert(near(cpu.display_list_cpu_last_ms, 3.25));

    // The current-view window is bounded while lifetime evidence is retained.
    for (int i = 0; i < 130; ++i) {
        tooie::pacing::record_render_work(RenderWorkPhase::ProcessDisplayLists, 1ms);
    }
    auto wrapped = tooie::pacing::render_work_snapshot();
    assert(wrapped.process_display_lists.recent_samples == 120);
    assert(near(wrapped.process_display_lists.recent_average_ms, 1.0));
    assert(near(wrapped.process_display_lists.recent_max_ms, 1.0));
    assert(near(wrapped.process_display_lists.max_ms, 60.0));
    assert(wrapped.process_display_lists.samples == 133);
    assert(wrapped.process_display_lists.samples_over_50ms == 1);

    tooie::pacing::reset_render_work();
    auto reset = tooie::pacing::render_work_snapshot();
    assert(reset.process_display_lists.samples == 0);
    assert(reset.process_display_lists.recent_samples == 0);
    assert(near(reset.process_display_lists.recent_average_ms, 0.0));
    assert(reset.update_screen.samples == 0);
    assert(near(reset.display_list_cpu_last_ms, 0.0));
}
