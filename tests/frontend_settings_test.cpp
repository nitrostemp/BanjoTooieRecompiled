#include "frontend_settings.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    using namespace tooie::frontend;

    static_assert(static_cast<uint32_t>(OutputRateMode::Original) == 0);
    static_assert(static_cast<uint32_t>(OutputRateMode::Display) == 1);
    static_assert(static_cast<uint32_t>(OutputRateMode::Custom) == 2);
    static_assert(static_cast<uint32_t>(PresentationMode::Console) == 0);
    static_assert(static_cast<uint32_t>(PresentationMode::Early) == 1);

    {
        const GraphicsSettings defaults{};
        assert(defaults.vsync);
        assert(defaults.output_rate_mode == OutputRateMode::Display);
        assert(defaults.presentation_mode == PresentationMode::Early);

        const auto launcher_graphics = launcher_graphics_settings();
        assert(launcher_graphics.output_rate_mode == OutputRateMode::Original);
        assert(launcher_graphics.presentation_mode == PresentationMode::Console);
    }

    {
        GraphicsSettings requested{};
        requested.fullscreen = true;
        requested.resolution_multiplier = 4;
        requested.msaa_samples = 4;
        requested.vsync = false;
        requested.output_rate_mode = OutputRateMode::Custom;
        requested.custom_output_rate = 164;
        requested.presentation_mode = PresentationMode::Early;
        const auto result = normalize_graphics(requested, {true, 8});
        assert(result.settings.fullscreen);
        assert(result.settings.resolution_multiplier == 4);
        assert(result.settings.msaa_samples == 4);
        assert(!result.settings.vsync);
        assert(result.settings.output_rate_mode == OutputRateMode::Custom);
        assert(result.settings.custom_output_rate == 164);
        assert(result.settings.presentation_mode == PresentationMode::Early);
        assert(!result.used_fallback);

        const auto rt64 = to_rt64_user_values(result.settings);
        assert(rt64.manual_resolution);
        assert(rt64.resolution_multiplier == 4.0);
        assert(rt64.downsample_multiplier == 1);
        assert(rt64.msaa_samples == 4);
    }

    {
        GraphicsSettings requested{};
        requested.auto_resolution = true;
        requested.resolution_multiplier = 8;
        requested.downsample_multiplier = 4;
        const auto result = normalize_graphics(requested, {true, 8});
        assert(result.settings.auto_resolution);
        assert(result.settings.resolution_multiplier == 1);
        assert(result.settings.downsample_multiplier == 1);
        assert(result.used_fallback);

        const auto rt64 = to_rt64_user_values(result.settings);
        assert(!rt64.manual_resolution);
        assert(rt64.resolution_multiplier == 1.0);
        assert(rt64.downsample_multiplier == 1);
    }

    {
        GraphicsSettings requested{};
        requested.resolution_multiplier = 2;
        requested.downsample_multiplier = 4;
        const auto result = normalize_graphics(requested, {true, 8});
        assert(!result.settings.auto_resolution);
        assert(result.settings.resolution_multiplier == 2);
        assert(result.settings.downsample_multiplier == 4);
        assert(!result.used_fallback);

        const auto rt64 = to_rt64_user_values(result.settings);
        assert(rt64.manual_resolution);
        assert(rt64.resolution_multiplier == 8.0);
        assert(rt64.downsample_multiplier == 4);
    }

    {
        GraphicsSettings requested{};
        requested.resolution_multiplier = 4;
        requested.downsample_multiplier = 4;
        const auto result = normalize_graphics(requested, {true, 8});
        assert(result.settings.resolution_multiplier == 4);
        assert(result.settings.downsample_multiplier == 1);
        assert(result.used_fallback);

        const auto rt64 = to_rt64_user_values(result.settings);
        assert(rt64.resolution_multiplier == 4.0);
        assert(rt64.downsample_multiplier == 1);
    }

    {
        GraphicsSettings requested{};
        requested.resolution_multiplier = 3;
        requested.msaa_samples = 8;
        requested.output_rate_mode = static_cast<OutputRateMode>(99);
        requested.custom_output_rate = 999;
        requested.presentation_mode = static_cast<PresentationMode>(99);
        const auto result = normalize_graphics(requested, {true, 4});
        assert(result.settings.resolution_multiplier == 1);
        assert(result.settings.msaa_samples == 0);
        assert(result.settings.output_rate_mode == OutputRateMode::Original);
        assert(result.settings.custom_output_rate == 240);
        assert(result.settings.presentation_mode == PresentationMode::Console);
        assert(result.used_fallback);
    }

    {
        GraphicsSettings requested{};
        requested.resolution_multiplier = 8;
        requested.msaa_samples = 2;
        requested.custom_output_rate = 1;
        const auto result = normalize_graphics(requested, {false, 8});
        assert(result.settings.resolution_multiplier == 8);
        assert(result.settings.msaa_samples == 0);
        assert(result.settings.custom_output_rate == 20);
        assert(result.used_fallback);
    }

    {
        const auto baseline = baseline_graphics_preset();
        assert(baseline.vsync);
        assert(baseline.output_rate_mode == OutputRateMode::Original);
        assert(baseline.presentation_mode == PresentationMode::Console);
        const auto smooth = display_console_graphics_preset();
        assert(smooth.vsync);
        assert(smooth.output_rate_mode == OutputRateMode::Display);
        assert(smooth.presentation_mode == PresentationMode::Console);
        const auto experiment = display_early_graphics_preset();
        assert(experiment.vsync);
        assert(experiment.output_rate_mode == OutputRateMode::Display);
        assert(experiment.presentation_mode == PresentationMode::Early);
        assert(is_legacy_baseline_graphics(baseline));
        assert(!is_legacy_baseline_graphics(experiment));
    }

    {
        const DisplaySettings saved_gameplay{
            OutputMode::ExclusiveFullscreen,
            OutputDisplayId::Display2,
            OutputResolutionId::R3440x1440,
            OutputFit::PreserveGameAspect};
        const auto launcher = effective_display_settings(saved_gameplay, false);
        assert(launcher.mode == OutputMode::Windowed);
        assert(launcher.display == OutputDisplayId::Current);
        assert(launcher.resolution == OutputResolutionId::DefaultWindowed);
        const auto gameplay = effective_display_settings(saved_gameplay, true);
        assert(gameplay.mode == saved_gameplay.mode);
        assert(gameplay.display == saved_gameplay.display);
        assert(gameplay.resolution == saved_gameplay.resolution);
    }

    {
        DisplaySettings requested{static_cast<OutputMode>(99), static_cast<OutputDisplayId>(99),
            static_cast<OutputResolutionId>(99), static_cast<OutputFit>(99)};
        const auto normalized = normalize_display(requested);
        assert(normalized.settings.mode == OutputMode::Windowed);
        assert(normalized.settings.display == OutputDisplayId::Current);
        assert(normalized.settings.resolution == OutputResolutionId::DefaultWindowed);
        assert(normalized.settings.fit == OutputFit::PreserveGameAspect);
        assert(normalized.used_fallback);
    }

    {
        std::array<std::int16_t, 5> samples{-32768, -1, 0, 1, 32767};
        apply_main_volume(samples, 50);
        assert(samples[0] == -16384);
        assert(samples[1] == -1);
        assert(samples[2] == 0);
        assert(samples[3] == 1);
        assert(samples[4] == 16384);
        apply_main_volume(samples, 0);
        for (const auto sample : samples) assert(sample == 0);
    }

    return 0;
}
