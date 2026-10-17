#include "gameplay/World.h"

#include <google/protobuf/util/message_differencer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace gameplay {
namespace {
bool SameMaterial(const render::config::MaterialAsset& first, const render::config::MaterialAsset& second) {
    // 运行时补全的材质默认参数与原始简写配置视为相同定义。
    auto a = first, b = second;
    render::WriteMaterialParameters(render::ReadMaterialParameters(first.parameters()), *a.mutable_parameters());
    render::WriteMaterialParameters(render::ReadMaterialParameters(second.parameters()), *b.mutable_parameters());
    return google::protobuf::util::MessageDifferencer::Equivalent(a, b);
}

template<class Records, class Equal> void MergeCatalog(Records* output, const Records& source, Equal equal) {
    // 同标识只接受等价定义，冲突在任何运行时资源变更之前报告。
    for (const auto& item : source) {
        const auto found = std::find_if(output->begin(), output->end(), [&](const auto& value) { return value.id() == item.id(); });
        if (found == output->end()) *output->Add() = item;
        else if (!equal(*found, item)) throw std::invalid_argument("对象预设资源标识与当前世界定义冲突：" + item.id());
    }
}

void ValidatePlacement(const Transform& placement) {
    // 编辑器已计算预览与支撑偏移；创建接口只验证最终姿态，避免默认值被叠加两次。
    if (!render::IsFinite(placement.position) || !render::IsFinite(placement.rotationDegrees) || !render::IsFinite(placement.scale) ||
        placement.scale.x == 0 || placement.scale.y == 0 || placement.scale.z == 0)
        throw std::invalid_argument("对象放置必须使用有限姿态和非零缩放");
}
} // 匿名命名空间

void World::InstallObjectPresets(const render::WorldDocument& library) {
    render::ValidateWorldDocument(library);
    const auto& authored = render::FindDocumentScene(library, {});
    // 每个模板独立创建一个实体；跨对象发光体和纹理视图需要额外的组合资产边界。
    for (const auto& object : authored.objects()) {
        if (!object.texture_bindings().empty()) throw std::invalid_argument("对象预设暂不支持纹理视图绑定");
        // 组合预设尚未安装私有纹理与表面变体，不能让新模型格式绕过既有资源边界。
        for (const auto& surface : object.surface_materials())
            if (!surface.texture_bindings().empty() || surface.has_material_override() ||
                surface.has_material_script_source() || surface.has_material_script_path())
                throw std::invalid_argument("对象预设暂不支持表面私有纹理、参数或脚本覆盖");
        if (object.has_light() && !render::config::Light_Type_IsValid(object.light().type()))
            throw std::invalid_argument("对象预设包含未知灯光类型");
        if (object.has_light() && !object.light().emitter_object_id().empty())
            throw std::invalid_argument("对象预设不能引用另一个模板作为发光体");
    }

    // 兼容导入入口先在临时世界提升为文档，只有完整安装成功才替换旧状态。
    if (document_.levels().empty()) {
        World normalized;
        normalized.CreateDocument(ExportDocument());
        normalized.InstallObjectPresets(library);
        normalized.revision_ = revision_ + 1;
        *this = std::move(normalized);
        return;
    }
    auto merged = ExportDocument();
    const auto equivalent = [](const auto& a, const auto& b) { return google::protobuf::util::MessageDifferencer::Equivalent(a, b); };
    MergeCatalog(merged.mutable_meshes(), library.meshes(), equivalent);
    MergeCatalog(merged.mutable_textures(), library.textures(), equivalent);
    // 运行中保留库基准，外部库更改不能伪装为用户对当前材质的编辑。
    if (!objectPresets_.empty()) {
        if (presetMaterialDefinitions_.size() != static_cast<std::size_t>(library.materials_size()))
            throw std::invalid_argument("当前世界已经安装不同的对象预设材质库");
        for (const auto& baseline : presetMaterialDefinitions_) {
            const auto match = std::find_if(library.materials().begin(), library.materials().end(), [&](const auto& item) { return item.id() == baseline.id(); });
            if (match == library.materials().end() || !SameMaterial(baseline, *match))
                throw std::invalid_argument("对象预设材质库定义已改变：" + baseline.id());
        }
    }
    std::set<std::string> editableMaterials;
    for (const auto& preset : objectPresets_)
        if (!preset.object.mesh_id().empty()) editableMaterials.insert(preset.object.material_id());
    // 删除全部实例后仍保留预设目录；只凭安装前已存在且等价的保留命名空间网格确认归属。
    for (const auto& preset : authored.objects()) {
        if (!preset.mesh_id().starts_with("editor.preset.") || !preset.material_id().starts_with("editor.preset.")) continue;
        const auto existing = std::find_if(document_.meshes().begin(), document_.meshes().end(),
            [&](const auto& mesh) { return mesh.id() == preset.mesh_id(); });
        const auto definition = std::find_if(library.meshes().begin(), library.meshes().end(),
            [&](const auto& mesh) { return mesh.id() == preset.mesh_id(); });
        if (existing != document_.meshes().end() && definition != library.meshes().end() && equivalent(*existing, *definition))
            editableMaterials.insert(preset.material_id());
    }
    // 重开文档没有运行时库基准，以已有对象对匹配预设网格的引用确认材质的作者归属。
    for (const auto& level : merged.levels()) for (const auto& scene : level.scenes()) for (const auto& object : scene.objects())
        for (const auto& preset : authored.objects())
            if (!preset.mesh_id().empty() && object.mesh_id() == preset.mesh_id() && object.material_id() == preset.material_id())
                editableMaterials.insert(object.material_id());
    MergeCatalog(merged.mutable_materials(), library.materials(), [&](const auto& currentMaterial, const auto& definition) {
        return SameMaterial(currentMaterial, definition) ||
            (editableMaterials.contains(currentMaterial.id()) && currentMaterial.name() == definition.name());
    });
    render::ValidateWorldDocument(merged);
    if (!objectPresets_.empty()) {
        if (objectPresets_.size() != static_cast<std::size_t>(authored.objects_size()))
            throw std::invalid_argument("当前世界已经安装不同的对象预设库");
        for (std::size_t index = 0; index < objectPresets_.size(); ++index)
            if (!equivalent(objectPresets_[index].object, authored.objects(static_cast<int>(index))))
                throw std::invalid_argument("当前世界已经安装不同的对象预设库");
        return;
    }

    // 临时烘焙只生成模板源数据，解析灯光与模板实体均不进入当前场景。
    auto bakingLibrary = library;
    for (auto& material : *bakingLibrary.mutable_materials())
        for (const auto& currentMaterial : merged.materials())
            if (material.id() == currentMaterial.id()) { material = currentMaterial; break; }
    const auto baked = render::BakeScene(bakingLibrary, authored.id(), false);
    std::vector<render::config::MaterialAsset> materialDefinitions(library.materials().begin(), library.materials().end());
    const auto* current = resources_.Resolve(meshHandle_);
    if (!current) throw std::logic_error("对象预设安装需要已创建的世界网格所有者");
    if (baked.triangles.size() > std::numeric_limits<std::uint32_t>::max() - current->Triangles().size() ||
        baked.attributes.size() + current->Attributes().size() >= (1u << 24))
        throw std::length_error("对象预设源几何超过索引容量");
    std::vector<render::GpuTriangle> triangles(current->Triangles().begin(), current->Triangles().end());
    std::vector<render::GpuTriangleAttributes> attributes(current->Attributes().begin(), current->Attributes().end());
    std::vector<render::SceneMeshPart> surfaces(current->Surfaces().begin(), current->Surfaces().end());
    const auto triangleOffset = static_cast<std::uint32_t>(triangles.size());
    const auto attributeOffset = static_cast<float>(attributes.size());
    attributes.insert(attributes.end(), baked.attributes.begin(), baked.attributes.end());
    auto handles = materialHandles_;
    auto materialIds = materialIds_;
    auto materialOwners = materialObjectIds_;
    materialOwners.resize(handles.size());
    std::vector<render::MaterialResource*> materials;
    materials.reserve(handles.size() + baked.materials.size());
    for (const auto handle : handles) materials.push_back(resources_.Resolve(handle));
    std::vector<std::uint32_t> materialMap;
    materialMap.reserve(baked.materials.size());
    std::vector<ObjectPreset> presets;
    presets.reserve(static_cast<std::size_t>(authored.objects_size()));
    auto sources = objectSources_;
    sources.reserve(sources.size() + baked.meshParts.size());
    std::vector<engine::ResourceHandle<render::MaterialResource>> createdMaterials;
    createdMaterials.reserve(baked.materials.size());
    engine::ResourceHandle<render::MeshResource> mesh;
    try {
        // 共享目录材质复用旧槽；对象覆盖材质只烘焙一次并由所有放置实例共享。
        for (std::size_t index = 0; index < baked.materials.size(); ++index) {
            std::size_t slot = handles.size();
            if (baked.materialObjectIds[index].empty()) {
                for (std::size_t existing = 0; existing < materialIds.size(); ++existing)
                    if (materialIds[existing] == baked.materialIds[index] && materialOwners[existing].empty()) { slot = existing; break; }
            } else {
                // 多个已保存实例可拥有等价的覆盖槽；只有相同编辑结果才可作为新放置的共享默认值。
                const auto prototype = std::find_if(authored.objects().begin(), authored.objects().end(),
                    [&](const auto& value) { return value.id() == baked.materialObjectIds[index]; });
                for (std::size_t existing = 0; existing < handles.size(); ++existing) {
                    if (materialOwners[existing].empty() || prototype == authored.objects().end()) continue;
                    const auto source = std::find_if(objectSources_.begin(), objectSources_.end(), [&](const auto& value) {
                        const auto& object = value.object;
                        return object.id() == materialOwners[existing] && object.mesh_id() == prototype->mesh_id() &&
                            object.material_id() == prototype->material_id() && object.first_triangle() == prototype->first_triangle() &&
                            object.triangle_count() == prototype->triangle_count();
                    });
                    if (source == objectSources_.end()) continue;
                    if (slot != handles.size()) {
                        render::config::MaterialParameters first, second;
                        render::WriteMaterialParameters(materials[slot]->Parameters(), first);
                        render::WriteMaterialParameters(materials[existing]->Parameters(), second);
                        if (!equivalent(first, second) || materials[slot]->ScriptSource() != materials[existing]->ScriptSource() ||
                            materials[slot]->ScriptPath() != materials[existing]->ScriptPath())
                            throw std::invalid_argument("已放置预设的覆盖材质存在不同编辑，无法确定共享默认值");
                    } else slot = existing;
                }
            }
            if (slot == handles.size()) {
                const auto handle = resources_.Create<render::MaterialResource>(baked.materialNames[index], baked.materials[index]);
                createdMaterials.push_back(handle);
                auto* material = resources_.Resolve(handle);
                for (const auto& definition : bakingLibrary.materials())
                    if (definition.id() == baked.materialIds[index]) {
                        material->ScriptSource() = definition.script_source();
                        material->ScriptPath() = definition.script_path();
                    }
                if (!baked.materialObjectIds[index].empty())
                    for (const auto& object : authored.objects())
                        if (object.id() == baked.materialObjectIds[index]) {
                            if (object.has_material_script_source()) material->ScriptSource() = object.material_script_source();
                            if (object.has_material_script_path()) material->ScriptPath() = object.material_script_path();
                        }
                handles.push_back(handle); materials.push_back(material);
                materialIds.push_back(baked.materialIds[index]); materialOwners.push_back(baked.materialObjectIds[index]);
            }
            materialMap.push_back(static_cast<std::uint32_t>(slot));
        }

        // 几何和属性都只追加，旧组件与撤销快照中的源索引继续有效。
        for (auto triangle : baked.triangles) {
            triangle.a.w = static_cast<float>(materialMap.at(static_cast<std::size_t>(triangle.a.w)));
            if (triangle.b.w > 0) triangle.b.w += attributeOffset;
            triangle.c.w = 0;
            triangles.push_back(triangle);
        }
        for (auto part : baked.meshParts) {
            part.firstTriangle += triangleOffset;
            part.materialIndex = materialMap.at(part.materialIndex);
            surfaces.push_back(part);
        }
        for (const auto& object : authored.objects()) {
            ObjectPreset preset; preset.object = object;
            if (!object.mesh_id().empty()) {
                const auto part = std::find_if(baked.meshParts.begin(), baked.meshParts.end(), [&](const auto& value) { return value.objectId == object.id(); });
                if (part == baked.meshParts.end()) throw std::logic_error("对象预设缺少烘焙源范围");
                preset.firstTriangle = part->firstTriangle + triangleOffset; preset.triangleCount = part->triangleCount;
                preset.materialIndex = materialMap.at(part->materialIndex);
                sources.push_back(preset);
            }
            presets.push_back(std::move(preset));
        }
        mesh = resources_.Create<render::MeshResource>(current->Name(), std::move(triangles), std::move(attributes), std::move(surfaces));
    } catch (...) {
        // 候选资源失败时回收本次分配，既有句柄与可见世界保持完整。
        for (const auto handle : createdMaterials) resources_.Erase(handle);
        if (mesh) resources_.Erase(mesh);
        throw;
    }

    // 发布新聚合资源时保留旧资源所有权，因此既有组件和历史中的句柄仍然可解析。
    document_.Swap(&merged);
    meshHandle_ = mesh;
    materialHandles_ = std::move(handles); materials_ = render::MaterialParameters(std::move(materials));
    materialIds_ = std::move(materialIds); materialObjectIds_ = std::move(materialOwners);
    objectPresets_ = std::move(presets); objectSources_ = std::move(sources);
    presetMaterialDefinitions_ = std::move(materialDefinitions);
    MarkDirty();
}

Entity World::CreatePresetObject(std::size_t presetIndex, const Transform& transform) {
    if (presetIndex >= objectPresets_.size()) throw std::out_of_range("对象预设索引不存在");
    const auto& preset = objectPresets_[presetIndex];
    const auto& object = preset.object;
    ValidatePlacement(transform);
    // 上限在创建任何实体之前检查，禁用光源不占用渲染器的十六个活动槽。
    if (object.has_light() && object.light().enabled()) {
        std::size_t enabled{};
        for (auto [entity, light] : registry_.view<const Light>().each()) { (void)entity; if (light.enabled) ++enabled; }
        if (enabled >= 16) throw std::length_error("一个场景最多允许十六个启用光源");
    }
    auto name = object.name().empty() ? std::string("对象") : object.name();
    const auto base = name;
    for (std::size_t suffix = 2;; ++suffix) {
        bool exists = false;
        for (auto [entity, current] : registry_.view<const Name>().each()) { (void)entity; if (current.value == name) { exists = true; break; } }
        if (!exists) break;
        name = base + " " + std::to_string(suffix);
    }

    // 普通 ECS 组件供现有编辑历史捕获，预设索引不成为实体的隐藏依赖。
    const auto previousRevision = revision_;
    const auto previousId = nextObjectId_;
    const auto entity = CreateEntity(std::move(name));
    try {
        registry_.replace<Transform>(entity, transform);
        if (!object.mesh_id().empty())
            // 多表面预设与普通模型共用一个实体，不再按材质拆分放置。
            registry_.emplace<MeshRenderer>(entity, preset.firstTriangle, preset.triangleCount, preset.materialIndex,
                object.visible(), false, meshHandle_, !object.surface_materials().empty() && object.use_surface_materials());
        if (object.has_camera()) {
            const auto& source = object.camera();
            registry_.emplace<Camera>(entity, source.vertical_fov_degrees(), source.near_plane(), source.far_plane(), source.active());
        }
        if (object.has_light()) {
            const auto& source = object.light();
            auto& light = registry_.emplace<Light>(entity);
            light.type = static_cast<LightType>(source.type());
            light.color = source.has_color() ? render::ReadVector(source.color()) : Vec3{1, 1, 1};
            light.intensity = source.intensity(); light.width = source.width(); light.height = source.height(); light.range = source.range();
            light.enabled = source.enabled(); light.sourceRadius = source.source_radius();
            light.outerAngleDegrees = source.outer_angle_degrees(); light.innerAngleDegrees = source.inner_angle_degrees();
            if (source.has_direction()) light.direction = render::ReadVector(source.direction());
            if (source.has_tangent()) light.tangent = render::ReadVector(source.tangent());
            if (source.has_bitangent()) light.bitangent = render::ReadVector(source.bitangent());
        }
        if (object.has_velocity()) registry_.emplace<Velocity>(entity, render::ReadVector(object.velocity()));
        if (object.has_lua()) registry_.emplace<LuaScript>(entity, object.lua().source(), object.lua().path(), object.lua().enabled());
    } catch (...) {
        registry_.destroy(entity); revision_ = previousRevision; nextObjectId_ = previousId;
        throw;
    }
    if (CameraEntity() == entt::null && object.has_camera() && object.camera().active()) cameraEntity_ = entity;
    if (LightEntity() == entt::null && object.has_light()) lightEntity_ = entity;
    return entity;
}
} // 游戏逻辑命名空间
