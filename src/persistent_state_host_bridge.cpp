#include "persistent_state_host_bridge.hpp"

#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "hle/rt64_state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace tooie::native_host::persistent_bridge {
namespace {
constexpr std::uint32_t image_magic = 0x31504452; // "RDP1" in LE.
struct RdpImageV1 {
    std::uint32_t magic = image_magic;
    std::uint32_t version = renderer_image_version;
    std::uint32_t byte_size = 0;
    std::array<std::uint64_t, RDP_TMEM_WORDS> tmem{};
    RT64::LoadTexture texture{};
    std::array<RT64::LoadTile, RDP_TILES> tiles{};
    std::array<std::uint64_t, RDP_TILES> tile_replacement_hashes{};
    std::uint32_t color_address = 0;
    std::uint8_t color_fmt = 0;
    std::uint8_t color_siz = 0;
    std::uint16_t color_width = 0;
    std::uint32_t depth_address = 0;
    RT64::ExtendedAlignment rect_alignment{};
    RT64::ExtendedAlignment scissor_alignment{};
    std::uint8_t rect_aspect = 0;
    std::array<std::uint16_t, RDP_EXTENDED_STACK_SIZE> scissor_left_origins{};
    std::array<std::uint16_t, RDP_EXTENDED_STACK_SIZE> scissor_right_origins{};
    std::uint8_t force_upscale_2d = 0;
    std::uint8_t force_true_bilerp = 0;
    std::uint8_t force_scale_lod = 0;
    std::array<RT64::LoadOperation, RDP_TMEM_WORDS> rice_loads{};
    interop::OtherMode other_mode{};
    std::array<interop::ColorCombiner, RDP_EXTENDED_STACK_SIZE> combiner{};
    std::array<std::array<float, 4>, RDP_EXTENDED_STACK_SIZE> env_color{};
    std::array<std::array<float, 4>, RDP_EXTENDED_STACK_SIZE> prim_color{};
    std::array<std::array<float, 2>, RDP_EXTENDED_STACK_SIZE> prim_lod{};
    std::array<std::array<float, 2>, RDP_EXTENDED_STACK_SIZE> prim_depth{};
    std::array<std::array<float, 4>, RDP_EXTENDED_STACK_SIZE> blend_color{};
    std::array<std::array<float, 4>, RDP_EXTENDED_STACK_SIZE> fog_color{};
    std::array<std::uint32_t, RDP_EXTENDED_STACK_SIZE> fill_color{};
    std::array<RT64::FixedRect, RDP_EXTENDED_STACK_SIZE> scissor_rect{};
    std::array<std::uint8_t, RDP_EXTENDED_STACK_SIZE> scissor_mode{};
    std::uint8_t combiner_size = 0;
    std::uint8_t env_size = 0;
    std::uint8_t prim_size = 0;
    std::uint8_t prim_depth_size = 0;
    std::uint8_t blend_size = 0;
    std::uint8_t fog_size = 0;
    std::uint8_t fill_size = 0;
    std::uint8_t scissor_size = 0;
    std::array<std::int32_t, 6> convert_k{};
    std::array<float, 3> key_center{};
    std::array<float, 3> key_scale{};
    RT64::VIHistory vi_history{};
    std::array<float, 2> texcoord_wrap_point{};
    std::uint16_t refresh_rate = 0;
    std::uint8_t render_to_ram = 0;
    std::uint8_t vertex_test_z_active = 0;
    float dither_noise_strength = 0.0f;
    std::uint8_t extend_rdram = 0;
    std::uint32_t last_screen_factor_counter = 0;
    std::uint32_t dither_random_seed = 0;
};

static_assert(std::is_trivially_copyable_v<RdpImageV1>);
static_assert(sizeof(RdpImageV1) <= 256U * 1024U);

bool valid_stack_size(int size) noexcept {
    return size >= 1 && size <= RDP_EXTENDED_STACK_SIZE;
}

bool valid_image(const RdpImageV1& image) noexcept {
    return image.magic == image_magic && image.version == renderer_image_version &&
        image.byte_size == sizeof(RdpImageV1) &&
        valid_stack_size(image.combiner_size) && valid_stack_size(image.env_size) &&
        valid_stack_size(image.prim_size) && valid_stack_size(image.prim_depth_size) &&
        valid_stack_size(image.blend_size) && valid_stack_size(image.fog_size) &&
        valid_stack_size(image.fill_size) && valid_stack_size(image.scissor_size) &&
        image.force_upscale_2d <= 1 && image.force_true_bilerp <= 3 &&
        image.force_scale_lod <= 1 && image.vertex_test_z_active <= 1 &&
        image.extend_rdram <= 1 && image.color_siz <= 3 && image.color_fmt <= 7 &&
        image.color_width <= 4096 && image.vi_history.historyCursor >= 0 &&
        image.vi_history.historyCursor < int(image.vi_history.history.size()) &&
        image.vi_history.factorCursor >= 0 &&
        image.vi_history.factorCursor < int(image.vi_history.factors.size());
}

template <class T, std::size_t N>
void capture_array(std::array<T, N>& target, const T (&source)[N]) noexcept {
    std::copy_n(source, N, target.begin());
}

template <class T, std::size_t N>
void capture_prefix(std::array<T, N>& target, const T (&source)[N],
    int active) noexcept {
    std::copy_n(source, std::size_t(active), target.begin());
}

template <class T, std::size_t N>
void restore_array(T (&target)[N], const std::array<T, N>& source) noexcept {
    std::copy_n(source.begin(), N, target);
}

template <std::size_t Components, class Vector, std::size_t Count>
void capture_vectors(std::array<std::array<float, Components>, Count>& target,
    const Vector (&source)[Count], int active) noexcept {
    for (int i = 0; i < active; ++i)
        for (std::size_t c = 0; c < Components; ++c)
            target[std::size_t(i)][c] = source[i][int(c)];
}

template <std::size_t Components, class Vector, std::size_t Count>
void restore_vectors(Vector (&target)[Count],
    const std::array<std::array<float, Components>, Count>& source) noexcept {
    for (std::size_t i = 0; i < Count; ++i)
        for (std::size_t c = 0; c < Components; ++c)
            target[i][int(c)] = source[i][c];
}

} // namespace

const char* clean_boundary_reason(const RT64::State& state,
    const RT64::WorkloadQueue& workloads) noexcept {
    if (!state.rdp || !state.rsp) return "renderer_state_missing";
    // Recreating RT64 discards old GPU framebuffer caches. The guest image
    // can be authoritative only when the completed workload wrote color/depth
    // results back to RDRAM (same effective selector as State::fullSync).
    if (!state.ext.emulatorConfig ||
        !(state.extended.renderToRAM < UINT8_MAX
            ? state.extended.renderToRAM != 0
            : state.ext.emulatorConfig->framebuffer.renderToRAM))
        return "renderer_rdram_writeback_disabled";
    const auto& rdp = *state.rdp;
    // NativeRenderer::send_dl resets RSP before every guest task and then
    // loads that task's GBI. No RSP vertex cache or matrix stack spans tasks.
    if (rdp.pendingCommandCurrentBytes != 0 || rdp.pendingCommandRemainingBytes != 0 ||
        !rdp.triPointerBuffer.empty() || !rdp.triPosWorkBuffer.empty() ||
        !rdp.triColWorkBuffer.empty() || !rdp.triTcWorkBuffer.empty())
        return "rdp_partial_command";
    // regionIterators is only scratch returned by insertRegionsTMEM; RT64
    // leaves the last iterators resident after the operation has completed.
    // They point into the old framebuffer manager and are intentionally not
    // part of the image. A fresh manager reconstructs regions from later DLs.
    if (rdp.crashed || !valid_stack_size(rdp.colorCombinerStackSize) ||
        !valid_stack_size(rdp.envColorStackSize) || !valid_stack_size(rdp.primColorStackSize) ||
        !valid_stack_size(rdp.primDepthStackSize) || !valid_stack_size(rdp.blendColorStackSize) ||
        !valid_stack_size(rdp.fogColorStackSize) || !valid_stack_size(rdp.fillColorStackSize) ||
        !valid_stack_size(rdp.scissorStackSize)) return "rdp_state_invalid";
    if (!state.returnAddressStack.empty() || state.hasFramebufferOperationsPending() ||
        state.drawCall.triangleCount != 0) return "renderer_pending_draw";
    if (state.activeSpriteCommand.replacementHash != 0)
        return "renderer_sprite_command_pending";
    if (workloads.writeCursor < 0 || workloads.writeCursor >= int(workloads.workloads.size()))
        return "workload_cursor_invalid";
    const auto& pending = workloads.workloads[workloads.writeCursor];
    if (pending.fbPairCount != 0 || pending.fbPairSubmitted != 0 ||
        !pending.drawData.posFloats.empty() || !pending.drawData.faceIndices.empty() ||
        !pending.drawData.loadOperations.empty() || !pending.drawData.rdpParams.empty())
        return "workload_not_fullsync_clean";
    return nullptr;
}

bool export_image(const RT64::State& state,
    std::vector<std::uint8_t>& output) noexcept {
    if (!state.rdp) return false;
    const auto& rdp = *state.rdp;
    try {
        RdpImageV1 image{};
        image.byte_size = sizeof(RdpImageV1);
        capture_array(image.tmem, rdp.TMEM);
        image.texture = rdp.texture;
        capture_array(image.tiles, rdp.tiles);
        capture_array(image.tile_replacement_hashes, rdp.tileReplacementHashes);
        image.color_address = rdp.colorImage.address;
        image.color_fmt = rdp.colorImage.fmt;
        image.color_siz = rdp.colorImage.siz;
        image.color_width = rdp.colorImage.width;
        image.depth_address = rdp.depthImage.address;
        image.rect_alignment = rdp.extended.global.rect;
        image.scissor_alignment = rdp.extended.global.scissor;
        image.rect_aspect = rdp.extended.global.rectAspect;
        capture_prefix(image.scissor_left_origins, rdp.extended.scissorLeftOriginStack,
            rdp.scissorStackSize);
        capture_prefix(image.scissor_right_origins, rdp.extended.scissorRightOriginStack,
            rdp.scissorStackSize);
        image.force_upscale_2d = rdp.extended.drawExtendedFlags.forceUpscale2D;
        image.force_true_bilerp = rdp.extended.drawExtendedFlags.forceTrueBilerp;
        image.force_scale_lod = rdp.extended.drawExtendedFlags.forceScaleLOD;
        capture_array(image.rice_loads, rdp.rice.lastLoadOpByTMEM);
        image.other_mode = rdp.otherMode;
        capture_prefix(image.combiner, rdp.colorCombinerStack, rdp.colorCombinerStackSize);
        capture_vectors(image.env_color, rdp.envColorStack, rdp.envColorStackSize);
        capture_vectors(image.prim_color, rdp.primColorStack, rdp.primColorStackSize);
        capture_vectors(image.prim_lod, rdp.primLODStack, rdp.primColorStackSize);
        capture_vectors(image.prim_depth, rdp.primDepthStack, rdp.primDepthStackSize);
        capture_vectors(image.blend_color, rdp.blendColorStack, rdp.blendColorStackSize);
        capture_vectors(image.fog_color, rdp.fogColorStack, rdp.fogColorStackSize);
        capture_prefix(image.fill_color, rdp.fillColorStack, rdp.fillColorStackSize);
        capture_prefix(image.scissor_rect, rdp.scissorRectStack, rdp.scissorStackSize);
        capture_prefix(image.scissor_mode, rdp.scissorModeStack, rdp.scissorStackSize);
        image.combiner_size = std::uint8_t(rdp.colorCombinerStackSize);
        image.env_size = std::uint8_t(rdp.envColorStackSize);
        image.prim_size = std::uint8_t(rdp.primColorStackSize);
        image.prim_depth_size = std::uint8_t(rdp.primDepthStackSize);
        image.blend_size = std::uint8_t(rdp.blendColorStackSize);
        image.fog_size = std::uint8_t(rdp.fogColorStackSize);
        image.fill_size = std::uint8_t(rdp.fillColorStackSize);
        image.scissor_size = std::uint8_t(rdp.scissorStackSize);
        capture_array(image.convert_k, rdp.convertK);
        for (int c = 0; c < 3; ++c) {
            image.key_center[std::size_t(c)] = rdp.keyCenter[c];
            image.key_scale[std::size_t(c)] = rdp.keyScale[c];
        }
        image.vi_history = state.viHistory;
        image.texcoord_wrap_point = {state.extended.texcoordWrapPoint[0],
            state.extended.texcoordWrapPoint[1]};
        image.refresh_rate = state.extended.refreshRate;
        image.render_to_ram = state.extended.renderToRAM;
        image.vertex_test_z_active = state.extended.vertexTestZActive;
        image.dither_noise_strength = state.extended.ditherNoiseStrength;
        image.extend_rdram = state.extended.extendRDRAM;
        image.last_screen_factor_counter = state.lastScreenFactorCounter;
        image.dither_random_seed = state.ditherRandomSeed;
        if (!valid_image(image)) return false;
        output.resize(sizeof(image));
        std::memcpy(output.data(), &image, sizeof(image));
        return true;
    } catch (...) {
        return false;
    }
}

bool valid_image_blob(const std::vector<std::uint8_t>& input) noexcept {
    if (input.size() != sizeof(RdpImageV1)) return false;
    RdpImageV1 image{};
    std::memcpy(&image, input.data(), sizeof(image));
    return valid_image(image);
}

bool import_image(RT64::State& state,
    const std::vector<std::uint8_t>& input) noexcept {
    if (!state.rdp || !valid_image_blob(input)) return false;
    RdpImageV1 image{};
    std::memcpy(&image, input.data(), sizeof(image));
    auto& rdp = *state.rdp;
    restore_array(rdp.TMEM, image.tmem);
    rdp.texture = image.texture;
    restore_array(rdp.tiles, image.tiles);
    restore_array(rdp.tileReplacementHashes, image.tile_replacement_hashes);
    rdp.colorImage.address = image.color_address;
    rdp.colorImage.fmt = image.color_fmt;
    rdp.colorImage.siz = image.color_siz;
    rdp.colorImage.width = image.color_width;
    rdp.colorImage.changed = true; // New framebuffer manager must rediscover it.
    rdp.depthImage.address = image.depth_address;
    rdp.depthImage.changed = true;
    rdp.extended.global.rect = image.rect_alignment;
    rdp.extended.global.scissor = image.scissor_alignment;
    rdp.extended.global.rectAspect = image.rect_aspect;
    restore_array(rdp.extended.scissorLeftOriginStack, image.scissor_left_origins);
    restore_array(rdp.extended.scissorRightOriginStack, image.scissor_right_origins);
    rdp.extended.drawExtendedFlags.forceUpscale2D = image.force_upscale_2d;
    rdp.extended.drawExtendedFlags.forceTrueBilerp = image.force_true_bilerp;
    rdp.extended.drawExtendedFlags.forceScaleLOD = image.force_scale_lod;
    restore_array(rdp.rice.lastLoadOpByTMEM, image.rice_loads);
    rdp.otherMode = image.other_mode;
    restore_array(rdp.colorCombinerStack, image.combiner);
    restore_vectors(rdp.envColorStack, image.env_color);
    restore_vectors(rdp.primColorStack, image.prim_color);
    restore_vectors(rdp.primLODStack, image.prim_lod);
    restore_vectors(rdp.primDepthStack, image.prim_depth);
    restore_vectors(rdp.blendColorStack, image.blend_color);
    restore_vectors(rdp.fogColorStack, image.fog_color);
    restore_array(rdp.fillColorStack, image.fill_color);
    restore_array(rdp.scissorRectStack, image.scissor_rect);
    restore_array(rdp.scissorModeStack, image.scissor_mode);
    rdp.colorCombinerStackSize = image.combiner_size;
    rdp.envColorStackSize = image.env_size;
    rdp.primColorStackSize = image.prim_size;
    rdp.primDepthStackSize = image.prim_depth_size;
    rdp.blendColorStackSize = image.blend_size;
    rdp.fogColorStackSize = image.fog_size;
    rdp.fillColorStackSize = image.fill_size;
    rdp.scissorStackSize = image.scissor_size;
    restore_array(rdp.convertK, image.convert_k);
    for (int c = 0; c < 3; ++c) {
        rdp.keyCenter[c] = image.key_center[std::size_t(c)];
        rdp.keyScale[c] = image.key_scale[std::size_t(c)];
    }
    state.viHistory = image.vi_history;
    state.extended.texcoordWrapPoint = hlslpp::float2(image.texcoord_wrap_point[0],
        image.texcoord_wrap_point[1]);
    state.extended.refreshRate = image.refresh_rate;
    state.extended.renderToRAM = image.render_to_ram;
    state.extended.vertexTestZActive = image.vertex_test_z_active != 0;
    state.extended.ditherNoiseStrength = image.dither_noise_strength;
    state.extended.extendRDRAM = image.extend_rdram != 0;
    state.lastScreenFactorCounter = image.last_screen_factor_counter;
    state.ditherRandomSeed = image.dither_random_seed;
    state.rdramCheckPending = true;
    state.lastScreenHash = 0; // Force the first restored VI to upload guest RAM.
    state.lastScreenVI = {};   // Force a fresh presentation epoch.
    // The new workload has empty drawData. Indices cached by the old RSP
    // addressed the old workload, so force each semantic value to be emitted
    // again on the first restored vertex rather than reusing stale indices.
    state.rsp->curViewProjIndex = 0;
    state.rsp->curTransformIndex = 0;
    state.rsp->curFogIndex = 0;
    state.rsp->curLightIndex = 0;
    state.rsp->curLightCount = 0;
    state.rsp->curLookAtIndex = 0;
    state.rsp->projectionIndex = -1;
    state.rsp->projectionMatrixChanged = true;
    state.rsp->viewportChanged = true;
    state.rsp->modelViewProjChanged = true;
    state.rsp->lightsChanged = true;
    state.rsp->fogChanged = true;
    state.rsp->lookAtChanged = true;
    state.rsp->extended.modelMatrixIdStackChanged = true;
    state.rsp->extended.viewProjMatrixIdStackChanged = true;
    state.rsp->indices.fill(0);
    state.rsp->used.reset();
    state.resetDrawCall();
    return true;
}
} // namespace tooie::native_host::persistent_bridge
#endif
