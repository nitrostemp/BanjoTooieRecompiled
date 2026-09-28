// Executes the actual generated wrappers and helpers; the runner supplies their
// retained source and the original ELF .core1 bytes. This is a control-return
// regression, not an independent certification of the upstream float helpers.
#include "recomp.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

static unsigned lookups = 0;
struct BadLookup { uint32_t address; };
extern "C" recomp_func_t* get_function(int32_t address) {
    ++lookups;
    throw BadLookup{uint32_t(address)};
}
void tooie_continuous_poll(uint8_t*, recomp_context*, uint32_t) {}
void tooie_overlay_callsite_push(uint32_t) {}
void tooie_overlay_callsite_pop() {}
void func_80013818(uint8_t*, recomp_context*);
void func_80013ABC(uint8_t*, recomp_context*);
#include "generated_fixture.inc"

static void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

// Independently spell out the four original wrapper preludes/delay slots. The
// unchanged generated helper supplies its actual lookup-table math. Comparing
// all context bytes and RDRAM detects duplicate/missing delay operations and
// register, stack, callee-saved and helper-memory changes, not just final F0.
static void reference(unsigned which, uint8_t* rdram, recomp_context* ctx) {
    if (which == 0) {
        ctx->r1 = S32(0x8004 << 16);
        ctx->f14.u32l = MEM_W(ctx->r1, 0x16CC);
        ctx->r1 = S32(0x8004 << 16);
        ctx->r5 = ctx->r31;
        ctx->f16.u32l = MEM_W(ctx->r1, 0x16D0);
        func_80013818(rdram, ctx);
        ctx->f0.u32l = ctx->f2.u32l;
    } else if (which == 1) {
        ctx->r1 = S32(0x8004 << 16);
        ctx->f14.u32l = MEM_W(ctx->r1, 0x16D4);
        ctx->r1 = 0x43B40000;
        ctx->f16.u32l = ctx->r1;
        ctx->r5 = ctx->r31;
        func_80013818(rdram, ctx);
        ctx->f0.u32l = ctx->f2.u32l;
    } else if (which == 2) {
        ctx->r1 = S32(0x8004 << 16);
        ctx->r7 = ctx->r31;
        ctx->f14.u32l = MEM_W(ctx->r1, 0x1700);
        func_80013ABC(rdram, ctx);
        ctx->r1 = S32(0x8004 << 16);
        ctx->f2.u32l = MEM_W(ctx->r1, 0x1704);
        ctx->f0.fl = ctx->f2.fl - ctx->f0.fl;
    } else {
        ctx->r1 = 0x3E340000;
        ctx->f14.u32l = ctx->r1;
        ctx->r7 = ctx->r31;
        func_80013ABC(rdram, ctx);
        ctx->r1 = 0x42B40000;
        ctx->f2.u32l = ctx->r1;
        ctx->f0.fl = ctx->f2.fl - ctx->f0.fl;
    }
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass word-swapped RDRAM fixture");
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<uint8_t> initial((std::istreambuf_iterator<char>(file)), {});
        require(initial.size() == 8 * 1024 * 1024, "fixture must be 8 MiB");
        const std::array<recomp_func_t*, 4> wrappers = {
            func_800137D4, func_800137F4, func_80013A5C, func_80013A7C};
        const std::array<uint32_t, 4> pcs = {0x800137D4,0x800137F4,0x80013A5C,0x80013A7C};
        const std::array<uint32_t, 3> ras = {0x800329A8,0x800CA2C8,0x87654320};
        const std::array<float, 16> sin_inputs = {
            0.0f,-0.0f,0.125f,-0.125f,0.5f,-0.5f,1.0f,-1.0f,
            3.1415927f,-3.1415927f,45.0f,-45.0f,90.0f,-90.0f,360.0f,-720.0f};
        const std::array<float, 16> acos_inputs = {
            0.0f,-0.0f,0.125f,-0.125f,0.5f,-0.5f,1.0f,-1.0f,
            0.99999f,-0.99999f,0.25f,-0.25f,0.75f,-0.75f,0.875f,-0.875f};
        unsigned cases = 0, returned = 0, bad = 0;
        for (unsigned which = 0; which < wrappers.size(); ++which) {
            for (float input : (which < 2 ? sin_inputs : acos_inputs)) {
                for (uint32_t ra : ras) {
                    recomp_context ctx{};
                    ctx.r16=0x16161616; ctx.r17=0x17171717; ctx.r18=0x18181818;
                    ctx.r19=0x19191919; ctx.r20=0x20202020; ctx.r21=0x21212121;
                    ctx.r22=0x22222222; ctx.r23=0x23232323; ctx.r28=0x28282828;
                    ctx.r29=S32(0x8007FF00); ctx.r30=0x30303030; ctx.r31=S32(ra);
                    ctx.f20.u64=0x2020202020202020; ctx.f22.u64=0x2222222222222222;
                    ctx.f24.u64=0x2424242424242424; ctx.f26.u64=0x2626262626262626;
                    ctx.f28.u64=0x2828282828282828; ctx.f30.u64=0x3030303030303030;
                    ctx.f12.fl=input;
                    auto expected=ctx;
                    auto memory=initial, expected_memory=initial;
                    reference(which, expected_memory.data(), &expected);
                    bool did_return=false;
                    try { wrappers[which](memory.data(), &ctx); did_return=true; ++returned; }
                    catch (BadLookup e) {
                        require(e.address==ra, "unexpected lookup target");
                        if (bad < 4) std::printf("bad lookup wrapper=%08X target=%08X\n",pcs[which],e.address);
                        ++bad;
                    }
                    require(std::memcmp(&ctx,&expected,sizeof(ctx))==0, "context/float bits differ from original helper and delay");
                    require(memory==expected_memory, "helper memory side effects differ");
                    require(ctx.r29==uint64_t(S32(0x8007FF00)) && ctx.r31==uint64_t(S32(ra)), "SP/RA changed");
                    (void)did_return;
                    ++cases;
                }
            }
        }
        std::printf("cases=%u returned=%u bad_lookup=%u lookup_calls=%u\n",cases,returned,bad,lookups);
        return bad ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"regression failed: %s\n",e.what()); return 2;
    }
}
