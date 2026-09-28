#include "graphics_branch_capture.hpp"

#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_rsp.h"
#include "hle/rt64_workload_queue.h"

#include <cstdint>
#include <utility>

namespace tooie::graphics_branch_capture {

thread_local Scope* Scope::owner_ = nullptr;

Scope::Scope(RT64::Interpreter& interpreter) noexcept {
    if (owner_ != nullptr) {
        status_ = "nested_scope";
        return;
    }

    RT64::GBI* gbi = interpreter.hleGBI;
    if (gbi == nullptr || gbi->ucode != RT64::GBIUCode::F3DEX2) {
        status_ = "starting_gbi_not_f3dex2";
        return;
    }

    auto& branch = gbi->map[F3DEX2_G_BRANCH_Z];
    if (branch != &RT64::GBI_F3DEX::branchZ) {
        status_ = "branch_handler_unexpected";
        return;
    }

    gbi_ = gbi;
    original_ = branch;
    owner_ = this;
    branch = &Scope::observe;
    installed_ = true;
    status_ = "installed";
}

Scope::~Scope() noexcept {
    stop();
}

void Scope::stop() noexcept {
    if (!installed_) return;
    if (gbi_->map[F3DEX2_G_BRANCH_Z] == &Scope::observe) {
        gbi_->map[F3DEX2_G_BRANCH_Z] = original_;
        status_ = "captured";
    }
    else {
        // A changed map must not be overwritten by diagnostics.
        status_ = "branch_handler_changed_during_task";
    }
    owner_ = nullptr;
    installed_ = false;
}

void Scope::observe(RT64::State* state, RT64::DisplayList** dl) {
    Scope* scope = owner_;
    if (scope == nullptr) {
        // The map belongs to one Interpreter; another thread must still see
        // the original behavior if it reaches this diagnostic slot.
        RT64::GBI_F3DEX::branchZ(state, dl);
        return;
    }

    ++scope->total_calls_;
    const RT64::DisplayList* command = *dl;
    Sample sample;
    sample.threshold_raw = command->w1;
    sample.vertex_index = command->p0(1, 11);
    sample.target_segmented = state->microcode.half1;
    sample.target_physical = state->rsp->fromSegmentedMasked(sample.target_segmented);

    const auto command_address = reinterpret_cast<std::uintptr_t>(command);
    const auto rdram_address = reinterpret_cast<std::uintptr_t>(state->RDRAM);
    if (command_address >= rdram_address && command_address - rdram_address < 8u * 1024u * 1024u) {
        sample.command_offset = static_cast<std::uint32_t>(command_address - rdram_address);
    }

    const int cursor = state->ext.workloadQueue->writeCursor;
    if (cursor >= 0 && cursor < static_cast<int>(state->ext.workloadQueue->workloads.size()) &&
        sample.vertex_index < state->rsp->indices.size()) {
        const auto& data = state->ext.workloadQueue->workloads[cursor].drawData;
        const std::uint32_t global_index = state->rsp->indices[sample.vertex_index];
        if (global_index < data.posScreen.size() && global_index < data.posTransformed.size()) {
            // Match RSP::branchZ's DepthRange=1024 computation exactly.
            sample.screen_z = data.posScreen[global_index][2] * 1024.0f;
            sample.clip_w = data.posTransformed[global_index][3];
            sample.vertex_available = true;
            sample.expected_branch_condition =
                state->ext.enhancementConfig->f3dex.forceBranch ||
                state->rsp->extended.forceBranch ||
                sample.screen_z < sample.threshold_raw / 65536.0f;
        }
    }

    scope->original_(state, dl);
    sample.pointer_changed = *dl != command;
    if (scope->recorded_ < max_samples) {
        scope->samples_[scope->recorded_++] = sample;
    }
}

nlohmann::json Scope::snapshot_json() const {
    nlohmann::json samples = nlohmann::json::array();
    for (std::size_t i = 0; i < recorded_; ++i) {
        const Sample& s = samples_[i];
        samples.push_back({
            {"command_offset", s.command_offset},
            {"target_segmented", s.target_segmented},
            {"target_physical", s.target_physical},
            {"threshold_raw", s.threshold_raw},
            {"vertex_index", s.vertex_index},
            {"screen_z", s.vertex_available ? nlohmann::json(s.screen_z) : nlohmann::json(nullptr)},
            {"clip_w", s.vertex_available ? nlohmann::json(s.clip_w) : nlohmann::json(nullptr)},
            {"vertex_available", s.vertex_available},
            {"expected_branch_condition", s.vertex_available ? nlohmann::json(s.expected_branch_condition) : nlohmann::json(nullptr)},
            {"pointer_changed", s.pointer_changed},
        });
    }
    return {
        {"status", status_},
        {"scope", "starting_f3dex2_gbi_map_only; later microcode_switches_not_instrumented"},
        {"sample_cap", max_samples},
        {"total_calls", total_calls_},
        {"recorded_calls", recorded_},
        {"dropped_calls", total_calls_ - recorded_},
        {"samples", std::move(samples)},
    };
}

} // namespace tooie::graphics_branch_capture
