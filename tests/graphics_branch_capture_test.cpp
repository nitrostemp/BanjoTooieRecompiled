#include "graphics_branch_capture.hpp"

#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_rsp.h"
#include "hle/rt64_workload_queue.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void no_interrupts() {}

struct Fixture {
    std::vector<std::uint8_t> rdram = std::vector<std::uint8_t>(8U * 1024U * 1024U);
    std::uint32_t mi_interrupt = 0;
    std::unique_ptr<RT64::State> state;
    std::unique_ptr<RT64::Interpreter> interpreter;
    std::unique_ptr<RT64::WorkloadQueue> queue;
    RT64::EnhancementConfiguration enhancement;
    RT64::GBI gbi;
    RT64::DisplayList* command = nullptr;

    Fixture() {
        state = std::make_unique<RT64::State>(rdram.data(), &mi_interrupt, &no_interrupts);
        interpreter = std::make_unique<RT64::Interpreter>();
        queue = std::make_unique<RT64::WorkloadQueue>();
        state->ext.workloadQueue = queue.get();
        state->ext.enhancementConfig = &enhancement;
        state->ext.interpreter = interpreter.get();
        interpreter->setup(state.get());

        gbi.ucode = RT64::GBIUCode::F3DEX2;
        gbi.map[F3DEX2_G_BRANCH_Z] = &RT64::GBI_F3DEX::branchZ;
        interpreter->hleGBI = &gbi;
        state->microcode.half1 = 0x100;
        state->rsp->indices[0] = 0;
        auto& data = queue->workloads[queue->writeCursor].drawData;
        data.posScreen.emplace_back(0.0f, 0.0f, 0.25f);
        data.posTransformed.emplace_back(0.0f, 0.0f, 0.0f, 2.0f);

        command = reinterpret_cast<RT64::DisplayList*>(rdram.data() + 0x200);
        command->w0 = std::uint32_t(F3DEX2_G_BRANCH_Z) << 24;
        command->w1 = 512U << 16;
    }

    void call(RT64::DisplayList*& cursor) {
        gbi.map[F3DEX2_G_BRANCH_Z](state.get(), &cursor);
    }
};

void test_forwarded_decisions_and_explicit_stop() {
    Fixture f;
    const auto original = f.gbi.map[F3DEX2_G_BRANCH_Z];
    tooie::graphics_branch_capture::Scope scope(*f.interpreter);
    assert(f.gbi.map[F3DEX2_G_BRANCH_Z] != original);

    auto* cursor = f.command;
    f.call(cursor);
    assert(cursor == reinterpret_cast<RT64::DisplayList*>(f.rdram.data() + 0x100) - 1);

    f.queue->workloads[f.queue->writeCursor].drawData.posScreen[0][2] = 0.75f;
    cursor = f.command;
    f.call(cursor);
    assert(cursor == f.command);

    // Forced branching takes the same RT64 destination even when depth fails.
    f.enhancement.f3dex.forceBranch = true;
    cursor = f.command;
    f.call(cursor);
    assert(cursor == reinterpret_cast<RT64::DisplayList*>(f.rdram.data() + 0x100) - 1);

    // A taken branch to the next command leaves the parser pointer unchanged.
    // Keep the recorded condition separate from the observed pointer change.
    f.state->microcode.half1 = 0x208;
    cursor = f.command;
    f.call(cursor);
    assert(cursor == f.command);

    scope.stop();
    assert(f.gbi.map[F3DEX2_G_BRANCH_Z] == original);
    const auto snapshot = scope.snapshot_json();
    assert(snapshot.at("status") == "captured");
    assert(snapshot.at("total_calls") == 4);
    assert(snapshot.at("dropped_calls") == 0);
    assert(snapshot.at("samples").at(0).at("expected_branch_condition") == true);
    assert(snapshot.at("samples").at(0).at("pointer_changed") == true);
    assert(snapshot.at("samples").at(1).at("expected_branch_condition") == false);
    assert(snapshot.at("samples").at(1).at("pointer_changed") == false);
    assert(snapshot.at("samples").at(2).at("expected_branch_condition") == true);
    assert(snapshot.at("samples").at(2).at("pointer_changed") == true);
    assert(snapshot.at("samples").at(3).at("expected_branch_condition") == true);
    assert(snapshot.at("samples").at(3).at("pointer_changed") == false);
}

void test_zero_branch_and_exception_restoration() {
    Fixture f;
    const auto original = f.gbi.map[F3DEX2_G_BRANCH_Z];
    try {
        tooie::graphics_branch_capture::Scope scope(*f.interpreter);
        assert(scope.snapshot_json().at("total_calls") == 0);
        throw std::runtime_error("stop display-list processing");
    }
    catch (const std::runtime_error&) {
    }
    assert(f.gbi.map[F3DEX2_G_BRANCH_Z] == original);
}

void test_sample_cap_keeps_forwarding() {
    Fixture f;
    const auto original = f.gbi.map[F3DEX2_G_BRANCH_Z];
    tooie::graphics_branch_capture::Scope scope(*f.interpreter);
    for (std::size_t i = 0; i < tooie::graphics_branch_capture::Scope::max_samples + 1; ++i) {
        auto* cursor = f.command;
        f.call(cursor);
        assert(cursor == reinterpret_cast<RT64::DisplayList*>(f.rdram.data() + 0x100) - 1);
    }
    scope.stop();
    assert(f.gbi.map[F3DEX2_G_BRANCH_Z] == original);
    const auto snapshot = scope.snapshot_json();
    assert(snapshot.at("total_calls") == tooie::graphics_branch_capture::Scope::max_samples + 1);
    assert(snapshot.at("recorded_calls") == tooie::graphics_branch_capture::Scope::max_samples);
    assert(snapshot.at("dropped_calls") == 1);
}

} // namespace

int main() {
    test_forwarded_decisions_and_explicit_stop();
    test_zero_branch_and_exception_restoration();
    test_sample_cap_keeps_forwarding();
}
