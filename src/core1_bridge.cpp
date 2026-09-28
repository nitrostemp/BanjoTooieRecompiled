#include "core1_bridge.hpp"
#include "core1_metadata.hpp"
#include <cfenv>
#include <stdexcept>

namespace {
uint32_t fcsr_shadow_value;
gpr guest(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }
}
namespace tooie { uint32_t core1_fcsr_shadow() { return fcsr_shadow_value; } }

extern "C" void tooie_core1_osInitialize(uint8_t* rdram, recomp_context* ctx) {
    using namespace tooie::core1_meta;
    // This is the linked core1 FINALROM/J instance, distinct from boot's globals.
    // MMIO/PIF, PI timing acquisition, cache/TLB effects and full FCSR exception/
    // denormal behavior are outside this bounded first-thread diagnostic.
    if (std::fesetround(FE_TONEAREST))
        throw std::runtime_error("Core1 rounding initialization failed");
    MEM_W(0, guest(finalrom)) = 1;
    ctx->status_reg |= cu1_mask;
    fcsr_shadow_value = tooie::core1_meta::fcsr;
    for (uint32_t vector : {0x80000000u, 0x80000080u, 0x80000100u, 0x80000180u})
        for (unsigned offset = 0; offset < 16; offset += 4)
            MEM_W(offset, guest(vector)) = MEM_W(offset, guest(exception_preamble));
    uint64_t clock = (uint64_t(uint32_t(MEM_W(0, guest(clock_rate)))) << 32)
                   | uint32_t(MEM_W(4, guest(clock_rate)));
    // Original __ll_mul wraps to 64 bits before unsigned division.
    clock = clock * uint64_t(3) / 4;
    MEM_W(0, guest(clock_rate)) = clock >> 32;
    MEM_W(4, guest(clock_rate)) = clock;
    if (MEM_W(0, guest(reset_type)) == 0)
        for (unsigned offset = 0; offset < 64; ++offset)
            MEM_B(offset, guest(nmi_buffer)) = 0;
    uint32_t tv = MEM_W(0, guest(tv_type));
    MEM_W(0, guest(vi_clock)) = tv == 0 ? 0x02F5B2D2u : tv == 2 ? 0x02E6025Cu : 0x02E6D354u;
    MEM_B(0, guest(pi_dom1_type)) = 7;
    MEM_B(0, guest(pi_dom2_type)) = 7;
}
