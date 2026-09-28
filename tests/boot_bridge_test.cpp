#include "game.hpp"
#include "boot_bridge.hpp"
#include <array>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" void boot_osInitialize(uint8_t*, recomp_context*);
extern "C" void boot___osDisableInt(uint8_t*, recomp_context*);
extern "C" void boot___osRestoreInt(uint8_t*, recomp_context*);
extern "C" void boot___osPiRawStartDma(uint8_t*, recomp_context*);
extern "C" void boot_osPiGetStatus(uint8_t*, recomp_context*);

namespace {
constexpr uint32_t ram_size = 8u * 1024 * 1024;
using Memory = std::vector<uint8_t>;
gpr guest(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error("boot bridge contract: " + std::string(message));
}
// Independent byte helpers deliberately avoid the implementation's MEM_* macros.
uint8_t byte(const Memory& ram, uint32_t address) {
    return ram.at((address & 0x1FFFFFFFu) ^ 3u);
}
void put_byte(Memory& ram, uint32_t address, uint8_t value) {
    ram.at((address & 0x1FFFFFFFu) ^ 3u) = value;
}
uint32_t word(const Memory& ram, uint32_t address) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | byte(ram, address + i);
    return value;
}
void put_word(Memory& ram, uint32_t address, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) put_byte(ram, address + i, value >> (24 - i * 8));
}
uint64_t clock_value(const Memory& ram) {
    return (uint64_t(word(ram, 0x80004250)) << 32) | word(ram, 0x80004254);
}
void put_clock(Memory& ram, uint64_t value) {
    put_word(ram, 0x80004250, value >> 32);
    put_word(ram, 0x80004254, value);
}
void initialize_contract(Memory& ram) {
    const std::array<uint32_t, 4> preamble{0x3C1A8000, 0x275A1CA0, 0x03400008, 0};
    const std::array<uint32_t, 4> vectors{0x80000000, 0x80000080, 0x80000100, 0x80000180};
    for (unsigned i = 0; i < preamble.size(); ++i) put_word(ram, 0x80001C90 + i * 4, preamble[i]);
    put_word(ram, 0x80004260, 0x003FFF01);
    const std::array<uint32_t, 4> tvs{0, 1, 2, 0xFFFFFFFF};
    const std::array<uint32_t, 4> vi_clocks{0x02F5B2D2, 0x02E6D354, 0x02E6025C, 0x02E6D354};
    for (unsigned test = 0; test < tvs.size(); ++test) {
        recomp_context ctx{};
        const bool fr = test & 1;
        const uint32_t before_sr = 0x0040FF01 | (fr ? 0x04000000 : 0);
        ctx.status_reg = before_sr;
        ctx.mips3_float_mode = fr;
        ctx.f_odd = fr ? &ctx.f1.u32l : &ctx.f0.u32h;
        const auto before_odd = ctx.f_odd;
        put_word(ram, 0x80000300, tvs[test]);
        put_word(ram, 0x8000030C, test == 0 ? 0 : 1);
        put_word(ram, 0x80006F50, 0xDEADBEEF);
        const uint64_t before_clock = test == 0 ? 62500000 : (test == 3 ? 0x8000000000000001ULL : 101);
        put_clock(ram, before_clock);
        for (uint32_t a = 0x8000031C; a < 0x8000035C; ++a) put_byte(ram, a, uint8_t(a));
        const auto nmi_before = word(ram, 0x80000318);
        const auto nmi_after = word(ram, 0x8000035C);
        for (uint32_t base : vectors) {
            for (unsigned i = 0; i < 16; ++i) put_byte(ram, base + i, 0xCD);
            put_word(ram, base + 16, 0xD15EA5ED);
            if (base != 0x80000000) put_word(ram, base - 4, 0xF00DFACE);
        }
        std::fesetround(FE_DOWNWARD);
        boot_osInitialize(ram.data(), &ctx);
        require(word(ram, 0x80006F50) == 1, "FINALROM flag");
        require(ctx.status_reg == (before_sr | 0x20000000), "SR CU1 and other-bit preservation");
        require(ctx.f_odd == before_odd && bool(ctx.mips3_float_mode) == fr, "FR and float pointer preservation");
        require(tooie::boot_fcsr_shadow() == 0x01000800, "FCSR shadow FS/EV value");
        require(std::fegetround() == FE_TONEAREST, "FCSR nearest rounding");
        require(clock_value(ram) == (before_clock * uint64_t(3)) / 4, "64-bit clock scaling");
        require(word(ram, 0x80004258) == vi_clocks[test], "TV clock branch");
        require(word(ram, 0x80004260) == 0x003FFF01, "global interrupt mask preserved");
        for (uint32_t base : vectors) {
            for (unsigned i = 0; i < 4; ++i) require(word(ram, base + i * 4) == preamble[i], "exception preamble copy");
            require(word(ram, base + 16) == 0xD15EA5ED, "exception vector upper guard");
            if (base != 0x80000000) require(word(ram, base - 4) == 0xF00DFACE, "exception vector lower guard");
        }
        for (uint32_t a = 0x8000031C; a < 0x8000035C; ++a)
            require(byte(ram, a) == (test == 0 ? 0 : uint8_t(a)), "cold clear or warm NMI preservation");
        require(word(ram, 0x80000318) == nmi_before && word(ram, 0x8000035C) == nmi_after, "NMI guards");
    }
}
void interrupt_contract(Memory& ram) {
    for (uint32_t ie : {0u, 1u}) {
        recomp_context ctx{};
        const uint32_t sr = 0x2440A400 | ie;
        ctx.status_reg = sr;
        ctx.f_odd = &ctx.f1.u32l;
        ctx.mips3_float_mode = true;
        ctx.r2 = 0xBAD;
        boot___osDisableInt(ram.data(), &ctx);
        require(ctx.r2 == ie && ctx.status_reg == (sr & ~1u), "disable old IE return token");
        const gpr outer_token = ctx.r2;
        boot___osDisableInt(ram.data(), &ctx);
        require(ctx.r2 == 0 && ctx.status_reg == (sr & ~1u), "nested disable token");
        ctx.r4 = 0;
        ctx.r2 = 0x12345678;
        boot___osRestoreInt(ram.data(), &ctx);
        require(ctx.status_reg == (sr & ~1u) && ctx.r2 == 0x12345678, "restore zero and v0 preservation");
        ctx.status_reg |= 0x00800000;
        ctx.r4 = outer_token;
        boot___osRestoreInt(ram.data(), &ctx);
        require(ctx.status_reg == (sr | 0x00800000), "restore preserves current status changes");
        require(ctx.f_odd == &ctx.f1.u32l && ctx.mips3_float_mode, "interrupt bridges preserve FR layout");
        // Actual assembly ORs the whole argument; it does not mask it to IE.
        ctx.r4 = 0x10;
        boot___osRestoreInt(ram.data(), &ctx);
        require(ctx.status_reg == (sr | 0x00800010), "restore literal OR semantics");
    }
}
void dma_contract(Memory& ram) {
    put_word(ram, 0x80000308, 0xB0000000);
    const auto rom = recomp::get_rom();
    unsigned positive = 0, negative = 0;
    auto request = [](recomp_context& ctx, uint32_t direction, uint32_t offset, uint32_t dest, uint32_t size) {
        ctx.r4 = direction; ctx.r5 = offset; ctx.r6 = guest(dest); ctx.r7 = size;
        ctx.r2 = 0x123456789ABCDEF0ULL;
    };
    const std::array<std::array<uint32_t, 3>, 4> good{{
        {0x1000, 0x80200100, 32}, {0x1002, 0xA0200180, 18},
        {uint32_t(rom.size() - 8), 0x80200200, 8}, {0, 0x807FFFF8, 8}
    }};
    for (auto [offset, dest, size] : good) {
        recomp_context ctx{};
        const auto before = ram;
        request(ctx, 0, offset, dest, size);
        boot___osPiRawStartDma(ram.data(), &ctx);
        require(ctx.r2 == 0, "successful DMA return");
        const uint32_t physical = dest & 0x1FFFFFFF;
        for (uint32_t i = 0; i < ram_size; ++i) {
            if (i >= physical && i < physical + size)
                require(byte(ram, i) == rom[offset + i - physical], "synchronous DMA guest byte order");
            else require(ram[i ^ 3] == before[i ^ 3], "DMA writes confined to destination");
        }
        ctx.r2 = 0xBAD;
        boot_osPiGetStatus(ram.data(), &ctx);
        require(ctx.r2 == 0, "completed synchronous DMA status");
        ++positive;
    }
    struct Bad { const char* name; uint32_t direction, offset, dest, size; };
    const std::array<Bad, 12> bad{{
        {"write direction", 1, 0x1000, 0x80200100, 32},
        {"unknown direction", 2, 0x1000, 0x80200100, 32},
        {"zero size", 0, 0x1000, 0x80200100, 0},
        {"odd size", 0, 0x1000, 0x80200100, 31},
        {"unaligned ROM", 0, 0x1001, 0x80200100, 32},
        {"unaligned RAM", 0, 0x1000, 0x80200104, 32},
        {"ROM end overrun", 0, uint32_t(rom.size() - 8), 0x80200100, 16},
        {"ROM outside", 0, uint32_t(rom.size()), 0x80200100, 8},
        {"RAM end overrun", 0, 0x1000, 0x807FFFF8, 16},
        {"RAM outside", 0, 0x1000, 0x80800000, 8},
        {"physical destination", 0, 0x1000, 0x00200100, 32},
        {"size overflow", 0, 0x1000, 0x80200100, 0xFFFFFFFE}
    }};
    for (const auto& item : bad) {
        recomp_context ctx{};
        request(ctx, item.direction, item.offset, item.dest, item.size);
        const auto before = ram;
        const auto before_v0 = ctx.r2;
        bool rejected = false;
        try { boot___osPiRawStartDma(ram.data(), &ctx); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, std::string(item.name) + " rejected");
        require(ram == before, std::string(item.name) + " all guest bytes unchanged");
        require(ctx.r2 == before_v0, std::string(item.name) + " v0 unchanged on rejection");
        ++negative;
    }
    tooie::trace("boot_bridge_contract", "G", "pass", 0, nullptr,
                 {{"dma_positive_cases", positive}, {"dma_negative_cases", negative},
                  {"negative_guard_scope_bytes", ram_size}, {"initialization_tv_cases", 4},
                  {"interrupt_initial_ie_cases", 2}, {"runtime_threads_started", false}});
    std::cout << "PASS boot initialization, nested interrupt tokens, " << positive
              << " real ROM DMA cases, " << negative << " rejection cases with unchanged 8 MiB and v0\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: boot_bridge_test ORIGINAL_ROM TRACE_DIRECTORY");
        tooie::start_trace(argv[2]);
        const auto identity = tooie::validate_and_install_rom(argv[1]);
        require(identity.size == 32u * 1024 * 1024, "installed original ROM size");
        Memory ram(ram_size, 0xA5);
        initialize_contract(ram);
        interrupt_contract(ram);
        dma_contract(ram);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
