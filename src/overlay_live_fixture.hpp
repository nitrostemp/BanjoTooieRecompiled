#pragma once
#include "recomp.h"
namespace tooie {
// Opt-in terminal diagnostic only. Parent calls this once before guest startup.
void enable_live_overlay_fixture(bool enabled);
// Call at the top of recomp_syscall_handler. Disabled/reentrant calls return
// false. The first gcstatus request runs the fixture and requests clean stop;
// it never continues ordinary startup after success or failure.
bool maybe_run_live_overlay_fixture(uint8_t*,recomp_context*,uint32_t stub);
}
