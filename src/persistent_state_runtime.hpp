#pragma once
#include "recomp.h"
#include <cstdint>

namespace tooie::persistent_state::runtime {
// Experimental live continuation ownership. Whole-machine capture/restore is
// coordinated separately; attaching a worker alone never admits a checkpoint.
void attach_worker(std::uint8_t* rdram,recomp_context* context,std::uint32_t guest_thread);
void detach_worker() noexcept;
void run_worker(std::uint8_t* rdram,std::uint64_t entry,std::uint64_t sp,std::uint64_t arg);
}
