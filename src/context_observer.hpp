#pragma once
#include <cstddef>

namespace tooie::context_observer {
// Bounded diagnostic allocation observation. Arm before a worker can exit.
// Tokens never recycle; counters outlive the allocation and require no freed read.
using Token = std::size_t;
Token watch_context(void* context);
unsigned deletion_count(Token token);
void wait_for_deletion(Token token);
}
