#pragma once

namespace tooie::cutscene_tools {
// A short-lived request, consumed only at the original intro ticker's
// controller-0 Z-button decision. Unsupported scenes retain their original
// behavior.
void request_skip() noexcept;
void reset() noexcept;
}

extern "C" int tooie_cutscene_skip_input(int original_pressed) noexcept;
