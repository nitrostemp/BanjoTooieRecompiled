#include "imgui_backend_metal.hpp"

#define IMGUI_IMPL_METAL_CPP
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_metal.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include "plume_metal.h"

namespace tooie::imgui_backend::metal {
namespace {
bool s_initialized = false;

// Present-thread Metal calls run outside the main run loop's pool.
struct AutoreleasePool {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    ~AutoreleasePool() { pool->release(); }
};
}

bool initialize(plume::RenderDevice* device, SDL_Window* window) {
    AutoreleasePool pool;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForMetal(window);
    if (!ImGui_ImplMetal_Init(static_cast<plume::MetalDevice*>(device)->mtl)) {
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    s_initialized = true;
    return true;
}

void process_event(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void new_frame(plume::RenderCommandList* command_list) {
    AutoreleasePool pool;
    const auto* list = static_cast<plume::MetalCommandList*>(command_list);
    // ImGui keys its pipeline on the target's formats and sample count.
    MTL::RenderPassDescriptor* target = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (list->targetFramebuffer && !list->targetFramebuffer->colorAttachments.empty())
        target->colorAttachments()->object(0)->setTexture(
            list->targetFramebuffer->colorAttachments[0].getTexture());
    ImGui_ImplMetal_NewFrame(target);
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void render(plume::RenderCommandList* command_list) {
    AutoreleasePool pool;
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    auto* list = static_cast<plume::MetalCommandList*>(command_list);
    if (!draw_data || !list->targetFramebuffer) return;
    list->checkActiveRenderEncoder();
    ImGui_ImplMetal_RenderDrawData(draw_data, list->mtl, list->activeRenderEncoder);
    // ImGui changed encoder state behind plume's cache; end the encoder so
    // any later plume work starts from a fresh, fully bound one.
    list->endActiveRenderEncoder();
}

void shutdown() noexcept {
    if (!s_initialized) return;
    AutoreleasePool pool;
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    s_initialized = false;
}

} // namespace tooie::imgui_backend::metal
