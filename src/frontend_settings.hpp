#pragma once

#include <cstdint>
#include <span>

namespace tooie::frontend {

enum class OutputRateMode : std::uint32_t { Original = 0, Display = 1, Custom = 2 };
enum class PresentationMode : std::uint32_t { Console = 0, Early = 1 };
enum class OutputMode : std::uint32_t { Windowed = 0, BorderlessDesktop = 1, ExclusiveFullscreen = 2 };
enum class OutputDisplayId : std::uint32_t { Current = 0, Display1 = 1, Display2 = 2, Display3 = 3, Display4 = 4 };
enum class OutputResolutionId : std::uint32_t {
    DesktopNative = 0,
    R1280x720 = 1,
    R1920x1080 = 2,
    R2560x1440 = 3,
    R3440x1440 = 4,
    R3840x2160 = 5,
    DefaultWindowed = 6
};
enum class OutputFit : std::uint32_t { PreserveGameAspect = 0 };

struct GraphicsSettings {
    bool fullscreen = false;
    unsigned resolution_multiplier = 1;
    unsigned msaa_samples = 0;
    bool vsync = true;
    OutputRateMode output_rate_mode = OutputRateMode::Display;
    unsigned custom_output_rate = 60;
    PresentationMode presentation_mode = PresentationMode::Early;
    bool auto_resolution = false;
    unsigned downsample_multiplier = 1;
};

struct DisplaySettings {
    OutputMode mode = OutputMode::Windowed;
    OutputDisplayId display = OutputDisplayId::Current;
    OutputResolutionId resolution = OutputResolutionId::DefaultWindowed;
    OutputFit fit = OutputFit::PreserveGameAspect;
};

struct GraphicsCapabilities {
    bool programmable_sample_positions = false;
    unsigned max_msaa_samples = 0;
    bool vsync_toggle = true;
    bool presentation_early = true;
    bool output_rate_custom = true;
};

struct NormalizedGraphics {
    GraphicsSettings settings;
    bool used_fallback = false;
};
struct NormalizedDisplay {
    DisplaySettings settings;
    bool used_fallback = false;
};

struct Rt64UserValues {
    bool manual_resolution = true;
    double resolution_multiplier = 1.0;
    unsigned downsample_multiplier = 1;
    unsigned msaa_samples = 0;
};

NormalizedGraphics normalize_graphics(GraphicsSettings requested, GraphicsCapabilities capabilities) noexcept;
NormalizedDisplay normalize_display(DisplaySettings requested) noexcept;
Rt64UserValues to_rt64_user_values(const GraphicsSettings& settings) noexcept;
GraphicsSettings baseline_graphics_preset() noexcept;
GraphicsSettings display_console_graphics_preset() noexcept;
GraphicsSettings display_early_graphics_preset() noexcept;
GraphicsSettings launcher_graphics_settings() noexcept;
bool is_legacy_baseline_graphics(const GraphicsSettings& settings) noexcept;
DisplaySettings launcher_display_settings() noexcept;
DisplaySettings effective_display_settings(DisplaySettings requested, bool gameplay_started) noexcept;
void apply_main_volume(std::span<std::int16_t> samples, unsigned percent) noexcept;

} // namespace tooie::frontend
