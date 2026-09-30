#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace tooie::rt64_match {

struct ViewTranslation {
    double x;
    double y;
    double z;
};

// Source-relative camera fallback for an auxiliary AUTO view whose ordinal
// history was rejected. RT64 uses row-vector transforms: aux = T(source) * main.
template <typename Matrix>
bool relative_auxiliary_translation(const Matrix& auxiliary, const Matrix& main,
    ViewTranslation& offset) noexcept {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (!std::isfinite(static_cast<double>(auxiliary[row][col])) ||
                !std::isfinite(static_cast<double>(main[row][col]))) return false;
        }
    }
    for (int row = 0; row < 3; ++row) {
        if (std::abs(static_cast<double>(auxiliary[row][3])) > 0.0001 ||
            std::abs(static_cast<double>(main[row][3])) > 0.0001) return false;
        for (int col = 0; col < 4; ++col) {
            const double a = static_cast<double>(auxiliary[row][col]);
            const double b = static_cast<double>(main[row][col]);
            if (std::abs(a - b) > 0.0001 * std::max({1.0, std::abs(a), std::abs(b)}))
                return false;
        }
    }
    if (std::abs(static_cast<double>(auxiliary[3][3]) - 1.0) > 0.0001 ||
        std::abs(static_cast<double>(main[3][3]) - 1.0) > 0.0001) return false;

    // Solve offset * main[0..2][0..2] = aux[3][0..2] - main[3][0..2].
    double rows[3][4]{};
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row)
            rows[col][row] = static_cast<double>(main[row][col]);
        rows[col][3] = static_cast<double>(auxiliary[3][col]) -
            static_cast<double>(main[3][col]);
    }
    for (int pivot = 0; pivot < 3; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < 3; ++row)
            if (std::abs(rows[row][pivot]) > std::abs(rows[best][pivot])) best = row;
        if (std::abs(rows[best][pivot]) < 0.000001) return false;
        if (best != pivot)
            for (int col = pivot; col < 4; ++col)
                std::swap(rows[pivot][col], rows[best][col]);
        const double divisor = rows[pivot][pivot];
        for (int col = pivot; col < 4; ++col) rows[pivot][col] /= divisor;
        for (int row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const double factor = rows[row][pivot];
            for (int col = pivot; col < 4; ++col)
                rows[row][col] -= factor * rows[pivot][col];
        }
    }
    const ViewTranslation candidate{rows[0][3], rows[1][3], rows[2][3]};
    if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
        !std::isfinite(candidate.z)) return false;
    const double values[3] = {candidate.x, candidate.y, candidate.z};
    for (int col = 0; col < 4; ++col) {
        double reconstructed = static_cast<double>(main[3][col]);
        for (int row = 0; row < 3; ++row)
            reconstructed += values[row] * static_cast<double>(main[row][col]);
        const double observed = static_cast<double>(auxiliary[3][col]);
        if (std::abs(reconstructed - observed) >
            0.01 + 0.00001 * std::max(std::abs(reconstructed), std::abs(observed)))
            return false;
    }
    offset = candidate;
    return true;
}

template <typename Matrix>
bool same_projection_lens(const Matrix& auxiliary, const Matrix& main) noexcept {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const double a = static_cast<double>(auxiliary[row][col]);
            const double b = static_cast<double>(main[row][col]);
            if (!std::isfinite(a) || !std::isfinite(b) ||
                std::abs(a - b) > 0.0005 * std::max({1.0, std::abs(a), std::abs(b)}))
                return false;
        }
    }
    return true;
}

template <typename Matrix>
bool compose_relative_auxiliary_view(const Matrix& main, ViewTranslation offset,
    Matrix& auxiliary) noexcept {
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        !std::isfinite(offset.z)) return false;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (!std::isfinite(static_cast<double>(main[row][col]))) return false;
    Matrix candidate = main;
    const double values[3] = {offset.x, offset.y, offset.z};
    for (int col = 0; col < 4; ++col) {
        double value = static_cast<double>(main[3][col]);
        for (int row = 0; row < 3; ++row)
            value += values[row] * static_cast<double>(main[row][col]);
        if (!std::isfinite(value)) return false;
        candidate[3][col] = value;
        if (!std::isfinite(static_cast<double>(candidate[3][col]))) return false;
    }
    auxiliary = candidate;
    return true;
}

template <typename Matrix>
bool compose_relative_camera_delta(const Matrix& auxiliaryCurrent,
    const Matrix& mainCurrent, const Matrix& mainInterpolated,
    ViewTranslation offset, Matrix& auxiliaryInterpolated) noexcept {
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y) ||
        !std::isfinite(offset.z)) return false;
    Matrix candidate = auxiliaryCurrent;
    const double values[3] = {offset.x, offset.y, offset.z};
    for (int col = 0; col < 4; ++col) {
        double bottomDelta = static_cast<double>(mainInterpolated[3][col]) -
            static_cast<double>(mainCurrent[3][col]);
        for (int row = 0; row < 3; ++row) {
            const double delta = static_cast<double>(mainInterpolated[row][col]) -
                static_cast<double>(mainCurrent[row][col]);
            const double value = static_cast<double>(auxiliaryCurrent[row][col]) + delta;
            if (!std::isfinite(value)) return false;
            candidate[row][col] = value;
            if (!std::isfinite(static_cast<double>(candidate[row][col]))) return false;
            bottomDelta += values[row] * delta;
        }
        const double value = static_cast<double>(auxiliaryCurrent[3][col]) + bottomDelta;
        if (!std::isfinite(value)) return false;
        candidate[3][col] = value;
        if (!std::isfinite(static_cast<double>(candidate[3][col]))) return false;
    }
    auxiliaryInterpolated = candidate;
    return true;
}

inline double view_translation_distance_squared(ViewTranslation a, ViewTranslation b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

inline bool allow_auxiliary_projection_history(bool automatic, bool auxiliary,
    ViewTranslation current, ViewTranslation previous,
    ViewTranslation mainCurrent, ViewTranslation mainPrevious) noexcept {
    if (!automatic || !auxiliary) return true;
    const double auxiliaryTravel = view_translation_distance_squared(current, previous);
    const double mainTravel = view_translation_distance_squared(mainCurrent, mainPrevious);
    if (!std::isfinite(auxiliaryTravel) || !std::isfinite(mainTravel)) return false;
    // A large jump beyond the main camera's motion suggests that an
    // auxiliary projection ordinal was reused by another source. This is a
    // discontinuity safeguard, not a persistent source identity test.
    return auxiliaryTravel <= 256.0 * 256.0 || auxiliaryTravel <= 16.0 * mainTravel;
}

template <typename Matrix>
ViewTranslation view_translation(const Matrix& matrix) noexcept {
    return {static_cast<double>(matrix[3][0]),
        static_cast<double>(matrix[3][1]),
        static_cast<double>(matrix[3][2])};
}

template <typename Workload, typename Matrix>
bool allow_auxiliary_projection_pair(const Workload& current, const Workload& previous,
    const Matrix& currentView, const Matrix& previousView) noexcept {
    struct DominantView {
        ViewTranslation position{};
        bool found = false;
        uint32_t calls = 0;
        bool malformed = false;
    };
    auto dominant = [](const Workload& workload) noexcept {
        DominantView result;
        if (workload.fbPairCount > workload.fbPairs.size()) {
            result.malformed = true;
            return result;
        }
        for (uint32_t pairIndex = 0; pairIndex < workload.fbPairCount; ++pairIndex) {
            const auto& pair = workload.fbPairs[pairIndex];
            if (pair.projectionCount > pair.projections.size()) {
                result.malformed = true;
                return result;
            }
            for (uint32_t projectionIndex = 0; projectionIndex < pair.projectionCount; ++projectionIndex) {
                const auto& projection = pair.projections[projectionIndex];
                if (projection.type != decltype(projection.type)::Perspective) continue;
                if (projection.transformsIndex >= workload.drawData.viewTransforms.size()) {
                    result.malformed = true;
                    return result;
                }
                if (!result.found || projection.gameCallCount > result.calls) {
                    const Matrix& view = workload.drawData.viewTransforms[projection.transformsIndex];
                    result.position = view_translation(view);
                    result.found = true;
                    result.calls = projection.gameCallCount;
                }
            }
        }
        return result;
    };

    const DominantView mainCurrent = dominant(current);
    const DominantView mainPrevious = dominant(previous);
    if (mainCurrent.malformed || mainPrevious.malformed) return false;
    if (!mainCurrent.found || !mainPrevious.found) return true;

    const ViewTranslation currentPosition = view_translation(currentView);
    const ViewTranslation dominantPosition = mainCurrent.position;
    const double auxiliaryOffset = view_translation_distance_squared(currentPosition, dominantPosition);
    if (!std::isfinite(auxiliaryOffset)) return false;
    if (auxiliaryOffset <= 1.0) return true;
    return allow_auxiliary_projection_history(true, true,
        currentPosition, view_translation(previousView),
        dominantPosition, mainPrevious.position);
}

} // namespace tooie::rt64_match
