#pragma once

#include <cstdint>

namespace tooie::frontend::prompt {

// A host-rendered mapping legend. It does not replace game-owned artwork.
enum class Corner : std::uint8_t {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
};

void set_visible(bool value) noexcept;
bool visible() noexcept;
void set_corner(Corner value) noexcept;
Corner corner() noexcept;
void render(int width, int height, bool practice_visible, Corner practice_corner);

} // namespace tooie::frontend::prompt
