#pragma once

#include "common/rt64_user_configuration.h"

namespace plume {
struct RenderDevice;
struct RenderSwapChain;
struct RenderCommandList;
}
namespace RT64 { struct RenderWorker; }

namespace tooie::imgui_backend {

// Called only by the project-owned RT64 present-queue overlay.
void initialize(plume::RenderDevice* device, const plume::RenderSwapChain* swap_chain,
                RT64::UserConfiguration::GraphicsAPI api);
void draw(RT64::RenderWorker* worker, plume::RenderCommandList* command_list);
void renderer_shutdown() noexcept;

} // namespace tooie::imgui_backend
