#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <json/json.hpp>

namespace RT64 {
struct DisplayList;
struct GBI;
struct Interpreter;
struct State;
}

namespace tooie::graphics_branch_capture {

// Install only around an F4-claimed graphics task on RT64's graphics thread.
// The source GBI is required to be F3DEX2 with RT64's original BRANCH_Z handler.
class Scope {
public:
    static constexpr std::size_t max_samples = 256;

    explicit Scope(RT64::Interpreter& interpreter) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // Restore before processing the next graphics task; the destructor is backup.
    void stop() noexcept;
    nlohmann::json snapshot_json() const;

private:
    using Handler = void (*)(RT64::State*, RT64::DisplayList**);

    struct Sample {
        std::uint32_t command_offset = 0;
        std::uint32_t target_segmented = 0;
        std::uint32_t target_physical = 0;
        std::uint32_t threshold_raw = 0;
        std::uint32_t vertex_index = 0;
        float screen_z = 0.0f;
        float clip_w = 0.0f;
        bool vertex_available = false;
        bool expected_branch_condition = false;
        bool pointer_changed = false;
    };

    static void observe(RT64::State* state, RT64::DisplayList** dl);
    static thread_local Scope* owner_;

    RT64::GBI* gbi_ = nullptr;
    Handler original_ = nullptr;
    std::array<Sample, max_samples> samples_{};
    std::uint64_t total_calls_ = 0;
    std::size_t recorded_ = 0;
    const char* status_ = "not_installed";
    bool installed_ = false;
};

} // namespace tooie::graphics_branch_capture
