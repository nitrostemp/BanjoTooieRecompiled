#include "overlay_call_trace.hpp"
namespace tooie {void original_observe(const overlays::Event& event){
        trace("overlay_lifecycle","G4",event.operation,event.entry,nullptr,
            {{"overlay_id",event.id},{"header",hex32(event.header)},{"text",hex32(event.text)},
             {"entry",hex32(event.entry)},{"raw_ra",hex32(event.caller)},
             {"observed_callsite",overlays::current_callsite()?Json(hex32(overlays::current_callsite())):Json(nullptr)},
             {"generation",event.generation}});
}}
