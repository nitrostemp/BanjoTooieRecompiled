#include "save_progress.hpp"
#include "save_persistence.hpp"
#include <cassert>
#include <cstring>
#include <vector>

namespace { bool write_requested = true; }
extern "C" void func_800FC6B0(uint8_t*, recomp_context* ctx) { ctx->r8 = 55; }
extern "C" void func_800D389C(uint8_t*, recomp_context* ctx) {
    if (write_requested) tooie::save_persistence::buffer_changed();
    ctx->r9 = 99;
}
int main() {
    std::vector<uint8_t> storage(8 * 1024 * 1024);
    auto* rdram = storage.data();
    constexpr auto base = static_cast<gpr>(static_cast<int32_t>(0x80000000U));
    MEM_B(0x12762C, base) = 1;
    MEM_B(0x12B3F1, base) = 0;
    MEM_B(0x100, base) = 2;
    recomp_context ctx{}; ctx.r8 = 123; ctx.r9 = 456;
    const auto before = ctx;
    using namespace tooie::save_progress;
    reset(); request();
    tooie_save_progress_pause_tick(rdram, &ctx, 0x80000100U);
    assert(std::memcmp(&ctx, &before, sizeof(ctx)) == 0);
    assert(status() == Status::SubmittedToGame);
    const auto generation = tooie::save_persistence::generation();
    tooie::save_persistence::acknowledge(generation - 1, true);
    assert(status() == Status::SubmittedToGame); // Older autosave is insufficient.
    tooie::save_persistence::acknowledge(generation, true);
    assert(status() == Status::Persisted);
    request(); tooie_save_progress_pause_tick(rdram, &ctx, 0x80000100U);
    tooie::save_persistence::acknowledge(tooie::save_persistence::generation(), false);
    assert(status() == Status::PersistenceFailed);
    write_requested = false;
    request(); tooie_save_progress_pause_tick(rdram, &ctx, 0x80000100U);
    assert(status() == Status::RejectedUnavailable);
}
