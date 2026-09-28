#pragma once
#include "tooie_overlays.hpp"
#include "trace.hpp"
namespace tooie {
void configure_overlay_call_observation(bool enabled); // Before start_trace/workers.
void check_overlay_call_observation_health();
void observe_overlay_lifecycle(const overlays::Event& event);
Json overlay_call_observation_snapshot(bool after_all_resource_joins=false);
void finish_overlay_call_observation(); // After joins, quit and RDRAM release; may throw.
}
