#include "rt64_aux_projection_policy.hpp"

#include <cassert>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using tooie::rt64_match::ViewTranslation;
using tooie::rt64_match::allow_auxiliary_projection_history;
using tooie::rt64_match::allow_auxiliary_projection_pair;
using tooie::rt64_match::relative_auxiliary_translation;
using tooie::rt64_match::same_projection_lens;
using tooie::rt64_match::compose_relative_auxiliary_view;
using tooie::rt64_match::compose_relative_camera_delta;

using Matrix = std::array<std::array<double, 4>, 4>;
enum class ProjectionType { Orthographic, Perspective };
struct Projection {
    ProjectionType type;
    uint32_t transformsIndex;
    uint32_t gameCallCount;
};
struct FramebufferPair {
    std::vector<Projection> projections;
    uint32_t projectionCount;
};
struct Workload {
    std::vector<FramebufferPair> fbPairs;
    uint32_t fbPairCount;
    struct { std::vector<Matrix> viewTransforms; } drawData;
};

static Matrix matrix(ViewTranslation translation) {
    Matrix result{};
    for (int i = 0; i < 4; ++i) result[i][i] = 1.0;
    result[3][0] = translation.x;
    result[3][1] = translation.y;
    result[3][2] = translation.z;
    return result;
}

static Matrix mul(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                result[r][c] += a[r][k] * b[k][c];
    return result;
}

static bool close(const Matrix& a, const Matrix& b, double tolerance = 0.0001) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::abs(a[r][c] - b[r][c]) > tolerance) return false;
    return true;
}

int main() {
    // A feather view is a pure actor translation before the main camera view.
    // A reused previous projection must not force its camera to the guest tick.
    Matrix mainView = matrix({10.0, 20.0, 30.0});
    mainView[0][0] = 0.0; mainView[0][2] = -1.0;
    mainView[2][0] = 1.0; mainView[2][2] = 0.0;
    const ViewTranslation actor{-1104.0, -50.0, 2012.0};
    const Matrix auxiliaryView = mul(matrix(actor), mainView);
    Matrix lens = matrix({0.0, 0.0, 0.0});
    lens[0][0] = 1.3; lens[1][1] = 1.1; lens[2][2] = -1.0;
    lens[2][3] = -1.0; lens[3][2] = -0.2; lens[3][3] = 0.0;
    const Matrix auxiliaryCombined = mul(auxiliaryView, lens);
    const Matrix mainCombined = mul(mainView, lens);
    Matrix mainInterpolatedView = mainView;
    mainInterpolatedView[3][0] += 4.5;
    mainInterpolatedView[3][1] -= 2.25;
    Matrix mainInterpolatedLens = lens;
    mainInterpolatedLens[0][0] += 0.01;
    const Matrix mainInterpolatedCombined = mul(mainInterpolatedView, mainInterpolatedLens);
    ViewTranslation extracted{};
    assert(relative_auxiliary_translation(auxiliaryView, mainView, extracted));
    assert(std::abs(extracted.x - actor.x) < 0.001);
    assert(std::abs(extracted.y - actor.y) < 0.001);
    assert(std::abs(extracted.z - actor.z) < 0.001);
    assert(same_projection_lens(lens, lens));
    Matrix interpolatedAuxView{};
    assert(compose_relative_auxiliary_view(mainInterpolatedView, extracted, interpolatedAuxView));
    assert(close(interpolatedAuxView, mul(matrix(actor), mainInterpolatedView)));
    Matrix interpolatedAuxCombined{};
    assert(compose_relative_camera_delta(auxiliaryCombined, mainCombined,
        mainInterpolatedCombined, extracted, interpolatedAuxCombined));
    assert(close(interpolatedAuxCombined, mul(matrix(actor), mainInterpolatedCombined), 0.0002));
    Matrix endpoint{};
    assert(compose_relative_camera_delta(auxiliaryCombined, mainCombined,
        mainCombined, extracted, endpoint));
    assert(close(endpoint, auxiliaryCombined));

    // A different lens or rotated auxiliary pass is not eligible for this
    // camera-relative fallback. Reject malformed and nonfinite matrices too.
    Matrix differentLens = lens; differentLens[0][0] += 0.2;
    assert(!same_projection_lens(lens, differentLens));
    Matrix rotatedAux = auxiliaryView; rotatedAux[0][0] += 0.1;
    assert(!relative_auxiliary_translation(rotatedAux, mainView, extracted));
    Matrix nonAffineMain = mainView; nonAffineMain[0][3] = 0.1;
    Matrix nonAffineAux = mul(matrix({0.0, 10.0, 0.0}), nonAffineMain);
    assert(!relative_auxiliary_translation(nonAffineAux, nonAffineMain, extracted));
    Matrix singularMain = mainView; singularMain[1] = singularMain[0];
    assert(!relative_auxiliary_translation(auxiliaryView, singularMain, extracted));
    Matrix badLens = lens; badLens[0][0] = std::numeric_limits<double>::quiet_NaN();
    assert(!same_projection_lens(lens, badLens));
    using FloatMatrix = std::array<std::array<float, 4>, 4>;
    FloatMatrix floatMain{};
    for (int i = 0; i < 4; ++i) floatMain[i][i] = 1.0f;
    floatMain[0][0] = 1.0e38f;
    FloatMatrix floatOutput{};
    assert(!compose_relative_auxiliary_view(floatMain, {10.0, 0.0, 0.0}, floatOutput));
    assert(!compose_relative_camera_delta(floatMain, FloatMatrix{}, floatMain,
        {10.0, 0.0, 0.0}, floatOutput));

    const ViewTranslation mainCurrent{101.45, 1436.77, 1817.74};
    const ViewTranslation mainPrevious{29.84, 1423.91, 1822.58};

    // The same projection ordinal can belong to a different feather nest in
    // the preceding workload. It must not supply camera history to that nest.
    assert(!allow_auxiliary_projection_history(true, true,
        {1350.56, 1780.38, 2234.16}, {-1197.40, 918.04, 1382.50},
        mainCurrent, mainPrevious));

    // An ordinary auxiliary camera moving with the main view keeps smoothing.
    assert(allow_auxiliary_projection_history(true, true,
        {-388.66, 1367.53, 1737.07}, {-379.99, 1378.92, 1737.27},
        {946.65, 1270.11, 1574.15}, {954.50, 1276.04, 1570.09}));

    // A fast, coherent camera move is not a change of auxiliary owner.
    assert(allow_auxiliary_projection_history(true, true,
        {600.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        {200.0, 0.0, 0.0}, {0.0, 0.0, 0.0}));

    // This guard never changes the accepted main or explicitly identified pass.
    assert(allow_auxiliary_projection_history(true, false,
        {3000.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        mainCurrent, mainPrevious));
    assert(allow_auxiliary_projection_history(false, true,
        {3000.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        mainCurrent, mainPrevious));

    Workload current{};
    current.drawData.viewTransforms = {
        matrix(mainCurrent), matrix({1350.56, 1780.38, 2234.16}),
        matrix({1350.56, 1780.38, 2234.16})};
    current.fbPairs = {
        {{{ProjectionType::Orthographic, 0, 1},
           {ProjectionType::Perspective, 0, 461}}, 2},
        {{{ProjectionType::Perspective, 1, 27},
          {ProjectionType::Perspective, 2, 9999}}, 1},
        {{{ProjectionType::Perspective, 2, 9999}}, 1}};
    current.fbPairCount = 2;
    Workload previous{};
    previous.drawData.viewTransforms = {
        matrix({-1197.40, 918.04, 1382.50}), matrix(mainPrevious)};
    previous.fbPairs = {
        {{{ProjectionType::Perspective, 0, 27},
          {ProjectionType::Perspective, 1, 461}}, 2}};
    previous.fbPairCount = 1;

    // Active counts and draw-call dominance identify the main view without
    // relying on framebuffer/projection ordinal or retained vector capacity.
    assert(!allow_auxiliary_projection_pair(current, previous,
        current.drawData.viewTransforms[1], previous.drawData.viewTransforms[0]));
    assert(allow_auxiliary_projection_pair(current, previous,
        current.drawData.viewTransforms[0], previous.drawData.viewTransforms[1]));
    current.drawData.viewTransforms[1] = matrix({55.0, 1450.0, 1830.0});
    previous.drawData.viewTransforms[0] = matrix({50.0, 1430.0, 1820.0});
    assert(allow_auxiliary_projection_pair(current, previous,
        current.drawData.viewTransforms[1], previous.drawData.viewTransforms[0]));

    // Missing main history retains the existing mapping; corrupt active
    // indices or nonfinite view data must not be interpolated.
    Workload noPerspective = current;
    noPerspective.fbPairs[0].projections[1].type = ProjectionType::Orthographic;
    noPerspective.fbPairs[1].projections[0].type = ProjectionType::Orthographic;
    assert(allow_auxiliary_projection_pair(noPerspective, previous,
        current.drawData.viewTransforms[1], previous.drawData.viewTransforms[0]));
    Workload malformed = current;
    malformed.fbPairs[0].projections[1].transformsIndex = 99;
    assert(!allow_auxiliary_projection_pair(malformed, previous,
        current.drawData.viewTransforms[1], previous.drawData.viewTransforms[0]));
    malformed = current;
    malformed.fbPairs[0].projectionCount = 99;
    assert(!allow_auxiliary_projection_pair(malformed, previous,
        current.drawData.viewTransforms[1], previous.drawData.viewTransforms[0]));
    Matrix nonfinite = current.drawData.viewTransforms[1];
    nonfinite[3][0] = std::numeric_limits<double>::quiet_NaN();
    assert(!allow_auxiliary_projection_pair(current, previous,
        nonfinite, previous.drawData.viewTransforms[0]));
}
