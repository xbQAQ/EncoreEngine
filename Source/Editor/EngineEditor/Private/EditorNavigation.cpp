#include "editor/EditorNavigation.h"
#include "editor/EditorGizmo.h"
#include "gameplay/World.h"
#include "render/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace render::editor {
namespace {
constexpr double radians = 3.14159265358979323846 / 180.0;
constexpr float maximumDistance = 1e6f;
constexpr float lookSensitivity = 0.18f;

bool ValidCamera(const gameplay::Transform& transform, const gameplay::Camera& camera) {
    // Navigation must never turn malformed inspector data into persistent controller state.
    return IsFinite(transform.position) && IsFinite(transform.rotationDegrees) && IsFinite(transform.scale) &&
        std::isfinite(camera.verticalFovDegrees) && camera.verticalFovDegrees > 0 && camera.verticalFovDegrees < 180 &&
        std::isfinite(camera.nearPlane) && camera.nearPlane > 0 && camera.nearPlane < maximumDistance * 0.5f &&
        std::isfinite(camera.farPlane) && camera.farPlane > camera.nearPlane;
}

bool ValidInput(const NavigationInput& input) {
    return std::isfinite(input.mouseDeltaX) && std::isfinite(input.mouseDeltaY) &&
        std::isfinite(input.forward) && std::isfinite(input.right) && std::isfinite(input.up) &&
        std::isfinite(input.deltaSeconds) && input.deltaSeconds >= 0 && std::isfinite(input.wheel) &&
        std::isfinite(input.viewportHeight) && input.viewportHeight > 0;
}

bool Different(Vec3 a, Vec3 b) { return a.x != b.x || a.y != b.y || a.z != b.z; }

float Distance(Vec3 offset) {
    // Double precision avoids overflow when a finite imported world uses very large coordinates.
    return static_cast<float>(std::min(std::hypot(double(offset.x), double(offset.y), double(offset.z)), double(maximumDistance)));
}

bool ExpandTransformed(Aabb& bounds, Vec3 point, const gameplay::Transform& transform, const CameraBasis& basis) {
    Vec3 transformed;
    // Match Rz*Ry*Rx*S from extraction, retaining negative scale without moving the source origin.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double value = double(transform.position[axis]) + double(basis.right[axis]) * point.x * transform.scale.x +
            double(basis.up[axis]) * point.y * transform.scale.y + double(basis.forward[axis]) * point.z * transform.scale.z;
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
        if (axis == 0) transformed.x = static_cast<float>(value);
        else if (axis == 1) transformed.y = static_cast<float>(value);
        else transformed.z = static_cast<float>(value);
    }
    bounds.Expand(transformed);
    return true;
}

bool TargetBounds(const Scene& source, const gameplay::World& world, gameplay::Entity entity, Aabb& bounds) {
    const auto& registry = world.Registry();
    if (!registry.valid(entity)) return false;
    const auto* selectedMesh = registry.try_get<gameplay::MeshRenderer>(entity);
    if (selectedMesh && !selectedMesh->visible) return false;
    entity = GizmoTarget(world, entity);
    if (entity == entt::null) return false;
    const auto& transform = registry.get<gameplay::Transform>(entity);
    if (!IsFinite(transform.position) || !IsFinite(transform.rotationDegrees) || !IsFinite(transform.scale)) return false;
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);

    // A light-owned source mesh resolves to its analytic owner, just as selection and manipulation do.
    if (const auto* light = registry.try_get<gameplay::Light>(entity)) {
        if (!light->enabled) return false;
        if (light->type == gameplay::LightType::Rectangle || light->type == gameplay::LightType::Disk) {
            if (!std::isfinite(light->width) || !std::isfinite(light->height) || light->width <= 0 || light->height <= 0 ||
                !IsFinite(light->tangent) || !IsFinite(light->bitangent)) return false;
            const auto rotate = [&](Vec3 value) { return basis.right * value.x + basis.up * value.y + basis.forward * value.z; };
            const auto tangent = Normalize(rotate(light->tangent)), bitangent = Normalize(rotate(light->bitangent));
            if (Length(tangent) < 0.9f || Length(bitangent) < 0.9f) return false;
            // Emitter dimensions are world meters: renderer extraction intentionally ignores light transform scale.
            for (const float x : {-0.5f, 0.5f})
                for (const float y : {-0.5f, 0.5f}) {
                    const auto point = transform.position + tangent * (light->width * x) + bitangent * (light->height * y);
                    if (!IsFinite(point)) return false;
                    bounds.Expand(point);
                }
        } else {
            if (!std::isfinite(light->sourceRadius) || light->sourceRadius < 0) return false;
            // Directional angular radius has no world extent; give its editor position a useful half-meter framing size.
            const float radius = light->type == gameplay::LightType::Directional ? 0.5f : std::max(light->sourceRadius, 0.05f);
            const Vec3 extent{radius, radius, radius};
            if (!IsFinite(transform.position - extent) || !IsFinite(transform.position + extent)) return false;
            bounds.Expand(transform.position - extent);
            bounds.Expand(transform.position + extent);
        }
        return true;
    }

    const auto* mesh = registry.try_get<gameplay::MeshRenderer>(entity);
    if (!mesh || !mesh->visible || mesh->triangleCount == 0) return false;
    const std::size_t first = mesh->firstTriangle, count = mesh->triangleCount;
    if (first > source.triangles.size() || count > source.triangles.size() - first) return false;
    // Only immutable source vertices belonging to the selected visible mesh contribute to its frame.
    for (std::size_t index = first; index < first + count; ++index)
        for (const auto& vertex : {source.triangles[index].a, source.triangles[index].b, source.triangles[index].c}) {
            const Vec3 point{vertex.x, vertex.y, vertex.z};
            if (!IsFinite(point) || !ExpandTransformed(bounds, point, transform, basis)) return false;
        }
    return true;
}

Vec3 AimWithRoll(Vec3 direction, float roll) {
    // Invert the engine's Rz*Ry*Rx forward vector after removing the retained authored Z rotation.
    const double cosine = std::cos(roll * radians), sine = std::sin(roll * radians);
    const double x = cosine * direction.x + sine * direction.y;
    const double y = -sine * direction.x + cosine * direction.y;
    return {static_cast<float>(std::asin(std::clamp(-y, -1.0, 1.0)) / radians),
        static_cast<float>(std::atan2(x, double(direction.z)) / radians), roll};
}
}

bool EditorNavigation::Update(gameplay::Transform& transform, const gameplay::Camera& camera, const NavigationInput& input) {
    if (!ValidCamera(transform, camera) || !ValidInput(input)) return false;
    auto next = *this;
    auto edited = transform;
    auto basis = MakeCameraBasis(edited.rotationDegrees.x, edited.rotationDegrees.y, edited.rotationDegrees.z);
    const float minimumDistance = std::max(camera.nearPlane * 2, 0.01f);
    const float dx = std::clamp(input.mouseDeltaX, -5000.0f, 5000.0f);
    const float dy = std::clamp(input.mouseDeltaY, -5000.0f, 5000.0f);
    const float wheel = std::clamp(input.wheel, -100.0f, 100.0f);
    if (!next.hasPivot_) {
        next.focusDistance_ = std::max(next.focusDistance_, minimumDistance);
        next.pivot_ = edited.position + basis.forward * next.focusDistance_;
        next.hasPivot_ = true;
    }
    if (!IsFinite(next.pivot_)) return false;

    switch (input.mode) {
    case NavigationMode::Fly: {
        // Wheel controls a persistent fly speed; only movement or mouse look changes the rendered camera.
        next.speed_ = static_cast<float>(std::clamp(double(next.speed_) * std::exp(double(wheel) * 0.2), 0.05, 500.0));
        if (dx != 0 || dy != 0) {
            edited.rotationDegrees.x = std::clamp(edited.rotationDegrees.x + dy * lookSensitivity, -89.5f, 89.5f);
            edited.rotationDegrees.y = std::remainder(edited.rotationDegrees.y + dx * lookSensitivity, 360.0f);
            basis = MakeCameraBasis(edited.rotationDegrees.x, edited.rotationDegrees.y, edited.rotationDegrees.z);
        }
        const auto direction = basis.forward * std::clamp(input.forward, -1.0f, 1.0f) +
            basis.right * std::clamp(input.right, -1.0f, 1.0f) + Vec3{0, std::clamp(input.up, -1.0f, 1.0f), 0};
        // Normalize combined keys so diagonal navigation cannot increase speed; cap stalls to avoid a resume jump.
        if (Length(direction) > 0) edited.position += Normalize(direction) * (next.speed_ * std::min(input.deltaSeconds, 0.1f));
        break;
    }
    case NavigationMode::Pan: {
        const double distance = std::max(double(Distance(edited.position - next.pivot_)), double(minimumDistance));
        const double perPixel = 2 * distance * std::tan(camera.verticalFovDegrees * radians * 0.5) / input.viewportHeight;
        const auto offset = (-basis.right * dx + basis.up * dy) * static_cast<float>(perPixel);
        // Moving both positions keeps the retained target stationary in the displayed image during a pan.
        edited.position += offset;
        next.pivot_ += offset;
        break;
    }
    case NavigationMode::Orbit:
        if (dx != 0 || dy != 0) {
            const auto offset = next.pivot_ - edited.position;
            const float distance = std::clamp(Distance(offset), minimumDistance, maximumDistance);
            edited.rotationDegrees = AimWithRoll(Normalize(offset), edited.rotationDegrees.z);
            edited.rotationDegrees.x = std::clamp(edited.rotationDegrees.x + dy * lookSensitivity, -89.5f, 89.5f);
            edited.rotationDegrees.y = std::remainder(edited.rotationDegrees.y + dx * lookSensitivity, 360.0f);
            basis = MakeCameraBasis(edited.rotationDegrees.x, edited.rotationDegrees.y, edited.rotationDegrees.z);
            edited.position = next.pivot_ - basis.forward * distance;
        }
        break;
    case NavigationMode::None:
    case NavigationMode::Dolly:
        break;
    default:
        return false;
    }

    // Dolly scales the camera-to-focus vector, keeping the target fixed without changing the projection lens.
    if (input.mode != NavigationMode::Fly && (wheel != 0 || (input.mode == NavigationMode::Dolly && (dx != 0 || dy != 0)))) {
        const auto offset = edited.position - next.pivot_;
        const float distance = std::max(Distance(offset), minimumDistance);
        const double drag = input.mode == NavigationMode::Dolly ? double(dx + dy) * 0.01 : 0;
        const double factor = std::exp(std::clamp(-double(wheel) * 0.15 - drag, -80.0, 80.0));
        const float result = static_cast<float>(std::clamp(distance * factor, double(minimumDistance), double(maximumDistance)));
        const auto direction = Length(Normalize(offset)) > 0 ? Normalize(offset) : -basis.forward;
        edited.position = next.pivot_ + direction * result;
    }

    if (!IsFinite(edited.position) || !IsFinite(edited.rotationDegrees) || !IsFinite(next.pivot_)) return false;
    next.focusDistance_ = std::clamp(Distance(edited.position - next.pivot_), minimumDistance, maximumDistance);
    const bool changed = Different(edited.position, transform.position) || Different(edited.rotationDegrees, transform.rotationDegrees);
    transform = edited;
    *this = next;
    return changed;
}

bool EditorNavigation::Focus(gameplay::Transform& transform, const gameplay::Camera& camera, const Scene& source,
    const gameplay::World& world, gameplay::Entity entity, float aspect) {
    if (!ValidCamera(transform, camera) || !std::isfinite(aspect) || aspect <= 0) return false;
    Aabb bounds;
    if (!TargetBounds(source, world, entity, bounds)) return false;
    const auto center = bounds.minimum * 0.5f + bounds.maximum * 0.5f;
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);
    const double vertical = std::tan(camera.verticalFovDegrees * radians * 0.5), horizontal = vertical * aspect;
    double distance = std::max(double(camera.nearPlane) * 2, 0.01);

    // Fit every world-box corner in both perspective axes; projected depth also reserves near-plane clearance.
    for (const float x : {bounds.minimum.x, bounds.maximum.x})
        for (const float y : {bounds.minimum.y, bounds.maximum.y})
            for (const float z : {bounds.minimum.z, bounds.maximum.z}) {
                const auto offset = Vec3{x, y, z} - center;
                const double depth = Dot(offset, basis.forward);
                distance = std::max({distance, std::abs(Dot(offset, basis.right)) * 1.1 / horizontal - depth,
                    std::abs(Dot(offset, basis.up)) * 1.1 / vertical - depth, double(camera.nearPlane) * 1.1 - depth});
            }
    if (!std::isfinite(distance) || distance > maximumDistance) return false;
    const auto position = center - basis.forward * static_cast<float>(distance);
    if (!IsFinite(position)) return false;

    // Commit focus state only after all selected geometry and resulting camera coordinates have validated.
    const bool changed = Different(position, transform.position);
    transform.position = position;
    pivot_ = center;
    focusDistance_ = static_cast<float>(distance);
    hasPivot_ = true;
    return changed;
}
}
