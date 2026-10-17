#include "render/SceneExtraction.h"
#include "gameplay/Components.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace render {
namespace {
constexpr std::uint32_t emitterMaterial = 3;
// 一基浮点属性索引仅在二的二十四次方以内可精确表示所有整数。
constexpr std::size_t maximumAttributeCount = std::size_t{1} << 24;

// 业务组件按字段比较，避免 bool、句柄附近的填充字节造成无意义的失效。
bool Same(Vec3 left, Vec3 right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}
// 任意实例位姿变化都要求重新生成世界空间几何或光源表示。
bool Same(const gameplay::Transform& left, const gameplay::Transform& right) {
    return Same(left.position, right.position) && Same(left.rotationDegrees, right.rotationDegrees) && Same(left.scale, right.scale);
}
// 实体身份、资源范围和覆盖模式共同决定提取结果，不能只比较位姿。
bool Same(const gameplay::RenderInstance& left, const gameplay::RenderInstance& right) {
    const auto& a = left.mesh;
    const auto& b = right.mesh;
    return left.entity == right.entity && Same(left.transform, right.transform) && a.firstTriangle == b.firstTriangle &&
        a.triangleCount == b.triangleCount && a.materialIndex == b.materialIndex && a.visible == b.visible &&
        a.lightOwned == b.lightOwned && a.meshResource == b.meshResource && a.useSurfaceMaterials == b.useSurfaceMaterials;
}

// 解析面光会生成几何和材质，其颜色、采样轴及发光体归属都必须参与缓存失效。
bool Same(const gameplay::RenderLight& left, const gameplay::RenderLight& right) {
    const auto& a = left.light;
    const auto& b = right.light;
    return left.entity == right.entity && Same(left.transform, right.transform) && a.type == b.type && Same(a.color, b.color) &&
        a.intensity == b.intensity && a.width == b.width && a.height == b.height && a.range == b.range && a.enabled == b.enabled &&
        a.sourceRadius == b.sourceRadius && a.outerAngleDegrees == b.outerAngleDegrees && a.innerAngleDegrees == b.innerAngleDegrees &&
        Same(a.direction, b.direction) && Same(a.tangent, b.tangent) && Same(a.bitangent, b.bitangent) && a.emitterMaterial == b.emitterMaterial;
}

// 材质的每个 GPU 通道都影响着色或透明拾取，不依赖结构体对象表示进行比较。
template<class Lane> bool SameLane(const Lane& left, const Lane& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}
bool Same(const GpuMaterial& left, const GpuMaterial& right) {
    return SameLane(left.baseRoughness, right.baseRoughness) && SameLane(left.emissionMetallic, right.emissionMetallic) &&
        SameLane(left.transmissionIor, right.transmissionIor) && SameLane(left.principled, right.principled) &&
        SameLane(left.sheen, right.sheen) && SameLane(left.medium, right.medium) && SameLane(left.flags, right.flags) &&
        SameLane(left.textures, right.textures) && SameLane(left.textures2, right.textures2) &&
        SameLane(left.extras, right.extras) && SameLane(left.tangentFrame, right.tangentFrame);
}

void ValidateTransform(const gameplay::Transform& transform) {
    if (!IsFinite(transform.position) || !IsFinite(transform.rotationDegrees) || !IsFinite(transform.scale))
        throw std::invalid_argument("scene extraction requires finite position, rotation and scale");
}

bool Finite(const Float4& lane) {
    return std::isfinite(lane.x) && std::isfinite(lane.y) && std::isfinite(lane.z) && std::isfinite(lane.w);
}

void ValidateMaterial(const GpuMaterial& material, std::size_t textureCount) {
    // 可编辑 Disney 参数与导入基础材质遵循相同的有限数值约束。
    for (const auto* lane : {&material.baseRoughness, &material.emissionMetallic, &material.transmissionIor,
                            &material.principled, &material.sheen, &material.medium, &material.extras, &material.tangentFrame})
        if (!Finite(*lane)) throw std::invalid_argument("scene extraction material contains a nonfinite parameter");
    for (const auto* views : {&material.textures, &material.textures2})
        for (const auto index : {views->x, views->y, views->z, views->w})
            if (index != ~0u && index >= textureCount)
                throw std::invalid_argument("scene extraction material references an invalid texture view");
}

void ValidateAssets(const Scene& reference) {
    // CPU 寻址或着色器上传之前拒绝无效资源间接引用。
    for (const auto& attribute : reference.attributes)
        for (const auto* lane : {&attribute.normal0, &attribute.normal1, &attribute.normal2, &attribute.uv01, &attribute.uv2, &attribute.tangent})
            if (!Finite(*lane)) throw std::invalid_argument("scene extraction triangle attribute contains a nonfinite parameter");
    for (const auto& view : reference.textureViews) {
        const std::uint64_t end = std::uint64_t(view.image.x) + std::uint64_t(view.image.y) * view.image.z;
        if (!view.image.y || !view.image.z || end > reference.packedTexels.size() ||
            !Finite(view.offsetScale) || !Finite(view.rotation) || view.rotation.z != 0 ||
            (view.image.w & 3u) > 2u || ((view.image.w >> 2) & 3u) > 2u || (view.image.w >> 4) != 0)
            throw std::invalid_argument("scene extraction texture view has an invalid image range, transform or sampler");
    }
}

float FiniteFloat(double value) {
    if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max())
        throw std::invalid_argument("scene extraction produced a coordinate or radiance outside finite float range");
    return static_cast<float>(value);
}

Vec3 TransformPoint(const Vec3& point, const gameplay::Transform& transform) {
    constexpr double degreesToRadians = 3.14159265358979323846 / 180.0;
    const double angleX = double(transform.rotationDegrees.x) * degreesToRadians;
    const double angleY = double(transform.rotationDegrees.y) * degreesToRadians;
    const double angleZ = double(transform.rotationDegrees.z) * degreesToRadians;
    double x = double(point.x) * transform.scale.x;
    double y = double(point.y) * transform.scale.y;
    double z = double(point.z) * transform.scale.z;
    // 先缩放，再依次应用右手系 X、Y、Z 旋转。
    const double afterXy = y * std::cos(angleX) - z * std::sin(angleX);
    z = y * std::sin(angleX) + z * std::cos(angleX);
    y = afterXy;
    const double afterYx = x * std::cos(angleY) + z * std::sin(angleY);
    z = -x * std::sin(angleY) + z * std::cos(angleY);
    x = afterYx;
    const double afterZx = x * std::cos(angleZ) - y * std::sin(angleZ);
    y = x * std::sin(angleZ) + y * std::cos(angleZ);
    x = afterZx;
    return {FiniteFloat(x + transform.position.x), FiniteFloat(y + transform.position.y), FiniteFloat(z + transform.position.z)};
}

Float4 MoveVertex(const Float4& vertex, const gameplay::Transform& transform) {
    const auto position = TransformPoint({vertex.x, vertex.y, vertex.z}, transform);
    return {position.x, position.y, position.z, vertex.w};
}

struct EmitterReference {
    Vec3 center; // 作者参考发光体的世界空间中心。
    Vec3 tangent{1, 0, 0}; // 参考发光体的单位宽轴。
    Vec3 bitangent{0, 0, 1}; // 参考发光体的单位高轴。
    Vec3 direction{0, -1, 0}; // 参考发光体的单位发射方向。
    float width{1}; // 作者参考发光体完整宽度。
    float height{1}; // 作者参考发光体完整高度。
};

EmitterReference ReferenceEmitter(const Scene& reference) {
    // 历史兼容入口也从作者光源或实际源范围推导参考坐标系，不维护房间尺寸常量。
    for (const auto& light : reference.lights) if (light.shadow.z == emitterMaterial) {
        if (!Finite(light.positionType) || !Finite(light.tangentWidth) || !Finite(light.bitangentHeight) ||
            !Finite(light.directionRange) || light.tangentWidth.w <= 0 || light.bitangentHeight.w <= 0)
            throw std::invalid_argument("参考发光体坐标系或尺寸无效");
        return {{light.positionType.x, light.positionType.y, light.positionType.z},
            Normalize({light.tangentWidth.x, light.tangentWidth.y, light.tangentWidth.z}),
            Normalize({light.bitangentHeight.x, light.bitangentHeight.y, light.bitangentHeight.z}),
            Normalize({light.directionRange.x, light.directionRange.y, light.directionRange.z}),
            light.tangentWidth.w, light.bitangentHeight.w};
    }
    Aabb bounds;
    bool found = false;
    for (const auto& triangle : reference.triangles) if (triangle.a.w == static_cast<float>(emitterMaterial)) {
        found = true;
        for (const auto& v : {triangle.a, triangle.b, triangle.c}) bounds.Expand({v.x, v.y, v.z});
    }
    // 最小兼容场景可以没有发光体；默认坐标系只占位，不会参与任何顶点移动。
    if (!found) return {};
    const auto extent = bounds.maximum - bounds.minimum;
    if (!IsFinite(extent) || extent.x <= 0 || extent.z <= 0)
        throw std::invalid_argument("兼容发光体缺少有效源范围或作者光源坐标系");
    EmitterReference frame;
    frame.center = (bounds.minimum + bounds.maximum) * 0.5f;
    frame.width = extent.x;
    frame.height = extent.z;
    return frame;
}

Float4 MoveEmitterVertex(const Float4& vertex, const gameplay::RenderLight& emitter, const EmitterReference& frame) {
    const Vec3 delta = Vec3{vertex.x, vertex.y, vertex.z} - frame.center;
    const auto local = Normalize(emitter.light.tangent) * FiniteFloat(double(Dot(delta, frame.tangent)) / frame.width * emitter.light.width) +
                       Normalize(emitter.light.bitangent) * FiniteFloat(double(Dot(delta, frame.bitangent)) / frame.height * emitter.light.height) +
                       Normalize(emitter.light.direction) * Dot(delta, frame.direction);
    auto pose = emitter.transform;
    pose.scale = {1, 1, 1};
    const auto position = TransformPoint(local, pose);
    return {position.x, position.y, position.z, vertex.w};
}

void HideTriangle(GpuTriangle& triangle) {
    // 退化三角形保留原语索引和加速结构分配，同时不再可被命中。
    triangle.a.x = triangle.b.x = triangle.c.x = 0;
    triangle.a.y = triangle.b.y = triangle.c.y = -100000;
    triangle.a.z = triangle.b.z = triangle.c.z = 0;
    triangle.c.w = 0;
}
}

bool SameRenderSceneInputs(const gameplay::RenderSnapshot& left, const gameplay::RenderSnapshot& right) {
    // 只扫描快照中的小型作者输入，不访问三角形、属性和纹理像素数组。
    const auto equal = [](const auto& a, const auto& b) { return Same(a, b); };
    return std::equal(left.instances.begin(), left.instances.end(), right.instances.begin(), right.instances.end(), equal) &&
        std::equal(left.materials.begin(), left.materials.end(), right.materials.begin(), right.materials.end(), equal) &&
        std::equal(left.lights.begin(), left.lights.end(), right.lights.begin(), right.lights.end(), equal);
}

void ValidateRenderCameras(std::span<const gameplay::RenderCamera> cameras) {
    // 所有相机都在提取边界验证，而非只验证本帧最终选中的第一台相机。
    for (const auto& camera : cameras) {
        ValidateTransform(camera.transform);
        if (!std::isfinite(camera.camera.verticalFovDegrees) || !std::isfinite(camera.camera.nearPlane) ||
            !std::isfinite(camera.camera.farPlane) || camera.camera.verticalFovDegrees <= 0 ||
            camera.camera.verticalFovDegrees >= 180 || camera.camera.nearPlane <= 0 || camera.camera.farPlane <= camera.camera.nearPlane)
            throw std::invalid_argument("scene extraction camera projection is invalid");
    }
}

Scene ExtractRenderScene(const Scene& reference, const gameplay::RenderSnapshot& snapshot, bool cornell,
    std::vector<gameplay::Entity>* triangleOwners) {
    if (cornell && reference.materials.size() <= emitterMaterial)
        throw std::invalid_argument("Cornell scene extraction requires emitter material index three");
    if (reference.triangles.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("scene extraction triangle count exceeds mesh range capacity");
    if (reference.attributes.size() > maximumAttributeCount)
        throw std::invalid_argument("scene extraction attribute count exceeds exact float index capacity");
    ValidateAssets(reference);
    for (const auto& material : reference.materials) ValidateMaterial(material, reference.textureViews.size());
    for (const auto& material : snapshot.materials) ValidateMaterial(material, reference.textureViews.size());
    for (const auto& triangle : reference.triangles) {
        if (!Finite(triangle.a) || !Finite(triangle.b) || !Finite(triangle.c) || triangle.a.w < 0 ||
            std::floor(triangle.a.w) != triangle.a.w || double(triangle.a.w) >= double(reference.materials.size()))
            throw std::invalid_argument("scene extraction reference triangle has invalid vertices or material");
        if (triangle.b.w < 0 || std::floor(triangle.b.w) != triangle.b.w || triangle.b.w > double(reference.attributes.size()))
            throw std::invalid_argument("scene extraction reference triangle has an invalid attribute index");
    }
    const auto emitterReference = cornell ? ReferenceEmitter(reference) : EmitterReference{};

    // 只有持有同一非零资源标识的独立实体才能共享完全相同的源范围。
    constexpr auto unowned = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> sourceOwners(reference.triangles.size(), unowned);
    std::vector<std::size_t> destinations;
    destinations.reserve(snapshot.instances.size());
    std::unordered_set<gameplay::Entity> entities;
    std::size_t triangleCount = reference.triangles.size();
    for (std::size_t instanceIndex = 0; instanceIndex < snapshot.instances.size(); ++instanceIndex) {
        const auto& instance = snapshot.instances[instanceIndex];
        if (instance.entity == entt::null || !entities.insert(instance.entity).second)
            throw std::invalid_argument("scene extraction requires distinct nonnull mesh entity identities");
        ValidateTransform(instance.transform);
        if (instance.mesh.materialIndex >= reference.materials.size())
            throw std::invalid_argument("scene extraction mesh material is outside the reference material array");
        const std::size_t first = instance.mesh.firstTriangle;
        const std::size_t count = instance.mesh.triangleCount;
        if (first > reference.triangles.size() || count > reference.triangles.size() - first)
            throw std::invalid_argument("scene extraction mesh triangle range is outside the reference scene");
        if (count && sourceOwners[first] != unowned) {
            const auto ownerIndex = sourceOwners[first];
            const auto& owner = snapshot.instances[ownerIndex].mesh;
            if (owner.firstTriangle != first || owner.triangleCount != count || !instance.mesh.meshResource ||
                instance.mesh.meshResource != owner.meshResource)
                throw std::invalid_argument("scene extraction mesh triangle ranges overlap without a shared exact resource range");
            for (std::size_t index = first; index < first + count; ++index)
                if (sourceOwners[index] != ownerIndex) throw std::invalid_argument("scene extraction mesh triangle ranges partially overlap");
            if (count > std::numeric_limits<std::uint32_t>::max() - triangleCount)
                throw std::invalid_argument("scene extraction instanced triangle count exceeds mesh range capacity");
            destinations.push_back(triangleCount);
            triangleCount += count;
        } else {
            for (std::size_t index = first; index < first + count; ++index) {
                if (sourceOwners[index] != unowned) throw std::invalid_argument("scene extraction mesh triangle ranges partially overlap");
                sourceOwners[index] = instanceIndex;
            }
            destinations.push_back(first);
        }
    }
    ValidateRenderCameras(snapshot.cameras);
    const gameplay::RenderLight* emitter = nullptr;
    unsigned emitterLightTag = 0;
    unsigned enabledLights = 0;
    for (const auto& light : snapshot.lights) {
        ValidateTransform(light.transform);
        const auto& parameters = light.light;
        if (!IsFinite(parameters.color) || !std::isfinite(parameters.intensity) || !std::isfinite(parameters.width) ||
            !std::isfinite(parameters.height) || !std::isfinite(parameters.range) || parameters.color.x < 0 ||
            parameters.color.y < 0 || parameters.color.z < 0 || parameters.intensity < 0 || parameters.range < 0)
            throw std::invalid_argument("scene extraction light contains an invalid color, power or extent");
        if (parameters.enabled && ++enabledLights > 16) throw std::invalid_argument("scene supports at most sixteen enabled lights");
        if (!IsFinite(parameters.direction) || !IsFinite(parameters.tangent) || !IsFinite(parameters.bitangent) ||
            !std::isfinite(parameters.sourceRadius) || parameters.sourceRadius < 0 ||
            !std::isfinite(parameters.outerAngleDegrees) || !std::isfinite(parameters.innerAngleDegrees))
            throw std::invalid_argument("scene extraction light frame or source radius is invalid");
        switch (parameters.type) {
        case gameplay::LightType::Directional:
        case gameplay::LightType::Point:
            break;
        case gameplay::LightType::Spot:
            if (parameters.innerAngleDegrees < 0 || parameters.outerAngleDegrees <= 0 || parameters.outerAngleDegrees > 90 ||
                parameters.innerAngleDegrees > parameters.outerAngleDegrees)
                throw std::invalid_argument("spot angles must satisfy 0 <= inner <= outer <= 90 degrees");
            break;
        case gameplay::LightType::Rectangle:
        case gameplay::LightType::Disk:
            if (parameters.width <= 0 || parameters.height <= 0)
                throw std::invalid_argument("scene extraction rectangle light edges must be positive");
            if (cornell && parameters.type == gameplay::LightType::Rectangle && parameters.enabled && !emitter) {
                emitter = &light;
                emitterLightTag = enabledLights;
            }
            break;
        default:
            throw std::invalid_argument("scene extraction light type is invalid");
        }
    }

    Scene output = reference;
    output.triangles.resize(triangleCount);
    std::vector<gameplay::Entity> owners;
    if (triangleOwners) owners.resize(triangleCount, entt::null);
    std::vector<std::size_t> attributeOwners(reference.attributes.size(), unowned);
    output.lights.clear();
    output.lightNames.clear();
    while (output.materialNames.size() < output.materials.size())
        output.materialNames.push_back("Material " + std::to_string(output.materialNames.size()));
    if (!snapshot.materials.empty()) {
        if (snapshot.materials.size() != reference.materials.size()) throw std::invalid_argument("snapshot material table size changed");
        output.materials = snapshot.materials;
    }
    // 缺失范围保持隐藏，首个所有者恢复源槽，共享资源的复制实例追加独立槽。
    for (auto& triangle : output.triangles) HideTriangle(triangle);
    for (std::size_t instanceIndex = 0; instanceIndex < snapshot.instances.size(); ++instanceIndex) {
        const auto& instance = snapshot.instances[instanceIndex];
        const std::size_t first = instance.mesh.firstTriangle;
        const std::size_t count = instance.mesh.triangleCount;
        const std::size_t destination = destinations[instanceIndex];
        // 配置发光体由解析光源生成唯一可见几何，源槽保持隐藏以免重复发射与错误 MIS 标签。
        if (!cornell && instance.mesh.lightOwned) continue;
        for (std::size_t index = first; index < first + count; ++index) {
            const auto outputIndex = destination + index - first;
            auto& triangle = output.triangles[outputIndex];
            if (instance.mesh.visible) {
                const auto& original = reference.triangles[index];
                // 光源实体拥有发光体姿态，其对应网格的变换不参与计算。
                if (cornell && original.a.w == static_cast<float>(emitterMaterial)) {
                    if (emitter) triangle = {MoveEmitterVertex(original.a, *emitter, emitterReference), MoveEmitterVertex(original.b, *emitter, emitterReference), MoveEmitterVertex(original.c, *emitter, emitterReference)};
                    else HideTriangle(triangle);
                } else {
                    triangle = {MoveVertex(original.a, instance.transform), MoveVertex(original.b, instance.transform),
                                MoveVertex(original.c, instance.transform)};
                    const auto scale = instance.transform.scale;
                    const double determinant = double(scale.x) * scale.y * scale.z;
                    const bool mirrored = determinant < 0;
                    if (mirrored) {
                        // 只反转位置分量，绕序变化后 b.w 仍保留稳定属性索引。
                        const auto second = triangle.b;
                        triangle.b = {triangle.c.x, triangle.c.y, triangle.c.z, second.w};
                        triangle.c = {second.x, second.y, second.z, triangle.c.w};
                    }
                    // 法线使用逆缩放，切线使用正向缩放，以保留非均匀资产编辑。
                    if (original.b.w > 0) {
                        const auto sourceIndex = static_cast<std::size_t>(original.b.w - 1);
                        const auto& source = reference.attributes[sourceIndex];
                        auto attributeIndex = sourceIndex;
                        // 复制实例及其他实例复用的属性均生成私有三角形属性副本。
                        if (destination != first || (attributeOwners[sourceIndex] != unowned && attributeOwners[sourceIndex] != instanceIndex)) {
                            if (output.attributes.size() == maximumAttributeCount)
                                throw std::invalid_argument("instanced attribute count exceeds exact float index capacity");
                            attributeIndex = output.attributes.size();
                            output.attributes.push_back(source);
                            triangle.b.w = static_cast<float>(attributeIndex + 1);
                        } else attributeOwners[sourceIndex] = instanceIndex;
                        auto& attribute = output.attributes[attributeIndex];
                        attribute = source;
                        auto rotation = instance.transform;
                        rotation.position = {};
                        rotation.scale = {1, 1, 1};
                        if (determinant == 0) throw std::invalid_argument("mesh scale is singular");
                        auto normal = [&](const Float4& value) {
                            const auto n = Normalize(TransformPoint({value.x / scale.x, value.y / scale.y, value.z / scale.z}, rotation));
                            return Float4{n.x, n.y, n.z, value.w};
                        };
                        attribute.normal0 = normal(source.normal0);
                        attribute.normal0.w = source.normal0.w * (mirrored ? -1.0f : 1.0f);
                        attribute.normal1 = normal(mirrored ? source.normal2 : source.normal1);
                        attribute.normal2 = normal(mirrored ? source.normal1 : source.normal2);
                        if (mirrored) {
                            attribute.uv01 = {source.uv01.x, source.uv01.y, source.uv2.x, source.uv2.y};
                            attribute.uv2 = {source.uv01.z, source.uv01.w, source.uv2.z, source.uv2.w};
                        }
                        const auto t = Normalize(TransformPoint({source.tangent.x * scale.x, source.tangent.y * scale.y, source.tangent.z * scale.z}, rotation));
                        attribute.tangent = {t.x, t.y, t.z, source.tangent.w * (mirrored ? -1.0f : 1.0f)};
                    }
                }
                if (triangleOwners) owners[outputIndex] = cornell && original.a.w == static_cast<float>(emitterMaterial)
                    ? (emitter ? emitter->entity : gameplay::Entity{entt::null}) : instance.entity;
            }
            // 完整模型继承源表面材质，单材质覆盖与兼容灯板仍保持原有行为。
            triangle.a.w = cornell && reference.triangles[index].a.w == static_cast<float>(emitterMaterial)
                               ? static_cast<float>(emitterMaterial) : instance.mesh.useSurfaceMaterials
                                   ? reference.triangles[index].a.w : static_cast<float>(instance.mesh.materialIndex);
            // 共享材质不转移解析身份，标签顺序与启用光源上传顺序一致。
            triangle.c.w = cornell && emitter && instance.mesh.visible &&
                                   reference.triangles[index].a.w == static_cast<float>(emitterMaterial)
                               ? static_cast<float>(emitterLightTag) : 0.0f;
        }
    }
    if (cornell) output.materials[emitterMaterial].emissionMetallic = {0, 0, 0, 0};
    for (const auto& item : snapshot.lights) {
        if (!item.light.enabled) continue;
        const auto& light = item.light;
        auto rotation = item.transform;
        rotation.position = {};
        rotation.scale = {1, 1, 1};
        const auto direction = Normalize(TransformPoint(light.direction, rotation));
        const auto tangent = Normalize(TransformPoint(light.tangent, rotation));
        const auto bitangent = Normalize(TransformPoint(light.bitangent, rotation));
        if (Length(direction) < 0.9f || Length(tangent) < 0.9f || Length(bitangent) < 0.9f)
            throw std::invalid_argument("light axes must be nonzero");
        if (std::abs(Dot(direction, tangent)) > 0.001f || std::abs(Dot(direction, bitangent)) > 0.001f ||
            std::abs(Dot(tangent, bitangent)) > 0.001f || Dot(Cross(tangent, bitangent), direction) < 0.999f)
            throw std::invalid_argument("light axes must form an orthogonal emission frame");
        GpuLight gpu;
        gpu.positionType = {item.transform.position.x, item.transform.position.y, item.transform.position.z, static_cast<float>(light.type)};
        gpu.directionRange = {direction.x, direction.y, direction.z, light.range};
        gpu.tangentWidth = {tangent.x, tangent.y, tangent.z, light.width};
        gpu.bitangentHeight = {bitangent.x, bitangent.y, bitangent.z, light.height};
        gpu.colorIntensity = {light.color.x, light.color.y, light.color.z, light.intensity};
        constexpr float radians = 3.141592654f / 180;
        gpu.parameters = {light.sourceRadius, std::cos(light.outerAngleDegrees * radians), std::cos(light.innerAngleDegrees * radians), 0};
        if (light.type == gameplay::LightType::Rectangle || light.type == gameplay::LightType::Disk) {
            const float lightTag = static_cast<float>(output.lights.size() + 1);
            const auto materialId = cornell && &item == emitter ? emitterMaterial : static_cast<unsigned>(output.materials.size());
            gpu.shadow.z = materialId;
            GpuMaterial emissive;
            emissive.baseRoughness = {0, 0, 0, 1};
            emissive.emissionMetallic = {FiniteFloat(double(light.color.x) * light.intensity),
                FiniteFloat(double(light.color.y) * light.intensity), FiniteFloat(double(light.color.z) * light.intensity), 0};
            if (materialId < output.materials.size()) output.materials[materialId] = emissive;
            else {
                output.materials.push_back(emissive);
                output.materialNames.push_back("Area Emitter " + std::to_string(entt::to_integral(item.entity)));
            }
            if (!(cornell && &item == emitter)) {
                const auto center = item.transform.position;
                auto vertex = [materialId](Vec3 p) { return Float4{p.x, p.y, p.z, static_cast<float>(materialId)}; };
                auto triangle = [&](Vec3 a, Vec3 b, Vec3 c) {
                    if (!IsFinite(a) || !IsFinite(b) || !IsFinite(c))
                        throw std::invalid_argument("generated area emitter geometry exceeds finite float range");
                    if (Dot(Cross(b - a, c - a), direction) < 0) std::swap(b, c);
                    // 原语携带独立于材质标识的光源索引，BVH 排序后仍保持关联。
                    if (output.triangles.size() == std::numeric_limits<std::uint32_t>::max())
                        throw std::invalid_argument("generated emitter triangle count exceeds mesh range capacity");
                    output.triangles.push_back({vertex(a), {b.x, b.y, b.z, 0}, {c.x, c.y, c.z, lightTag}});
                    if (triangleOwners) owners.push_back(item.entity);
                };
                const auto x = tangent * (light.width * 0.5f), y = bitangent * (light.height * 0.5f);
                // 圆盘采样器与可见发光几何使用相同的六十四扇区多边形。
                if (light.type == gameplay::LightType::Disk) {
                    for (unsigned sector = 0; sector < 64; ++sector) {
                        const float a = sector * (2 * 3.141592654f / 64), b = (sector + 1) * (2 * 3.141592654f / 64);
                        triangle(center, center + x * std::cos(a) + y * std::sin(a), center + x * std::cos(b) + y * std::sin(b));
                    }
                } else {
                    triangle(center - x - y, center + x - y, center + x + y);
                    triangle(center - x - y, center + x + y, center - x + y);
                }
            }
        }
        output.lights.push_back(gpu);
        output.lightNames.push_back("Light " + std::to_string(entt::to_integral(item.entity)));
    }
    // 只有包含生成灯光几何的完整场景有效后，才向选择系统发布所有权映射。
    if (triangleOwners) *triangleOwners = std::move(owners);
    return output;
}

Scene ExtractCornellRenderScene(const Scene& reference, const gameplay::RenderSnapshot& snapshot) {
    return ExtractRenderScene(reference, snapshot, true);
}
}
