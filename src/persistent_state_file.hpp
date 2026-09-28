#pragma once
#include "persistent_state_snapshot.hpp"
#include <filesystem>

namespace tooie::persistent_state::file {
// These functions never inspect or modify a running guest. The coordinator must
// obtain/commit the image under its whole-machine lease. load_validated checks
// the envelope and bounds; scheduler/renderer semantic validation is additional.
void save_atomic(const std::filesystem::path& path, const Snapshot& snapshot);
Snapshot load_validated(const std::filesystem::path& path,
    const Compatibility& expected);
}
