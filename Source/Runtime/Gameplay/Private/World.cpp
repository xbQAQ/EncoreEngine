#include "gameplay/World.h"
#include "render/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace gameplay {
Entity World::CreateEntity(std::string name) {
    auto entity = registry_.create();
    // 文档替换使用独立版本区间，防止外部选择句柄误指向新场景同索引对象。
    if (entityVersionSeed_ && entt::to_version(entity) == 0) {
        const auto index = entt::to_entity(entity);
        registry_.destroy(entity);
        entity = registry_.create(entt::entt_traits<Entity>::construct(index,
            static_cast<entt::entt_traits<Entity>::version_type>(entityVersionSeed_)));
    }
    // 作者实体总有名称与默认变换，保证编辑器检查入口一致。
    try {
        registry_.emplace<Name>(entity, std::move(name));
        registry_.emplace<Transform>(entity);
        if (!activeSceneId_.empty()) {
            std::string id;
            bool found;
            do {
                id = document_.id() + ".object." + std::to_string(++nextObjectId_);
                found = false;
                for (const auto& level : document_.levels()) for (const auto& scene : level.scenes())
                    for (const auto& object : scene.objects()) if (object.id() == id) found = true;
            } while (found);
            registry_.emplace<ObjectIdentity>(entity, activeLevelId_, activeSceneId_, std::move(id));
        }
    } catch (...) {
        registry_.destroy(entity);
        throw;
    }
    MarkDirty();
    return entity;
}

bool World::DestroyEntity(Entity entity) {
    if (!registry_.valid(entity)) return false;
    registry_.destroy(entity);
    MarkDirty();
    return true;
}

void World::CreateCornell(const render::Scene& scene) {
    // 历史入口允许调用者先原地构建 BVH；重排后按三角形当前材质重新生成连续表面。
    if (!scene.meshParts.empty()) {
        bool coherent = true;
        std::vector<bool> covered(scene.triangles.size());
        for (const auto& part : scene.meshParts) {
            const std::size_t first = part.firstTriangle, count = part.triangleCount;
            if (!count || first > scene.triangles.size() || count > scene.triangles.size() - first) {
                coherent = false;
                break;
            }
            for (std::size_t index = first; index < first + count; ++index) {
                if (covered[index] || (!part.useSurfaceMaterials && scene.triangles[index].a.w != static_cast<float>(part.materialIndex))) coherent = false;
                covered[index] = true;
            }
        }
        if (!coherent) {
            auto reordered = scene;
            reordered.meshParts.clear();
            CreateScene(reordered, true);
            return;
        }
    }
    CreateScene(scene, true);
}

void World::CreateScene(const render::Scene& scene, bool cornell) {
    if (scene.triangles.empty() || scene.materials.empty()) throw std::invalid_argument("scene must contain geometry and materials");
    if (scene.triangles.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Cornell world triangle count exceeds mesh index capacity");
    // 作者摄像机也在资源替换前验证，避免新增传输字段绕过原子载入边界。
    for (const auto& camera : scene.cameras)
        if (!render::IsFinite(camera.position) || !render::IsFinite(camera.rotationDegrees) ||
            !std::isfinite(camera.verticalFovDegrees) || !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
            camera.verticalFovDegrees <= 0 || camera.verticalFovDegrees >= 180 || camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane)
            throw std::invalid_argument("场景摄像机的姿态或投影参数无效");
    // 替换前先验证输入，错误数据不能破坏现有实体。
    for (const auto& triangle : scene.triangles) {
        const float material = triangle.a.w;
        if (!std::isfinite(material) || material < 0 || std::floor(material) != material ||
            static_cast<double>(material) >= static_cast<double>(scene.materials.size()))
            throw std::invalid_argument("Cornell world triangle references an invalid material");
        for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) {
            if (!render::IsFinite({vertex.x, vertex.y, vertex.z}))
                throw std::invalid_argument("Cornell world contains a nonfinite vertex");
        }
    }
    // 验证全部作者范围，失败导入不会部分替换世界。
    if (!scene.meshParts.empty()) {
        std::vector<bool> covered(scene.triangles.size());
        for (const auto& part : scene.meshParts) {
            const size_t first = part.firstTriangle, count = part.triangleCount;
            if (count == 0 || first > scene.triangles.size() || count > scene.triangles.size() - first || part.materialIndex >= scene.materials.size())
                throw std::invalid_argument("scene mesh primitive range or material is invalid");
            for (size_t index = first; index < first + count; ++index) {
                if (covered[index] || (!part.useSurfaceMaterials && scene.triangles[index].a.w != static_cast<float>(part.materialIndex)))
                    throw std::invalid_argument("scene mesh primitives overlap or disagree with their authored material");
                covered[index] = true;
            }
        }
        if (std::find(covered.begin(), covered.end(), false) != covered.end()) throw std::invalid_argument("scene mesh primitive ranges leave unowned geometry");
    }
    // 同一解码图像可被多个 UV 或采样器视图复用，先创建完整候选资源。
    engine::ResourceStore nextResources;
    std::vector<render::TextureBinding> nextTextureBindings;
    nextTextureBindings.reserve(scene.textureViews.size());
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, engine::ResourceHandle<render::TextureResource>> images;
    const auto finite = [](const render::Float4& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    };
    for (const auto& view : scene.textureViews) {
        const std::uint64_t count = std::uint64_t(view.image.y) * view.image.z;
        if (!count || std::uint64_t(view.image.x) + count > scene.packedTexels.size() ||
            !finite(view.offsetScale) || !finite(view.rotation) || view.rotation.z != 0 ||
            (view.image.w & 3u) > 2u || ((view.image.w >> 2) & 3u) > 2u || (view.image.w >> 4) != 0)
            throw std::invalid_argument("scene resource texture view has an invalid image, sampler or UV transform");
        const auto key = std::tuple{view.image.x, view.image.y, view.image.z};
        auto found = images.find(key);
        if (found == images.end()) {
            const auto pixels = std::span(scene.packedTexels).subspan(view.image.x, static_cast<std::size_t>(count));
            const auto handle = nextResources.Create<render::TextureResource>("图像 " + std::to_string(images.size()),
                view.image.y, view.image.z, std::vector<std::uint32_t>(pixels.begin(), pixels.end()));
            found = images.emplace(key, handle).first;
        }
        nextTextureBindings.push_back({found->second, view});
    }

    // 材质资源保存唯一可编辑参数，兼容表仅提供投影视图。
    std::vector<engine::ResourceHandle<render::MaterialResource>> nextMaterialHandles;
    std::vector<render::MaterialResource*> nextMaterials;
    nextMaterialHandles.reserve(scene.materials.size());
    nextMaterials.reserve(scene.materials.size());
    for (std::size_t index = 0; index < scene.materials.size(); ++index) {
        const auto& parameters = scene.materials[index];
        for (const auto slot : {parameters.textures.x, parameters.textures.y, parameters.textures.z, parameters.textures.w,
                               parameters.textures2.x, parameters.textures2.y, parameters.textures2.z, parameters.textures2.w})
            if (slot != ~0u && slot >= nextTextureBindings.size())
                throw std::invalid_argument("scene resource material references an invalid texture view");
        const auto handle = nextResources.Create<render::MaterialResource>(
            index < scene.materialNames.size() ? scene.materialNames[index] : "材质 " + std::to_string(index),
            parameters, std::span<const render::TextureBinding>(nextTextureBindings));
        nextMaterialHandles.push_back(handle);
        nextMaterials.push_back(nextResources.Resolve(handle));
    }

    // 导入资产烘焙为多表面网格，保留共享属性与稳定原语身份。
    auto surfaces = scene.meshParts;
    if (surfaces.empty()) {
        std::uint32_t first = 0;
        while (static_cast<std::size_t>(first) < scene.triangles.size()) {
            const auto material = static_cast<std::uint32_t>(scene.triangles[first].a.w);
            std::uint32_t count = 1;
            while (static_cast<std::size_t>(first) + count < scene.triangles.size() &&
                   scene.triangles[static_cast<std::size_t>(first) + count].a.w == static_cast<float>(material)) ++count;
            surfaces.push_back({"网格 " + std::to_string(first) + " / 材质 " + std::to_string(material), first, count, material});
            first += count;
        }
    }
    const auto nextMesh = nextResources.Create<render::MeshResource>(scene.sourcePath.empty() ? "场景网格" : scene.sourcePath,
        scene.triangles, scene.attributes, surfaces);

    // 移动所有者保持资源地址和不可变纹理表的底层分配稳定。
    registry_.clear();
    resources_ = std::move(nextResources);
    materials_ = render::MaterialParameters(std::move(nextMaterials));
    textureBindings_ = std::move(nextTextureBindings);
    materialHandles_ = std::move(nextMaterialHandles);
    meshHandle_ = nextMesh;
    sourceLights_ = scene.lights;
    sourceLightNames_ = scene.lightNames;
    sourcePath_ = scene.sourcePath;
    // 烘焙必须保留建立摄像机和发光体约定时使用的显式模式。
    cornell_ = cornell;
    document_.Clear();
    activeSceneId_.clear();
    activeLevelId_.clear();
    materialIds_ = scene.materialIds;
    materialObjectIds_ = scene.materialObjectIds;
    textureIds_ = scene.textureIds;
    // 替换基础场景时旧模板与范围映射不再属于当前资源所有者。
    objectPresets_.clear();
    objectSources_.clear();
    presetMaterialDefinitions_.clear();
    cameraEntity_ = entt::null;
    lightEntity_ = entt::null;
    MarkDirty();

    cameraEntity_ = CreateEntity(scene.cameras.empty() ? "主摄像机" : scene.cameras.front().name);
    registry_.emplace<Camera>(cameraEntity_);
    render::Aabb bounds;
    for (const auto& triangle : scene.triangles)
        for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) bounds.Expand({vertex.x, vertex.y, vertex.z});
    const auto center = (bounds.minimum + bounds.maximum) * 0.5f;
    const float extent = std::max(render::Length(bounds.maximum - bounds.minimum), 1.0f);
    // 优先消费作者摄像机；没有摄像机数据时按任意资产包围盒取景。
    if (!scene.cameras.empty()) {
        const auto& source = scene.cameras.front();
        auto& transform = registry_.get<Transform>(cameraEntity_);
        transform.position = source.position;
        transform.rotationDegrees = source.rotationDegrees;
        registry_.replace<Camera>(cameraEntity_, source.verticalFovDegrees, source.nearPlane, source.farPlane, source.active);
    } else {
        registry_.get<Transform>(cameraEntity_).position = center - Vec3{0, 0, extent * 1.3f};
        registry_.get<Camera>(cameraEntity_).farPlane = std::max(100.0f, extent * 5);
    }
    for (std::size_t index = 0; index < scene.lights.size(); ++index) {
        const auto& source = scene.lights[index];
        const auto entity = CreateEntity(index < scene.lightNames.size() ? scene.lightNames[index] : "导入光源");
        registry_.get<Transform>(entity).position = {source.positionType.x, source.positionType.y, source.positionType.z};
        auto& light = registry_.emplace<Light>(entity);
        light.type = static_cast<LightType>(static_cast<unsigned>(source.positionType.w));
        light.color = {source.colorIntensity.x, source.colorIntensity.y, source.colorIntensity.z};
        light.intensity = source.colorIntensity.w;
        light.range = source.directionRange.w;
        light.direction = {source.directionRange.x, source.directionRange.y, source.directionRange.z};
        light.tangent = {source.tangentWidth.x, source.tangentWidth.y, source.tangentWidth.z};
        light.bitangent = {source.bitangentHeight.x, source.bitangentHeight.y, source.bitangentHeight.z};
        // 非面光的零尺寸表示未使用的 GPU 通道；已有作者尺寸和面光的严格输入约定继续保留。
        const bool areaLight = light.type == LightType::Rectangle || light.type == LightType::Disk;
        if (areaLight || source.tangentWidth.w != 0) light.width = source.tangentWidth.w;
        if (areaLight || source.bitangentHeight.w != 0) light.height = source.bitangentHeight.w;
        light.sourceRadius = source.parameters.x;
        light.outerAngleDegrees = std::acos(std::clamp(source.parameters.y, -1.0f, 1.0f)) * 180.0f / 3.141592654f;
        light.innerAngleDegrees = std::acos(std::clamp(source.parameters.z, -1.0f, 1.0f)) * 180.0f / 3.141592654f;
        light.emitterMaterial = source.shadow.z;
        if (lightEntity_ == entt::null) lightEntity_ = entity;
    }
    if (scene.lights.empty()) {
        lightEntity_ = CreateEntity("默认面光源");
        auto& pose = registry_.get<Transform>(lightEntity_);
        pose.position = center + Vec3{0, extent * 0.7f, -extent * 0.3f};
        auto& light = registry_.emplace<Light>(lightEntity_);
        light.width = light.height = extent * 0.3f;
    }

    // 源范围确定完整模型及发光体身份，多材质模型只创建一个可选择的实体。
    for (const auto& part : surfaces) {
        const auto entity = CreateEntity(part.name);
        registry_.emplace<MeshRenderer>(entity, part.firstTriangle, part.triangleCount, part.materialIndex, true,
            cornell && part.materialIndex == 3, meshHandle_, part.useSurfaceMaterials);
    }
}

render::Scene World::CookResourceScene() const {
    const auto* mesh = resources_.Resolve(meshHandle_);
    if (!mesh) throw std::logic_error("world has no owned mesh resource to cook");
    render::Scene scene;
    // 渲染器一次接收传输副本，避免逐帧借用依赖所有者寿命的指针。
    scene.triangles.assign(mesh->Triangles().begin(), mesh->Triangles().end());
    scene.attributes.assign(mesh->Attributes().begin(), mesh->Attributes().end());
    scene.meshParts.assign(mesh->Surfaces().begin(), mesh->Surfaces().end());
    scene.sourcePath = sourcePath_;
    scene.cornell = cornell_;
    scene.lights = sourceLights_;
    scene.lightNames = sourceLightNames_;
    scene.materialIds = materialIds_;
    scene.materialObjectIds = materialObjectIds_;
    scene.textureIds = textureIds_;
    // 摄像机记录由当前组件提取，烘焙重建不再依赖任何内置房间尺寸。
    for (auto [entity, transform, camera] : registry_.view<const Transform, const Camera>().each()) {
        const auto* name = registry_.try_get<Name>(entity);
        scene.cameras.push_back({name ? name->value : "摄像机", transform.position, transform.rotationDegrees,
            camera.verticalFovDegrees, camera.nearPlane, camera.farPlane, camera.active});
    }
    scene.materials.reserve(materialHandles_.size());
    scene.materialNames.reserve(materialHandles_.size());
    for (const auto handle : materialHandles_) {
        const auto* material = resources_.Resolve(handle);
        if (!material) throw std::logic_error("world material owner is inconsistent");
        scene.materials.push_back(material->Parameters());
        scene.materialNames.push_back(material->Name());
    }

    // 每张图像仅打包一次，同时保持各视图槽、采样器和 UV 变换不变。
    std::map<std::uint64_t, std::uint32_t> imageOffsets;
    scene.textureViews.reserve(textureBindings_.size());
    for (const auto& binding : textureBindings_) {
        const auto* texture = resources_.Resolve(binding.texture);
        if (!texture) throw std::logic_error("world texture owner is inconsistent");
        auto found = imageOffsets.find(binding.texture.value);
        if (found == imageOffsets.end()) {
            if (scene.packedTexels.size() + texture->Texels().size() > std::numeric_limits<std::uint32_t>::max())
                throw std::length_error("resource texture packing exceeds GPU offset capacity");
            const auto offset = static_cast<std::uint32_t>(scene.packedTexels.size());
            scene.packedTexels.insert(scene.packedTexels.end(), texture->Texels().begin(), texture->Texels().end());
            found = imageOffsets.emplace(binding.texture.value, offset).first;
        }
        auto view = binding.view;
        view.image = {found->second, texture->Width(), texture->Height(), view.image.w};
        scene.textureViews.push_back(view);
    }
    return scene;
}

void World::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0)
        throw std::invalid_argument("world timestep must be finite and nonnegative");
    if (deltaSeconds == 0) return;
    std::vector<std::pair<Entity, Vec3>> moved;
    // 先计算全部运动结果再提交，错误输入不能部分推进模拟。
    for (auto [entity, transform, velocity] : registry_.view<Transform, Velocity>().each()) {
        if (!render::IsFinite(transform.position) || !render::IsFinite(velocity.linear))
            throw std::invalid_argument("world motion requires finite position and velocity");
        const Vec3 next = transform.position + velocity.linear * deltaSeconds;
        if (!render::IsFinite(next)) throw std::overflow_error("world motion produced a nonfinite position");
        if (next.x != transform.position.x || next.y != transform.position.y || next.z != transform.position.z)
            moved.emplace_back(entity, next);
    }
    for (const auto& [entity, position] : moved) registry_.get<Transform>(entity).position = position;
    if (!moved.empty()) MarkDirty();
}

RenderSnapshot World::ExtractRenderSnapshot() const {
    RenderSnapshot snapshot;
    snapshot.revision = revision_;
    // 快照不受随后对资源材质参数的编辑影响。
    snapshot.materials.assign(materials_.begin(), materials_.end());
    // 仅复制渲染器消费的组件，World 不持有 GPU 资源或命令。
    for (auto [entity, transform, mesh] : registry_.view<const Transform, const MeshRenderer>().each())
        if (mesh.visible && mesh.triangleCount > 0) snapshot.instances.push_back({entity, transform, mesh});
    for (auto [entity, transform, camera] : registry_.view<const Transform, const Camera>().each())
        if (camera.active) snapshot.cameras.push_back({entity, transform, camera});
    for (auto [entity, transform, light] : registry_.view<const Transform, const Light>().each())
        if (light.enabled) snapshot.lights.push_back({entity, transform, light});
    const auto entityOrder = [](const auto& a, const auto& b) { return entt::to_integral(a.entity) < entt::to_integral(b.entity); };
    std::sort(snapshot.instances.begin(), snapshot.instances.end(), entityOrder);
    std::sort(snapshot.cameras.begin(), snapshot.cameras.end(), entityOrder);
    std::sort(snapshot.lights.begin(), snapshot.lights.end(), entityOrder);
    return snapshot;
}

Entity World::CameraEntity() const {
    return registry_.valid(cameraEntity_) && registry_.all_of<Transform, Camera>(cameraEntity_) ? cameraEntity_ : entt::null;
}

Entity World::LightEntity() const {
    return registry_.valid(lightEntity_) && registry_.all_of<Transform, Light>(lightEntity_) ? lightEntity_ : entt::null;
}
}
