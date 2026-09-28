#include "frontend_settings.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tooie::frontend {

namespace {
bool valid_resolution(unsigned value) noexcept {
    return value == 1 || value == 2 || value == 4 || value == 8;
}

bool valid_downsample(unsigned value) noexcept {
    return value == 1 || value == 2 || value == 4;
}

bool valid_msaa(unsigned value) noexcept {
    return value == 0 || value == 2 || value == 4 || value == 8;
}

bool valid_output_rate_mode(OutputRateMode value) noexcept {
    return value == OutputRateMode::Original || value == OutputRateMode::Display || value == OutputRateMode::Custom;
}

bool valid_presentation_mode(PresentationMode value) noexcept {
    return value == PresentationMode::Console || value == PresentationMode::Early;
}
} // namespace

NormalizedGraphics normalize_graphics(GraphicsSettings requested, GraphicsCapabilities capabilities) noexcept {
    NormalizedGraphics result{requested, false};
    if (!valid_resolution(result.settings.resolution_multiplier)) {
        result.settings.resolution_multiplier = 1;
        result.used_fallback = true;
    }
    if (!valid_downsample(result.settings.downsample_multiplier)) {
        result.settings.downsample_multiplier = 1;
        result.used_fallback = true;
    }
    if (result.settings.auto_resolution &&
        (result.settings.resolution_multiplier != 1 || result.settings.downsample_multiplier != 1)) {
        // RT64's output-matched Auto path chooses its own render resolution and
        // does not define a separate downsampling target.
        result.settings.resolution_multiplier = 1;
        result.settings.downsample_multiplier = 1;
        result.used_fallback = true;
    }
    if (!result.settings.auto_resolution &&
        result.settings.resolution_multiplier * result.settings.downsample_multiplier > 8) {
        // The supported fixed-resolution ceiling is 8x. Reject combinations
        // that would silently request a larger RT64 internal target.
        result.settings.downsample_multiplier = 1;
        result.used_fallback = true;
    }
    if (!valid_msaa(result.settings.msaa_samples) || result.settings.msaa_samples > capabilities.max_msaa_samples ||
        (result.settings.msaa_samples != 0 && !capabilities.programmable_sample_positions)) {
        result.settings.msaa_samples = 0;
        result.used_fallback = true;
    }
    if (!valid_output_rate_mode(result.settings.output_rate_mode)) {
        result.settings.output_rate_mode = OutputRateMode::Original;
        result.used_fallback = true;
    }
    const auto clamped_rate = std::clamp(result.settings.custom_output_rate, 20U, 240U);
    if (clamped_rate != result.settings.custom_output_rate) {
        result.settings.custom_output_rate = clamped_rate;
        result.used_fallback = true;
    }
    if (!valid_presentation_mode(result.settings.presentation_mode)) {
        result.settings.presentation_mode = PresentationMode::Console;
        result.used_fallback = true;
    }
    return result;
}

NormalizedDisplay normalize_display(DisplaySettings requested) noexcept {
    NormalizedDisplay result{requested, false};
    if (result.settings.mode != OutputMode::Windowed && result.settings.mode != OutputMode::BorderlessDesktop &&
        result.settings.mode != OutputMode::ExclusiveFullscreen) {
        result.settings.mode = OutputMode::Windowed;
        result.used_fallback = true;
    }
    const auto display = static_cast<std::uint32_t>(result.settings.display);
    if (display > static_cast<std::uint32_t>(OutputDisplayId::Display4)) {
        result.settings.display = OutputDisplayId::Current;
        result.used_fallback = true;
    }
    const auto resolution = static_cast<std::uint32_t>(result.settings.resolution);
    if (resolution > static_cast<std::uint32_t>(OutputResolutionId::DefaultWindowed)) {
        result.settings.resolution = OutputResolutionId::DefaultWindowed;
        result.used_fallback = true;
    }
    if (result.settings.fit != OutputFit::PreserveGameAspect) {
        result.settings.fit = OutputFit::PreserveGameAspect;
        result.used_fallback = true;
    }
    return result;
}

Rt64UserValues to_rt64_user_values(const GraphicsSettings& settings) noexcept {
    if (settings.auto_resolution) {
        return {false, 1.0, 1, settings.msaa_samples};
    }

    // RT64's multiplier names the internal render scale. Its downsample
    // multiplier then resolves that image to the selected output render scale.
    // This mirrors RecompFrontend's Original/Original2x mapping exactly.
    const auto internal_multiplier = settings.resolution_multiplier * settings.downsample_multiplier;
    return {true, static_cast<double>(internal_multiplier), settings.downsample_multiplier, settings.msaa_samples};
}

GraphicsSettings baseline_graphics_preset() noexcept {
    GraphicsSettings settings{};
    settings.output_rate_mode = OutputRateMode::Original;
    settings.presentation_mode = PresentationMode::Console;
    return settings;
}

GraphicsSettings display_console_graphics_preset() noexcept {
    GraphicsSettings settings{};
    settings.output_rate_mode = OutputRateMode::Display;
    settings.presentation_mode = PresentationMode::Console;
    return settings;
}

GraphicsSettings display_early_graphics_preset() noexcept {
    GraphicsSettings settings{};
    settings.output_rate_mode = OutputRateMode::Display;
    settings.presentation_mode = PresentationMode::Early;
    return settings;
}

GraphicsSettings launcher_graphics_settings() noexcept {
    return baseline_graphics_preset();
}

bool is_legacy_baseline_graphics(const GraphicsSettings& settings) noexcept {
    const auto baseline = baseline_graphics_preset();
    return settings.vsync == baseline.vsync &&
        settings.output_rate_mode == baseline.output_rate_mode &&
        settings.custom_output_rate == baseline.custom_output_rate &&
        settings.presentation_mode == baseline.presentation_mode;
}

DisplaySettings launcher_display_settings() noexcept {
    return {};
}

DisplaySettings effective_display_settings(DisplaySettings requested, bool gameplay_started) noexcept {
    return gameplay_started ? normalize_display(requested).settings : launcher_display_settings();
}

void apply_main_volume(std::span<std::int16_t> samples, unsigned percent) noexcept {
    const double gain = static_cast<double>(std::min(percent, 100U)) / 100.0;
    for (auto& sample : samples) {
        const auto scaled = std::llround(static_cast<double>(sample) * gain);
        sample = static_cast<std::int16_t>(std::clamp<long long>(scaled,
            std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }
}

} // namespace tooie::frontend
