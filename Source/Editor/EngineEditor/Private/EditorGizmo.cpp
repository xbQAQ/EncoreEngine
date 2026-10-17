#include "editor/EditorGizmo.h"
#include "gameplay/World.h"
#include "render/Bvh.h"
#include "render/SceneExtraction.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace render::editor {
namespace {
constexpr float degrees = 180.0f / 3.14159265358979323846f;
Vec3 Position(const Float4& value) { return {value.x, value.y, value.z}; }
Vec3 Column(const GizmoMatrix& matrix, std::size_t column) {
    return {matrix[column * 4], matrix[column * 4 + 1], matrix[column * 4 + 2]};
}

float Unwrap(float angle, float reference) {
    // Equivalent angles nearest the existing inspector value avoid jumps at the +/-180-degree seam.
    return reference + std::remainder(angle - reference, 360.0f);
}

bool ValidCamera(const gameplay::Transform& transform, const gameplay::Camera& camera, float aspect) {
    return IsFinite(transform.position) && IsFinite(transform.rotationDegrees) && std::isfinite(aspect) && aspect > 0 &&
        std::isfinite(camera.verticalFovDegrees) && camera.verticalFovDegrees > 0 && camera.verticalFovDegrees < 180 &&
        std::isfinite(camera.nearPlane) && std::isfinite(camera.farPlane) && camera.nearPlane > 0 && camera.farPlane > camera.nearPlane;
}

std::optional<std::array<float, 2>> HitUv(const Scene& scene, const GpuTriangle& triangle, const Ray& ray) {
    const auto edge1 = Position(triangle.b) - Position(triangle.a), edge2 = Position(triangle.c) - Position(triangle.a);
    const auto p = Cross(ray.direction, edge2), offset = ray.origin - Position(triangle.a);
    // 筛选阶段尚未调用 Trace，先使用相同的重心与裁剪条件拒绝平行或未命中三角形。
    const double determinant = Dot(edge1, p);
    if (std::abs(determinant) < 1e-10) return {};
    const double inverse = 1.0 / determinant;
    const double u = Dot(offset, p) * inverse, v = Dot(ray.direction, Cross(offset, edge1)) * inverse;
    const double distance = Dot(edge2, Cross(offset, edge1)) * inverse;
    if (u < 0 || u > 1 || v < 0 || u + v > 1 || distance < ray.tMin || distance > ray.tMax) return {};
    if (triangle.b.w < 1 || triangle.b.w > static_cast<float>(scene.attributes.size())) return std::array<float, 2>{};
    const auto& attribute = scene.attributes[static_cast<std::size_t>(triangle.b.w - 1)];
    // 镜像提取后的顶点权重与着色器 TriangleUV 一致，缺失属性使用默认零 UV。
    return std::array<float, 2>{static_cast<float>(attribute.uv01.x * (1 - u - v) + attribute.uv01.z * u + attribute.uv2.x * v),
        static_cast<float>(attribute.uv01.y * (1 - u - v) + attribute.uv01.w * u + attribute.uv2.y * v)};
}

std::int64_t WrapTexel(std::int64_t coordinate, std::uint32_t extent, std::uint32_t mode) {
    const auto size = static_cast<std::int64_t>(std::max(extent, 1u));
    if (mode == 1) return std::clamp(coordinate, std::int64_t{0}, size - 1);
    const auto period = mode == 2 ? size * 2 : size;
    const auto wrapped = (coordinate % period + period) % period;
    // 镜像重复边缘像素，负索引同样遵循着色器 WrapImageCoordinate 的寻址方式。
    return mode == 2 && wrapped >= size ? period - 1 - wrapped : wrapped;
}

float TexelAlpha(const Scene& scene, const GpuTextureView& view, std::int64_t x, std::int64_t y) {
    const auto column = WrapTexel(x, view.image.y, view.image.w & 3u);
    const auto row = WrapTexel(y, view.image.z, (view.image.w >> 2u) & 3u);
    const auto index = static_cast<std::uint64_t>(view.image.x) + static_cast<std::uint64_t>(row) * view.image.y + static_cast<std::uint64_t>(column);
    // 缺失图像采样使用白色，sRGB 基础色图像的透明度仍保持线性。
    return index < scene.packedTexels.size() ? static_cast<float>(scene.packedTexels[static_cast<std::size_t>(index)] >> 24u) / 255.0f : 1.0f;
}

float BaseTextureAlpha(const Scene& scene, std::uint32_t texture, std::array<float, 2> uv) {
    if (texture >= scene.textureViews.size()) return 1;
    const auto& view = scene.textureViews[texture];
    const float scaledU = uv[0] * view.offsetScale.z, scaledV = uv[1] * view.offsetScale.w;
    uv = {view.rotation.y * scaledU - view.rotation.x * scaledV + view.offsetScale.x,
        view.rotation.x * scaledU + view.rotation.y * scaledV + view.offsetScale.y};
    if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return 1;
    // 在归一化重复寻址前应用 KHR 缩放、旋转和平移，与 SampleMaterialTexture 保持一致。
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto mode = (view.image.w >> (axis * 2)) & 3u;
        if (mode == 1) uv[axis] = std::clamp(uv[axis], 0.0f, 1.0f);
        else {
            const float period = mode == 2 ? 2.0f : 1.0f;
            uv[axis] = (uv[axis] / period - std::floor(uv[axis] / period)) * period;
        }
    }
    const float pixelX = uv[0] * static_cast<float>(view.image.y) - 0.5f;
    const float pixelY = uv[1] * static_cast<float>(view.image.z) - 0.5f;
    const auto x = static_cast<std::int64_t>(std::floor(pixelX)), y = static_cast<std::int64_t>(std::floor(pixelY));
    const float fractionX = pixelX - std::floor(pixelX), fractionY = pixelY - std::floor(pixelY);
    const float a = TexelAlpha(scene, view, x, y), b = TexelAlpha(scene, view, x + 1, y);
    const float c = TexelAlpha(scene, view, x, y + 1), d = TexelAlpha(scene, view, x + 1, y + 1);
    // 像素中心与四个独立重复寻址采样点复现渲染器的双线性透明覆盖。
    const float top = a + (b - a) * fractionX, bottom = c + (d - c) * fractionX;
    return top + (bottom - top) * fractionY;
}

bool AcceptPickSurface(const Scene& scene, const GpuTriangle& triangle, const Ray& ray) {
    const auto& material = scene.materials[static_cast<std::size_t>(triangle.a.w)];
    const auto geometric = Cross(Position(triangle.b) - Position(triangle.a), Position(triangle.c) - Position(triangle.a));
    // 只有双面或作者透射表面接受背面，与 SceneQueries 的可见性约定一致。
    if (material.flags.y == 0 && material.transmissionIor.x <= 0 && Dot(geometric, ray.direction) >= 0) return false;
    if (material.flags.z != 1) return true;
    const auto uv = HitUv(scene, triangle, ray);
    if (!uv) return false;
    const float alpha = material.extras.w * BaseTextureAlpha(scene, material.textures.x, *uv);
    return alpha >= material.extras.z;
}
}

bool ViewportRect::Contains(float screenX, float screenY) const {
    // Exclusive far edges keep clicks on a neighboring panel or letterbox outside the image.
    return std::isfinite(screenX) && std::isfinite(screenY) && width > 0 && height > 0 &&
        screenX >= x && screenY >= y && screenX < x + width && screenY < y + height;
}

Vec3 MeshPivot(const Scene& source, const gameplay::MeshRenderer& mesh) {
    const std::size_t first = mesh.firstTriangle, count = mesh.triangleCount;
    if (count == 0 || first > source.triangles.size() || count > source.triangles.size() - first) return {};
    Aabb bounds;
    // Source positions can already include an imported node transform; their bounds locate the visible object center.
    for (std::size_t index = first; index < first + count; ++index)
        for (const auto& vertex : {source.triangles[index].a, source.triangles[index].b, source.triangles[index].c}) bounds.Expand(Position(vertex));
    return bounds.minimum * 0.5f + bounds.maximum * 0.5f;
}

GizmoMatrix GizmoPose(const gameplay::Transform& transform, Vec3 pivot) {
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);
    const auto right = basis.right * transform.scale.x, up = basis.up * transform.scale.y, forward = basis.forward * transform.scale.z;
    const auto center = transform.position + right * pivot.x + up * pivot.y + forward * pivot.z;
    // Columns encode Rz*Ry*Rx*S exactly as scene extraction; translation is the transformed source pivot.
    return {right.x, right.y, right.z, 0, up.x, up.y, up.z, 0, forward.x, forward.y, forward.z, 0,
        center.x, center.y, center.z, 1};
}

std::optional<gameplay::Transform> GizmoTransform(const GizmoMatrix& matrix, Vec3 pivot, const gameplay::Transform& previous) {
    if (!IsFinite(pivot) || !IsFinite(previous.rotationDegrees) || !IsFinite(previous.scale)) return {};
    for (const float value : matrix) if (!std::isfinite(value)) return {};
    if (std::abs(matrix[3]) > 0.00001f || std::abs(matrix[7]) > 0.00001f ||
        std::abs(matrix[11]) > 0.00001f || std::abs(matrix[15] - 1) > 0.00001f) return {};

    // Keep authored reflection signs; local scaling never crosses zero into a different handedness.
    const Vec3 lengths{Length(Column(matrix, 0)), Length(Column(matrix, 1)), Length(Column(matrix, 2))};
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (lengths[axis] < 0.001f || lengths[axis] > 10000 || !std::isfinite(lengths[axis])) return {};
    gameplay::Transform result = previous;
    result.scale = {std::copysign(lengths.x, previous.scale.x), std::copysign(lengths.y, previous.scale.y), std::copysign(lengths.z, previous.scale.z)};
    const auto right = Column(matrix, 0) / result.scale.x, up = Column(matrix, 1) / result.scale.y, forward = Column(matrix, 2) / result.scale.z;
    if (std::abs(Dot(right, up)) > 0.001f || std::abs(Dot(right, forward)) > 0.001f ||
        std::abs(Dot(up, forward)) > 0.001f || Dot(Cross(right, up), forward) < 0.999f) return {};

    // Invert Rz*Ry*Rx directly instead of using ImGuizmo's different Euler decomposition convention.
    const float horizontal = std::hypot(right.x, right.y);
    Vec3 angles;
    if (horizontal > 0.00001f) {
        angles = {std::atan2(up.z, forward.z) * degrees, std::atan2(-right.z, horizontal) * degrees,
            std::atan2(right.y, right.x) * degrees};
        Vec3 alternate{angles.x + 180, 180 - angles.y, angles.z + 180};
        angles = {Unwrap(angles.x, previous.rotationDegrees.x), Unwrap(angles.y, previous.rotationDegrees.y), Unwrap(angles.z, previous.rotationDegrees.z)};
        alternate = {Unwrap(alternate.x, previous.rotationDegrees.x), Unwrap(alternate.y, previous.rotationDegrees.y), Unwrap(alternate.z, previous.rotationDegrees.z)};
        if (Length(alternate - previous.rotationDegrees) < Length(angles - previous.rotationDegrees)) angles = alternate;
    } else {
        // At yaw +/-90 degrees only a combined X/Z angle is observable; keep the previous Z angle stable.
        const float yaw = right.z < 0 ? 90.0f : -90.0f;
        const float combined = std::atan2((yaw > 0 ? 1.0f : -1.0f) * up.x, up.y) * degrees;
        angles = {Unwrap(combined + (yaw > 0 ? previous.rotationDegrees.z : -previous.rotationDegrees.z), previous.rotationDegrees.x),
            Unwrap(yaw, previous.rotationDegrees.y), previous.rotationDegrees.z};
    }
    result.rotationDegrees = angles;
    result.position = Column(matrix, 3) - Column(matrix, 0) * pivot.x - Column(matrix, 1) * pivot.y - Column(matrix, 2) * pivot.z;
    if (!IsFinite(result.position) || !IsFinite(result.rotationDegrees)) return {};
    return result;
}

GizmoView MakeGizmoView(const gameplay::Transform& transform, const gameplay::Camera& camera, float aspect) {
    if (!ValidCamera(transform, camera, aspect)) throw std::invalid_argument("Gizmo camera must have a finite pose and valid perspective");
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);
    const float tangent = std::tan(camera.verticalFovDegrees * (0.5f / degrees));
    const float range = camera.farPlane - camera.nearPlane;
    // Positive view Z and w=Z match GBuffer rasterization and the renderer's primary-ray basis.
    return {{basis.right.x, basis.up.x, basis.forward.x, 0, basis.right.y, basis.up.y, basis.forward.y, 0,
             basis.right.z, basis.up.z, basis.forward.z, 0, -Dot(basis.right, transform.position), -Dot(basis.up, transform.position), -Dot(basis.forward, transform.position), 1},
            {1 / (tangent * aspect), 0, 0, 0, 0, 1 / tangent, 0, 0, 0, 0, camera.farPlane / range, 1,
             0, 0, -camera.nearPlane * camera.farPlane / range, 0}};
}

std::optional<Ray> ViewportRay(const ViewportRect& image, float screenX, float screenY,
    const gameplay::Transform& transform, const gameplay::Camera& camera) {
    if (!image.Contains(screenX, screenY) || !ValidCamera(transform, camera, image.width / image.height)) return {};
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);
    const float tangent = std::tan(camera.verticalFovDegrees * (0.5f / degrees));
    const float u = (2 * (screenX - image.x) / image.width - 1) * tangent * image.width / image.height;
    const float v = (1 - 2 * (screenY - image.y) / image.height) * tangent;
    const auto direction = Normalize(basis.forward + basis.right * u + basis.up * v);
    // Clip planes constrain forward depth, while Bvh::Trace measures along the normalized oblique ray.
    const float forward = Dot(direction, basis.forward);
    return Ray{transform.position, direction, camera.nearPlane / forward, camera.farPlane / forward};
}

void SurfacePickCache::Reset() {
    // 显式文档替换可能复用 World 地址与版本值，必须连同大块采样缓存一起清空。
    source_ = nullptr;
    world_ = nullptr;
    extracted_ = {};
    owners_.clear();
    candidates_.clear();
    leaf_.clear();
    ray_.reset();
    hit_.reset();
    ready_ = false;
    sceneBuildCount_ = 0;
    sceneInputs_ = {};
}

std::optional<SurfaceHit> SurfacePickCache::Pick(const Scene& source, const gameplay::World& world, const Ray& ray) {
    const bool sameSource = ready_ && source_ == &source && world_ == &world;
    if (!sameSource || revision_ != world.Revision()) {
        auto inputs = world.ExtractRenderSnapshot();
        // 视图变化只更新接受版本，既有几何和相同射线结果都仍然有效。
        if (sameSource && SameRenderSceneInputs(sceneInputs_, inputs)) {
            ValidateRenderCameras(inputs.cameras);
            revision_ = inputs.revision;
        } else {
            // 材质透明性、网格和解析发光体变化仍完整重建，失败时禁止复用旧快照。
            ready_ = false;
            extracted_ = ExtractRenderScene(source, inputs, source.cornell, &owners_);
            leaf_.clear();
            if (!extracted_.triangles.empty()) {
                Aabb bounds;
                for (const auto& triangle : extracted_.triangles)
                    for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) bounds.Expand(Position(vertex));
                leaf_.push_back({{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, 0},
                    {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z, 0},
                    {0, 0, 0, static_cast<std::uint32_t>(extracted_.triangles.size())}});
            }
            source_ = &source;
            world_ = &world;
            revision_ = world.Revision();
            sceneInputs_ = std::move(inputs);
            ray_.reset();
            hit_.reset();
            ready_ = true;
            // 单独计数提取成本，射线改变不计为场景重建。
            ++sceneBuildCount_;
        }
    }
    const auto equal = [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    if (ray_ && equal(ray_->origin, ray.origin) && equal(ray_->direction, ray.direction) && ray_->tMin == ray.tMin && ray_->tMax == ray.tMax)
        return hit_;

    // 每条新射线从未筛选快照恢复工作区，拒绝结果不能污染下一条射线。
    candidates_ = extracted_.triangles;
    for (auto& triangle : candidates_) if (!AcceptPickSurface(extracted_, triangle, ray)) {
        triangle.b.x = triangle.c.x = triangle.a.x;
        triangle.b.y = triangle.c.y = triangle.a.y;
        triangle.b.z = triangle.c.z = triangle.a.z;
    }
    // 先线性筛选再求交一次，避免长透明或背面堆栈反复重查整个三角形数组。
    const auto hit = Bvh::Trace(ray, candidates_, leaf_);
    hit_.reset();
    if (hit.Found()) {
        const auto entity = hit.triangleIndex < owners_.size() ? GizmoTarget(world, owners_[hit.triangleIndex]) : gameplay::Entity{entt::null};
        hit_ = SurfaceHit{entity, hit.position, hit.normal, hit.distance};
    }
    ray_ = ray;
    return hit_;
}

std::optional<SurfaceHit> PickSurface(const Scene& source, const gameplay::World& world, const Ray& ray) {
    // 独立调用保留无跨帧状态的行为，交互调用显式借用编辑器拥有的缓存。
    SurfacePickCache query;
    return query.Pick(source, world, ray);
}

gameplay::Entity PickEntity(const Scene& source, const gameplay::World& world, const Ray& ray, SurfacePickCache* cache) {
    // 选择与放置使用同一查询，避免材质透明或背面行为产生分歧。
    const auto hit = cache ? cache->Pick(source, world, ray) : PickSurface(source, world, ray);
    return hit ? hit->entity : gameplay::Entity{entt::null};
}

gameplay::Entity GizmoTarget(const gameplay::World& world, gameplay::Entity entity) {
    const auto& registry = world.Registry();
    if (!registry.valid(entity) || !registry.all_of<gameplay::Transform>(entity)) return entt::null;
    const auto* mesh = registry.try_get<gameplay::MeshRenderer>(entity);
    if (!mesh || !mesh->lightOwned) return entity;
    auto owner = gameplay::Entity{entt::null};
    // Cornell extraction binds its source emitter to the first enabled rectangle in deterministic entity order.
    for (auto [candidate, light] : registry.view<const gameplay::Light>().each())
        if (light.enabled && light.type == gameplay::LightType::Rectangle &&
            (owner == entt::null || entt::to_integral(candidate) < entt::to_integral(owner))) owner = candidate;
    return owner;
}
}
