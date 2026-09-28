#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace tooie::build_metadata {
enum class File : unsigned { Config, Core2Reference, Core1Reference, OverlayValidation, Count };
std::string_view identity();
std::string_view expected_hash(File file);
std::string_view relative_path(File file);
class Bundle {
    std::filesystem::path directory_;
    std::array<std::vector<uint8_t>,static_cast<unsigned>(File::Count)> data_;
public:
    // The content-addressed sibling bundle is mandatory. No source/build-tree
    // fallback exists in a release executable.
    static Bundle load_for_executable(const std::filesystem::path& executable);
    const std::filesystem::path& directory() const {return directory_;}
    std::span<const uint8_t> bytes(File file) const;
    std::string_view text(File file) const;
};
// Thread-safe initialization. Main calls this before any guest workers; focused
// application tests also obtain the same verified bundle on their first use.
const Bundle& current();
}
