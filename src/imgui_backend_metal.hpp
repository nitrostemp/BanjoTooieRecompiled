#pragma once

#include <SDL.h>

namespace plume {
struct RenderDevice;
struct RenderCommandList;
}

namespace tooie::imgui_backend::metal {

// RT64's Inspector has only D3D12 and Vulkan ImGui renderers. On macOS the
// player menu drives ImGui's Metal renderer directly on the present command
// list, inside the swap-chain pass the present-queue overlay already targets.
bool initialize(plume::RenderDevice* device, SDL_Window* window);
void process_event(const SDL_Event& event);
void new_frame(plume::RenderCommandList* command_list);
void render(plume::RenderCommandList* command_list);
void shutdown() noexcept;

} // namespace tooie::imgui_backend::metal
