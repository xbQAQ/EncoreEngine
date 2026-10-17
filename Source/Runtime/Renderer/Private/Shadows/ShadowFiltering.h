#pragma once
#include <algorithm>
#include <cmath>

namespace render::shadow {
inline float VarianceVisibility(float mean, float secondMoment, float receiver, float minimumVariance, float bleedingReduction) {
    // The one-tailed bound is fully lit before the mean; inconsistent moments still need a variance floor.
    if (!std::isfinite(mean) || !std::isfinite(secondMoment) || !std::isfinite(receiver)) return 1;
    if (receiver <= mean) return 1;
    const float variance = std::max({minimumVariance, secondMoment - mean * mean, 1e-8f});
    const float difference = receiver - mean;
    const float bound = variance / (variance + difference * difference);
    const float reduction = std::clamp(bleedingReduction, 0.0f, 0.999f);
    return std::clamp((bound - reduction) / (1 - reduction), 0.0f, 1.0f);
}
inline float PcssPenumbra(float sourceRadius, float receiverDistance, float blockerDistance, bool directional) {
    // Perspective emitter radius has units of meters; a directional radius is an angle in radians.
    if (!std::isfinite(sourceRadius) || !std::isfinite(receiverDistance) || !std::isfinite(blockerDistance) ||
        sourceRadius <= 0 || receiverDistance <= blockerDistance || blockerDistance <= 0) return 0;
    const float separation = receiverDistance - blockerDistance;
    return directional ? std::tan(std::min(sourceRadius, 1.4f)) * separation : sourceRadius * separation / blockerDistance;
}
}
