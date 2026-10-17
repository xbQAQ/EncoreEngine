#pragma once

#include "gameplay/Components.h"
#include "render/Resources.h"
#include "render/WorldDocument.h"

#include <entt/entt.hpp>
#include <cstdint>
#include <string>

namespace render { struct Scene; }

namespace gameplay {
struct ObjectIdentity {
    std::string levelId; // 所属关卡的稳定文档标识。
    std::string sceneId; // 所属场景的稳定文档标识。
    std::string objectId; // 对象的稳定文档标识，独立于运行时 EnTT 句柄。
    bool operator==(const ObjectIdentity&) const = default;
};
// 预设仅引用追加的共享源范围，放置时不再次复制不可变几何。
struct ObjectPreset {
    render::config::Object object; // 配置提供的作者默认值及资源引用。
    std::uint32_t firstTriangle{}; // 聚合网格中预加载模板的源三角形起点。
    std::uint32_t triangleCount{}; // 模板拥有的连续源三角形数量，非网格预设为零。
    std::uint32_t materialIndex{}; // 模板实例共享的运行时材质槽。
};
class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = default;
    World& operator=(World&&) = default;
    Entity CreateEntity(std::string name);
    bool DestroyEntity(Entity entity);
    // 源网格范围保持不可变，渲染器在独立副本中构建与排序 BVH。
    void CreateCornell(const render::Scene& scene);
    void CreateScene(const render::Scene& scene, bool cornell = false);
    // 模型追加到当前场景，保留已有对象、相机与作者编辑，失败时不替换当前世界。
    void AppendScene(const render::Scene& scene);
    void CreateDocument(const render::WorldDocument& document, std::string_view sceneId = {});
    render::WorldDocument ExportDocument() const;
    // 必须在渲染器首次烘焙前安装；模板不会创建可见实体或启用灯光。
    void InstallObjectPresets(const render::WorldDocument& library);
    const std::vector<ObjectPreset>& ObjectPresets() const { return objectPresets_; }
    // 调用者合并模板默认值与放置计算后传入最终作者姿态，此处不重复应用模板变换。
    Entity CreatePresetObject(std::size_t presetIndex, const Transform& transform);
    void SwitchScene(std::string_view sceneId);
    const render::WorldDocument& Document() const { return document_; }
    const std::string& ActiveSceneId() const { return activeSceneId_; }
    const std::string& ActiveLevelId() const { return activeLevelId_; }
    void Update(float deltaSeconds);
    RenderSnapshot ExtractRenderSnapshot() const;
    std::uint64_t Revision() const { return revision_; }
    void MarkDirty() { ++revision_; }
    Entity CameraEntity() const;
    Entity LightEntity() const;
    // 直接编辑组件后调用 MarkDirty，使依赖历史的渲染器观察到变更。
    entt::registry& Registry() { return registry_; }
    const entt::registry& Registry() const { return registry_; }
    render::MaterialParameters& Materials() { return materials_; }
    const render::MaterialParameters& Materials() const { return materials_; }
    // 资源在导入后一次烘焙，不将不可变几何复制到逐帧快照。
    render::Scene CookResourceScene() const;
    const engine::ResourceStore& Resources() const { return resources_; }
    // 编辑器借用类型安全的材质资源，所有权仍保留在 World。
    render::MaterialResource* Material(std::size_t index) { return resources_.Resolve(MaterialHandle(index)); }
    const render::MaterialResource* Material(std::size_t index) const { return resources_.Resolve(MaterialHandle(index)); }
    std::vector<render::MaterialShaderSource> MaterialScripts() const {
        std::vector<render::MaterialShaderSource> sources;
        for (std::size_t index = 0; index < materials_.size(); ++index) {
            const auto* resource = Material(index);
            if (resource && !resource->ScriptSource().empty())
                sources.push_back({static_cast<std::uint32_t>(index), resource->ScriptSource(), resource->ScriptPath()});
        }
        return sources;
    }
    engine::ResourceHandle<render::MaterialResource> MaterialHandle(std::size_t index) const {
        return index < materialHandles_.size() ? materialHandles_[index] : engine::ResourceHandle<render::MaterialResource>{};
    }

private:
    render::WorldDocument document_; // 世界、关卡、场景和共享资源的作者数据。
    std::string activeSceneId_; // 当前实例化到 ECS 的场景标识。
    std::string activeLevelId_; // 当前场景所属关卡标识。
    std::vector<std::string> materialIds_; // 运行时材质槽到共享目录的映射。
    std::vector<std::string> materialObjectIds_; // 运行时对象专属材质变体的归属。
    std::vector<std::string> textureIds_; // 采样视图到作者图像标识的稳定映射。
    std::vector<ObjectPreset> objectPresets_; // 编辑器可放置的配置模板及其预加载源范围。
    std::vector<ObjectPreset> objectSources_; // 独立于实体寿命的作者源范围映射，供复制和撤销后导出。
    std::vector<render::config::MaterialAsset> presetMaterialDefinitions_; // 本次安装的库材质基准，用于区分作者编辑和外部库定义冲突。
    std::uint32_t entityVersionSeed_{}; // 文档替换时隔离旧实体句柄的版本种子。
    std::uint64_t nextObjectId_{}; // 新建对象的稳定标识序列。
    entt::registry registry_; // 版本化实体句柄与组件存储的唯一所有者。
    engine::ResourceStore resources_; // 独立于实体寿命的 CPU 资源显式所有者。
    render::MaterialParameters materials_; // 面向属性面板的权威材质参数兼容视图。
    std::vector<render::TextureBinding> textureBindings_; // 借用 resources_ 图像的不可变采样视图。
    std::vector<engine::ResourceHandle<render::MaterialResource>> materialHandles_; // GPU 材质槽到资源标识的映射。
    engine::ResourceHandle<render::MeshResource> meshHandle_; // 所有源范围共享的烘焙多表面几何。
    std::vector<render::GpuLight> sourceLights_; // 初次烘焙保留的导入解析光源记录。
    std::vector<std::string> sourceLightNames_; // 与源解析光源对应的作者名称。
    std::string sourcePath_; // 导入来源路径，不额外保存一份 Scene 几何。
    bool cornell_{}; // 兼容历史发光体约定的显式 Cornell 标识。
    std::uint64_t revision_{}; // 结构变更、显式编辑或实际运动后递增。
    Entity cameraEntity_{entt::null}; // 主要摄像机句柄，访问时检查有效性。
    Entity lightEntity_{entt::null}; // 主要光源句柄，访问时检查有效性。
};
}
