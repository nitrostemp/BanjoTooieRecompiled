#include "core1_bridge.hpp"
#include <array>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Memory = std::vector<uint8_t>;
constexpr uint32_t ram_size = 8 * 1024 * 1024;
// Independent byte access and literal addresses from linked ELF audit.
uint8_t byte(const Memory& ram, uint32_t addr) { return ram.at((addr & 0x1FFFFFFF) ^ 3); }
void put_byte(Memory& ram, uint32_t addr, uint8_t value) { ram.at((addr & 0x1FFFFFFF) ^ 3) = value; }
uint32_t word(const Memory& ram, uint32_t addr) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | byte(ram, addr + i);
    return value;
}
void put_word(Memory& ram, uint32_t addr, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) put_byte(ram, addr + i, value >> (24 - 8 * i));
}
void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
}

int main() {
    try {
        const std::array<uint32_t, 4> tvs{0, 1, 2, 0xFFFFFFFF};
        const std::array<uint32_t, 4> vi{0x02F5B2D2, 0x02E6D354, 0x02E6025C, 0x02E6D354};
        const std::array<uint64_t, 4> clocks{62500000, 101, 0x123456789ABCDEF0ULL, 0x8000000000000001ULL};
        const std::array<uint32_t, 4> vectors{0x80000000, 0x80000080, 0x80000100, 0x80000180};
        const std::array<uint32_t, 4> preamble{0x3C1A8003, 0x275A1FF0, 0x03400008, 0};
        unsigned cases = 0;
        for (unsigned reset : {0u, 1u}) for (unsigned tv = 0; tv < tvs.size(); ++tv) {
            Memory ram(ram_size, 0xA5);
            for (unsigned i = 0; i < 4; ++i) put_word(ram, 0x80031FE0 + i * 4, preamble[i]);
            put_word(ram, 0x80000300, tvs[tv]);
            put_word(ram, 0x8000030C, reset);
            put_word(ram, 0x80041390, clocks[tv] >> 32);
            put_word(ram, 0x80041394, clocks[tv]);
            // Every byte beyond the explicitly modeled writes must survive,
            // including boot globals, PI timing fields and vector/NMI guards.
            Memory expected = ram;
            put_word(expected, 0x8007EAD0, 1);
            const uint64_t scaled = (clocks[tv] * uint64_t(3)) / 4;
            put_word(expected, 0x80041390, scaled >> 32);
            put_word(expected, 0x80041394, scaled);
            put_word(expected, 0x80041398, vi[tv]);
            put_byte(expected, 0x8007EAE4, 7);
            put_byte(expected, 0x8007EB5C, 7);
            for (auto base : vectors) for (unsigned i = 0; i < 4; ++i)
                put_word(expected, base + i * 4, preamble[i]);
            if (!reset) for (unsigned i = 0; i < 64; ++i) put_byte(expected, 0x8000031C + i, 0);
            recomp_context ctx{};
            const bool fr = tv & 1;
            const uint32_t old_sr = 0x0040FF01 | (fr ? 0x04000000 : 0);
            ctx.status_reg = old_sr;
            ctx.mips3_float_mode = fr;
            ctx.f_odd = fr ? &ctx.f1.u32l : &ctx.f0.u32h;
            auto old_odd = ctx.f_odd;
            ctx.r2 = 0x123456789ABCDEF0ULL;
            std::fesetround(FE_DOWNWARD);
            tooie_core1_osInitialize(ram.data(), &ctx);
            require(word(ram, 0x8007EAD0) == 1, "core1 final-ROM marker is missing");
            require(ram == expected, "core1 init bytes or full-RDRAM write guards differ");
            require(ctx.status_reg == (old_sr | 0x20000000), "CU1 enable or other SR bit preservation");
            require(ctx.f_odd == old_odd && bool(ctx.mips3_float_mode) == fr, "FR layout preservation");
            require(ctx.r2 == 0x123456789ABCDEF0ULL, "unmodeled return register changed");
            require(tooie::core1_fcsr_shadow() == 0x01000800, "core1 FCSR shadow");
            require(std::fegetround() == FE_TONEAREST, "core1 nearest rounding");
            ++cases;
        }
        std::cout << "PASS core1 initialization: " << cases
                  << " cold/warm x TV cases, 64-bit clock overflow, FR preservation, exact 8 MiB write guards\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
