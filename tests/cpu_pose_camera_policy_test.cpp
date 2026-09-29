#include "model_interpolation.hpp"
#include "model_interpolation_rt64.hpp"
#include "hle/rt64_workload.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using tooie::model_interpolation::MatrixRange;
    using tooie::model_interpolation::TaskScope;
    constexpr std::uint32_t cpuMatrix = 0x00100000;
    constexpr std::uint32_t ordinaryMatrix = 0x00100040;
    constexpr std::uint32_t invalidMatrix = 0x00800000;

    // The captured Wooded Hollow draw is a CPU-skinned world pose with a
    // stationary world root. Its camera must stay in the scene's smooth group.
    std::vector<MatrixRange> gameplay{{cpuMatrix, ordinaryMatrix, true}};
    {
        TaskScope task(gameplay);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        data.viewProjTransformGroups.push_back(0);
        const auto world = RT64::tooieOriginalPoseGroup(data, 0, cpuMatrix);
        const auto camera = RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix);
        assert(world != 0);
        assert(data.transformGroups[world].matrixId == G_EX_ID_IGNORE);
        assert(camera == 0);
        assert(!RT64::tooieOriginalPoseProjectionChanged(data, 0, 0, cpuMatrix));
        // A following ordinary draw must reuse the unmodified shared camera.
        assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, ordinaryMatrix) == 0);
    }

    // Intro/cutscene CPU poses retain their previously accepted paired guard.
    std::vector<MatrixRange> intro{{cpuMatrix, ordinaryMatrix, false}};
    {
        TaskScope task(intro);
        RT64::DrawData data;
        data.transformGroups.emplace_back();
        data.viewProjTransformGroups.push_back(0);
        const auto camera = RT64::tooieOriginalPoseProjectionGroup(data, 0, cpuMatrix);
        assert(camera != 0);
        assert(data.transformGroups[camera].matrixId == G_EX_ID_IGNORE);
        data.viewProjTransformGroups.push_back(camera);
        assert(!RT64::tooieOriginalPoseProjectionChanged(data, 1, 0, cpuMatrix));
        assert(RT64::tooieOriginalPoseProjectionChanged(data, 1, 0, ordinaryMatrix));
        assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, ordinaryMatrix) == 0);
    }

    // Missing/invalid source matrices never opt into a different policy.
    RT64::DrawData data;
    data.transformGroups.emplace_back();
    assert(RT64::tooieOriginalPoseProjectionGroup(data, 0, invalidMatrix) == 0);
}
