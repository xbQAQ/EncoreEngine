#include "render/ShadowViews.h"
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace render {
namespace {
constexpr unsigned MaxLights = 16;
constexpr unsigned MaxViews = 96;
Vec3 Xyz(const Float4& value) { return {value.x, value.y, value.z}; }
Float4 Lane(Vec3 vector, float scalar) { return {vector.x, vector.y, vector.z, scalar}; }

float CheckedFloat(double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::invalid_argument("Shadow projection exceeds finite float coordinates");
    return static_cast<float>(value);
}
double DotWide(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }
Vec3 Difference(Vec3 a, Vec3 b) {
    return {CheckedFloat(double(a.x) - b.x), CheckedFloat(double(a.y) - b.y), CheckedFloat(double(a.z) - b.z)};
}

std::array<Vec3, 8> Corners(Aabb bounds) {
    if (!IsFinite(bounds.minimum) || !IsFinite(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x || bounds.minimum.y > bounds.maximum.y || bounds.minimum.z > bounds.maximum.z)
        throw std::invalid_argument("Shadow fitting requires finite nonempty scene bounds");
    std::array<Vec3, 8> result{};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = {i & 1 ? bounds.maximum.x : bounds.minimum.x,
            i & 2 ? bounds.maximum.y : bounds.minimum.y, i & 4 ? bounds.maximum.z : bounds.minimum.z};
    return result;
}

GpuShadowView View(Vec3 origin, Vec3 forward, Vec3 preferredRight, float nearPlane, float farPlane, float extent, bool perspective) {
    forward = Normalize(forward);
    if (Length(forward) < 0.5f) throw std::invalid_argument("Shadow light direction must be nonzero");
    // Project the emitter tangent onto the view plane; punctual lights use a stable fallback axis.
    Vec3 right = Normalize(preferredRight - forward * Dot(preferredRight, forward));
    if (Length(right) < 0.5f)
        right = Normalize(Cross(std::abs(forward.y) < 0.95f ? Vec3{0, 1, 0} : Vec3{0, 0, 1}, forward));
    const Vec3 up = Cross(forward, right);
    if (!IsFinite(origin) || !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        !std::isfinite(extent) || nearPlane <= 0 || farPlane <= nearPlane || extent <= 0)
        throw std::invalid_argument("Shadow projection has invalid near/far/extent");
    return {Lane(origin, nearPlane), Lane(right, farPlane), Lane(up, extent), Lane(forward, perspective ? 1.0f : 0.0f), {}};
}

GpuShadowView Directional(const GpuLight& light, const std::array<Vec3, 8>& corners, Vec3 center) {
    auto view = View(center, Xyz(light.directionRange), Xyz(light.tangentWidth), 0.01f, 1, 1, false);
    const Vec3 right = Xyz(view.rightFar), up = Xyz(view.upExtent), forward = Xyz(view.forwardProjection);
    double xMin = std::numeric_limits<double>::infinity(), yMin = xMin, zMin = xMin;
    double xMax = -xMin, yMax = -xMin, zMax = -xMin;
    for (const auto& point : corners) {
        const Vec3 delta = Difference(point, center);
        const double x = DotWide(delta, right), y = DotWide(delta, up), z = DotWide(delta, forward);
        xMin = std::min(xMin, x); xMax = std::max(xMax, x);
        yMin = std::min(yMin, y); yMax = std::max(yMax, y);
        zMin = std::min(zMin, z); zMax = std::max(zMax, z);
    }
    // Padding covers floating-point fitting error and keeps boundary receivers away from tile edges.
    const double margin = std::max(0.01, std::max({xMax - xMin, yMax - yMin, zMax - zMin}) * 0.01);
    const Vec3 origin = center + right * CheckedFloat((xMin + xMax) * 0.5) + up * CheckedFloat((yMin + yMax) * 0.5)
        + forward * CheckedFloat(zMin - margin - view.originNear.w);
    view.originNear = Lane(origin, view.originNear.w);
    view.rightFar.w = CheckedFloat(zMax - zMin + margin * 2 + view.originNear.w);
    view.upExtent.w = CheckedFloat(std::max(xMax - xMin, yMax - yMin) * 0.5 + margin);
    if (!IsFinite(origin)) throw std::invalid_argument("Directional shadow origin overflow");
    return view;
}
}

std::vector<GpuShadowView> BuildShadowViews(std::vector<GpuLight>& lights, Aabb bounds, unsigned resolution, unsigned tiles) {
    if (lights.size() > MaxLights || resolution == 0 || tiles == 0 ||
        std::uint64_t(resolution) * tiles > 16384)
        throw std::invalid_argument("Shadow atlas/light capacity is invalid");
    const auto corners = Corners(bounds);
    const Vec3 center{bounds.minimum.x * 0.5f + bounds.maximum.x * 0.5f,
        bounds.minimum.y * 0.5f + bounds.maximum.y * 0.5f, bounds.minimum.z * 0.5f + bounds.maximum.z * 0.5f};
    std::vector<GpuShadowView> views;
    std::vector<Uint4> assignments;
    assignments.reserve(lights.size());
    for (unsigned index = 0; index < lights.size(); ++index) {
        const auto& light = lights[index];
        const float typeValue = light.positionType.w;
        if (!IsFinite(Xyz(light.positionType)) || !IsFinite(Xyz(light.directionRange)) ||
            !IsFinite(Xyz(light.tangentWidth)) || !std::isfinite(typeValue) || typeValue < 0 || typeValue > 4 ||
            std::floor(typeValue) != typeValue || !std::isfinite(light.directionRange.w) || light.directionRange.w < 0 ||
            !std::isfinite(light.parameters.x) || light.parameters.x < 0)
            throw std::invalid_argument("Shadow light has invalid position/type/direction/range");
        const unsigned type = static_cast<unsigned>(typeValue);
        if ((type == 2 || type == 3) && (!std::isfinite(light.tangentWidth.w) || light.tangentWidth.w <= 0 ||
            (type == 2 && (!std::isfinite(light.bitangentHeight.w) || light.bitangentHeight.w <= 0))))
            throw std::invalid_argument("Area shadow light requires positive finite emitter dimensions");
        const unsigned count = type == 1 ? 6u : 1u;
        if (views.size() + count > MaxViews || views.size() + count > std::uint64_t(tiles) * tiles)
            throw std::invalid_argument("Shadow atlas exceeds its tile or 96-view capacity");
        const auto first = static_cast<unsigned>(views.size());
        Uint4 assignment = light.shadow;
        assignment.x = first;
        assignment.y = count;
        assignments.push_back(assignment);
        if (type == 0) views.push_back(Directional(light, corners, center));
        else {
            // Bound unbounded lights by the scene, while finite range remains the shading cutoff.
            const Vec3 origin = Xyz(light.positionType);
            double sceneDistance = 0.01;
            for (const auto& point : corners) {
                const Vec3 delta = Difference(point, origin);
                sceneDistance = std::max(sceneDistance, std::sqrt(DotWide(delta, delta)));
            }
            const bool finitePunctual = (type == 1 || type == 4) && light.directionRange.w > 0;
            const float farPlane = finitePunctual ? light.directionRange.w : CheckedFloat(sceneDistance * 1.01 + 0.01);
            const float nearPlane = std::min(0.01f, farPlane * 0.001f);
            if (type == 1) {
                constexpr Vec3 directions[]{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (const Vec3 direction : directions) views.push_back(View(origin, direction, {}, nearPlane, farPlane, 1, true));
            } else {
                float extent = 1;
                if (type == 4) {
                    const float cosine = light.parameters.y;
                    if (!std::isfinite(cosine) || cosine < -1e-6f || cosine >= 1)
                        throw std::invalid_argument("Spot shadow outer half angle must be greater than zero and at most ninety degrees");
                    // glTF permits a ninety-degree half cone; finite raster projections cap that view at 178 degrees.
                    const float projectedCosine = std::max(cosine, 0.017452406f);
                    extent = std::sqrt(1 - projectedCosine * projectedCosine) / projectedCosine;
                } else {
                    // A single area-light view approximates its center; cap its field at 178 degrees.
                    const auto basis = View(origin, Xyz(light.directionRange), Xyz(light.tangentWidth), nearPlane, farPlane, 1, true);
                    for (const auto& point : corners) {
                        const auto delta = Difference(point, origin);
                        const double z = DotWide(delta, Xyz(basis.forwardProjection));
                        if (z <= nearPlane) continue;
                        extent = std::max(extent, CheckedFloat(std::max(std::abs(DotWide(delta, Xyz(basis.rightFar))),
                            std::abs(DotWide(delta, Xyz(basis.upExtent)))) / z * 1.01));
                    }
                    extent = std::min(extent, 57.28996f);
                }
                views.push_back(View(origin, Xyz(light.directionRange), Xyz(light.tangentWidth), nearPlane, farPlane, extent, true));
            }
        }
        for (unsigned viewIndex = first; viewIndex < views.size(); ++viewIndex)
            views[viewIndex].atlas = {viewIndex % tiles * resolution, viewIndex / tiles * resolution, resolution, index};
    }
    for (std::size_t index = 0; index < lights.size(); ++index) lights[index].shadow = assignments[index];
    return views;
}

ShadowProjection ProjectShadowPoint(const GpuShadowView& view, Vec3 position) {
    const Vec3 delta = position - Xyz(view.originNear);
    const float z = Dot(delta, Xyz(view.forwardProjection));
    const float denominator = view.upExtent.w * (view.forwardProjection.w > 0.5f ? z : 1.0f);
    if (!IsFinite(delta) || !std::isfinite(denominator) || denominator <= 0 || view.rightFar.w <= view.originNear.w) return {};
    const float u = 0.5f + Dot(delta, Xyz(view.rightFar)) / (2 * denominator);
    const float v = 0.5f - Dot(delta, Xyz(view.upExtent)) / (2 * denominator);
    const float depth = (z - view.originNear.w) / (view.rightFar.w - view.originNear.w);
    return {u, v, depth, std::isfinite(u) && std::isfinite(v) && std::isfinite(depth) &&
        u >= 0 && u <= 1 && v >= 0 && v <= 1 && depth >= 0 && depth <= 1};
}
}
