#include "gameplay/World.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <stdexcept>

namespace gameplay {
namespace {
Transform ReadTransform(const render::config::Transform &source) {
    Transform result;
    result.position = render::ReadVector(source.position());
    result.rotationDegrees = render::ReadVector(source.rotation_degrees());
    const auto &scale = source.scale();
    result.scale = {scale.has_x() ? scale.x() : 1, scale.has_y() ? scale.y() : 1, scale.has_z() ? scale.z() : 1};
    return result;
}
void WriteLane(const render::Float4 &v, render::config::Vec4 *out) {
    out->set_x(v.x);
    out->set_y(v.y);
    out->set_z(v.z);
    out->set_w(v.w);
}
render::config::Scene *MutableScene(render::WorldDocument &document, const std::string &id) {
    for (auto &level : *document.mutable_levels())
        for (auto &scene : *level.mutable_scenes())
            if (scene.id() == id)
                return &scene;
    return nullptr;
}
} // 匿名命名空间

void World::CreateDocument(const render::WorldDocument &document, std::string_view sceneId) {
    render::ValidateWorldDocument(document);
    const auto &authored = render::FindDocumentScene(document, sceneId);
    const auto scene = render::BakeScene(document, authored.id(), false);
    // 所有资源与组件先在临时所有者中创建，异常不能破坏当前世界。
    World next;
    static std::atomic_uint32_t generation{0};
    next.entityVersionSeed_ = ((generation.fetch_add(2) + 2) % (entt::entt_traits<Entity>::version_mask - 2)) + 1;
    if (!scene.triangles.empty())
        next.CreateScene(scene, false);
    else {
        // 空场景仍可拥有摄像机、灯光或脚本，无需伪造可见几何。
        std::vector<render::MaterialResource *> materials;
        for (std::size_t i = 0; i < scene.materials.size(); ++i) {
            const auto handle =
                next.resources_.Create<render::MaterialResource>(scene.materialNames[i], scene.materials[i]);
            next.materialHandles_.push_back(handle);
            materials.push_back(next.resources_.Resolve(handle));
        }
        next.materials_ = render::MaterialParameters(std::move(materials));
        next.meshHandle_ = next.resources_.Create<render::MeshResource>("空场景网格");
    }
    next.registry_.clear();
    next.cameraEntity_ = next.lightEntity_ = entt::null;
    next.document_ = document;
    next.activeSceneId_ = authored.id();
    for (const auto &level : document.levels())
        for (const auto &candidate : level.scenes())
            if (candidate.id() == authored.id())
                next.activeLevelId_ = level.id();
    next.materialIds_ = scene.materialIds;
    next.materialObjectIds_ = scene.materialObjectIds;
    next.textureIds_ = scene.textureIds;
    std::map<std::string, const render::SceneMeshPart *> parts;
    for (const auto &part : scene.meshParts)
        parts.emplace(part.objectId, &part);
    std::map<std::string, std::uint32_t> emitterMaterials;
    for (const auto &object : authored.objects())
        if (object.has_light() && !object.light().emitter_object_id().empty())
            emitterMaterials.emplace(object.light().emitter_object_id(),
                                     parts.at(object.light().emitter_object_id())->materialIndex);
    for (const auto &object : authored.objects()) {
        const auto entity = next.CreateEntity(object.name());
        next.registry_.replace<ObjectIdentity>(entity, next.activeLevelId_, next.activeSceneId_, object.id());
        next.registry_.replace<Transform>(entity, ReadTransform(object.transform()));
        if (!object.mesh_id().empty()) {
            const auto &part = *parts.at(object.id());
            // 作者源映射不依赖对象继续存活，复制后删除原对象仍可正确导出。
            next.objectSources_.push_back({object, part.firstTriangle, part.triangleCount, part.materialIndex});
            next.registry_.emplace<MeshRenderer>(entity, part.firstTriangle, part.triangleCount, part.materialIndex,
                                                 object.visible(), emitterMaterials.contains(object.id()),
                                                 next.meshHandle_, part.useSurfaceMaterials && object.use_surface_materials());
        }
        if (object.has_camera()) {
            const auto &c = object.camera();
            next.registry_.emplace<Camera>(entity, c.vertical_fov_degrees(), c.near_plane(), c.far_plane(), c.active());
            if (next.cameraEntity_ == entt::null && c.active())
                next.cameraEntity_ = entity;
        }
        if (object.has_light()) {
            const auto &source = object.light();
            auto &light = next.registry_.emplace<Light>(entity);
            light.type = static_cast<LightType>(source.type());
            light.color = source.has_color() ? render::ReadVector(source.color()) : Vec3{1, 1, 1};
            light.intensity = source.intensity();
            light.width = source.width();
            light.height = source.height();
            light.range = source.range();
            light.enabled = source.enabled();
            light.sourceRadius = source.source_radius();
            light.outerAngleDegrees = source.outer_angle_degrees();
            light.innerAngleDegrees = source.inner_angle_degrees();
            if (source.has_direction())
                light.direction = render::ReadVector(source.direction());
            if (source.has_tangent())
                light.tangent = render::ReadVector(source.tangent());
            if (source.has_bitangent())
                light.bitangent = render::ReadVector(source.bitangent());
            if (!source.emitter_object_id().empty())
                light.emitterMaterial = emitterMaterials.at(source.emitter_object_id());
            if (next.lightEntity_ == entt::null)
                next.lightEntity_ = entity;
        }
        if (object.has_velocity())
            next.registry_.emplace<Velocity>(entity, render::ReadVector(object.velocity()));
        if (object.has_lua())
            next.registry_.emplace<LuaScript>(entity, object.lua().source(), object.lua().path(),
                                              object.lua().enabled());
    }
    // 运行时材质资源保存共享目录的脚本；参数变体仍借用相同的作者定义。
    for (std::size_t i = 0; i < next.materialIds_.size(); ++i)
        for (const auto &source : document.materials())
            if (source.id() == next.materialIds_[i]) {
                next.Material(i)->ScriptSource() = source.script_source();
                next.Material(i)->ScriptPath() = source.script_path();
            }
    for (std::size_t i = 0; i < next.materialObjectIds_.size(); ++i)
        for (const auto &object : authored.objects())
            if (object.id() == next.materialObjectIds_[i]) {
                const auto& part = *parts.at(object.id());
                // 整体覆盖脚本只归属于覆盖槽，不能污染保留中的原始表面材质。
                if (i == part.materialIndex && (object.surface_materials().empty() || !object.use_surface_materials())) {
                    if (object.has_material_script_source()) next.Material(i)->ScriptSource() = object.material_script_source();
                    if (object.has_material_script_path()) next.Material(i)->ScriptPath() = object.material_script_path();
                }
                // 表面变体共用模型身份，通过其实际三角形材质槽恢复各自脚本。
                for (const auto& surface : object.surface_materials()) {
                    const auto slot = static_cast<std::uint32_t>(scene.triangles.at(part.firstTriangle + surface.first_triangle()).a.w);
                    if (slot != i) continue;
                    if (surface.has_material_script_source()) next.Material(i)->ScriptSource() = surface.material_script_source();
                    if (surface.has_material_script_path()) next.Material(i)->ScriptPath() = surface.material_script_path();
                }
            }
    next.revision_ = revision_ + 1;
    *this = std::move(next);
}

void World::AppendScene(const render::Scene& scene) {
    // 首次创建使用现有默认相机与灯光；后续追加只消费模型实际携带的组件。
    if (!meshHandle_) {
        CreateScene(scene);
        return;
    }
    World imported;
    imported.CreateScene(scene);
    const auto incoming = imported.ExportDocument();
    auto combined = ExportDocument();
    const auto active = activeSceneId_.empty() ? combined.default_scene_id() : activeSceneId_;
    auto* target = MutableScene(combined, active);
    if (!target) throw std::logic_error("追加模型的目标场景不存在");

    // 每次导入使用独立资源命名空间，源文件重复导入也不能覆盖已有作者引用。
    std::set<std::string> identifiers;
    for (const auto& material : combined.materials()) identifiers.insert(material.id());
    for (const auto& texture : combined.textures()) identifiers.insert(texture.id());
    for (const auto& mesh : combined.meshes()) identifiers.insert(mesh.id());
    for (const auto& level : combined.levels()) for (const auto& existing : level.scenes())
        for (const auto& object : existing.objects()) identifiers.insert(object.id());
    std::string prefix;
    for (std::size_t index = 1;; ++index) {
        prefix = "model-import." + std::to_string(index) + ".";
        if (std::none_of(identifiers.begin(), identifiers.end(), [&](const auto& id) { return id.starts_with(prefix); })) break;
    }
    for (const auto& source : incoming.materials()) {
        auto* material = combined.add_materials();
        *material = source;
        material->set_id(prefix + "material." + source.id());
    }
    for (const auto& source : incoming.textures()) {
        auto* texture = combined.add_textures();
        *texture = source;
        texture->set_id(prefix + "texture." + source.id());
    }
    for (const auto& source : incoming.meshes()) {
        auto* mesh = combined.add_meshes();
        *mesh = source;
        mesh->set_id(prefix + "mesh." + source.id());
    }
    // 模型表面与普通对象通过同一引用重定位逻辑，纹理仍由世界共享目录拥有。
    const auto relocateMaterial = [&](auto& binding) {
        if (!binding.material_id().empty()) binding.set_material_id(prefix + "material." + binding.material_id());
        for (auto& texture : *binding.mutable_texture_bindings())
            texture.set_texture_id(prefix + "texture." + texture.texture_id());
    };
    for (const auto& source : render::FindDocumentScene(incoming, {}).objects()) {
        if (source.mesh_id().empty() && !(source.has_light() && !scene.lights.empty()) &&
            !(source.has_camera() && !scene.cameras.empty())) continue;
        auto* object = target->add_objects();
        *object = source;
        object->set_id(prefix + "object." + source.id());
        if (!source.mesh_id().empty()) object->set_mesh_id(prefix + "mesh." + source.mesh_id());
        relocateMaterial(*object);
        for (auto& surface : *object->mutable_surface_materials()) relocateMaterial(surface);
        if (object->has_light() && !object->light().emitter_object_id().empty())
            object->mutable_light()->set_emitter_object_id(prefix + "object." + object->light().emitter_object_id());
        if (object->has_camera()) object->mutable_camera()->set_active(false);
    }
    // 正式文档边界在候选所有者内完成校验和创建，异常不会部分追加资源或对象。
    CreateDocument(combined, active);
}

render::WorldDocument World::ExportDocument() const {
    render::WorldDocument output = document_;
    std::string active = activeSceneId_;
    std::vector<std::string> materialIds = materialIds_;
    if (output.levels().empty()) {
        materialIds.clear();
        // 旧模型导入入口通过嵌入网格提升为完整文档，不引入反向模块依赖。
        output.set_format_version(1);
        output.set_id("imported-world");
        output.set_name("导入世界");
        active = "imported-scene";
        output.set_default_scene_id(active);
        auto *level = output.add_levels();
        level->set_id("imported-level");
        level->set_name("导入关卡");
        auto *scene = level->add_scenes();
        scene->set_id(active);
        scene->set_name("导入场景");
        for (std::size_t i = 0; i < materials_.size(); ++i) {
            const auto id = "material." + std::to_string(i);
            materialIds.push_back(id);
            auto *material = output.add_materials();
            material->set_id(id);
            material->set_name(Material(i)->Name());
        }
        const auto *resource = resources_.Resolve(meshHandle_);
        if (resource && !resource->Triangles().empty()) {
            auto *mesh = output.add_meshes();
            mesh->set_id("imported-mesh");
            mesh->set_name(resource->Name());
            mesh->set_source_path(sourcePath_);
            auto *data = mesh->mutable_data();
            for (const auto &a : resource->Attributes()) {
                auto *record = data->add_attributes();
                WriteLane(a.normal0, record->mutable_normal0());
                WriteLane(a.normal1, record->mutable_normal1());
                WriteLane(a.normal2, record->mutable_normal2());
                WriteLane(a.uv01, record->mutable_uv01());
                WriteLane(a.uv2, record->mutable_uv2());
                WriteLane(a.tangent, record->mutable_tangent());
            }
            for (const auto &t : resource->Triangles()) {
                auto *record = data->add_triangles();
                render::WriteVector({t.a.x, t.a.y, t.a.z}, *record->mutable_a());
                render::WriteVector({t.b.x, t.b.y, t.b.z}, *record->mutable_b());
                render::WriteVector({t.c.x, t.c.y, t.c.z}, *record->mutable_c());
                record->set_attribute(static_cast<std::uint32_t>(t.b.w));
            }
        }
    }
    auto *authored = MutableScene(output, active);
    if (!authored)
        throw std::logic_error("当前场景未包含在世界文档中");
    std::map<std::string, render::config::Object> originals;
    for (const auto &object : authored->objects())
        originals.emplace(object.id(), object);
    authored->clear_objects();
    // 基础材质参数与脚本回写到世界目录，对象变体单独保存覆盖值。
    for (std::size_t i = 0; i < materials_.size() && i < materialIds.size(); ++i) {
        if (i < materialObjectIds_.size() && !materialObjectIds_[i].empty())
            continue;
        for (auto &material : *output.mutable_materials())
            if (material.id() == materialIds[i]) {
                render::WriteMaterialParameters(materials_[i], *material.mutable_parameters());
                material.set_script_source(Material(i)->ScriptSource());
                material.set_script_path(Material(i)->ScriptPath());
            }
    }
    std::vector<Entity> entities;
    for (auto entity : registry_.view<const Name>())
        entities.push_back(entity);
    std::sort(entities.begin(), entities.end(),
              [](Entity a, Entity b) { return entt::to_integral(a) < entt::to_integral(b); });
    // 对象和模型表面共用同一序列化边界，完整保留参数、脚本、图像身份与采样方式。
    const auto writeMaterial = [&](auto& record, std::uint32_t materialIndex) {
        if (materialIndex >= materialIds.size()) throw std::invalid_argument("对象引用的材质槽越界");
        record.set_material_id(materialIds[materialIndex]);
        record.clear_material_override();
        record.clear_material_script_source();
        record.clear_material_script_path();
        record.clear_texture_bindings();
        if (materialIndex < materialObjectIds_.size() && !materialObjectIds_[materialIndex].empty()) {
            render::WriteMaterialParameters(materials_[materialIndex], *record.mutable_material_override());
            record.set_material_script_source(Material(materialIndex)->ScriptSource());
            record.set_material_script_path(Material(materialIndex)->ScriptPath());
        }
        for (unsigned slot = 0; slot < 8; ++slot)
            if (const auto* binding = Material(materialIndex)->Texture(slot)) {
                const auto* image = resources_.Resolve(binding->texture);
                if (!image) throw std::logic_error("纹理资源所有者失效");
                std::string textureId;
                const auto viewIndex = static_cast<std::size_t>(binding - textureBindings_.data());
                if (viewIndex < textureIds_.size()) textureId = textureIds_[viewIndex];
                if (textureId.empty())
                    for (const auto& texture : output.textures())
                        if (texture.width() == image->Width() && texture.height() == image->Height() &&
                            std::equal(texture.rgba().begin(), texture.rgba().end(), image->Texels().begin(), image->Texels().end())) {
                            textureId = texture.id();
                            break;
                        }
                const bool exists = std::any_of(output.textures().begin(), output.textures().end(),
                    [&](const auto& texture) { return texture.id() == textureId; });
                if (!exists) {
                    if (textureId.empty()) textureId = "image." + std::to_string(binding->texture.value);
                    auto* texture = output.add_textures();
                    texture->set_id(textureId);
                    texture->set_name(image->Name());
                    texture->set_width(image->Width());
                    texture->set_height(image->Height());
                    for (const auto pixel : image->Texels()) texture->add_rgba(pixel);
                }
                auto* texture = record.add_texture_bindings();
                texture->set_slot(slot);
                texture->set_texture_id(textureId);
                WriteLane(binding->view.offsetScale, texture->mutable_offset_scale());
                WriteLane(binding->view.rotation, texture->mutable_rotation());
                texture->set_wrap_u(binding->view.image.w & 3);
                texture->set_wrap_v((binding->view.image.w >> 2) & 3);
            }
    };
    for (const auto entity : entities) {
        const auto *identity = registry_.try_get<ObjectIdentity>(entity);
        const auto id =
            identity ? identity->objectId : output.id() + ".entity." + std::to_string(entt::to_integral(entity));
        auto *object = authored->add_objects();
        if (originals.contains(id))
            *object = originals.at(id);
        object->set_id(id);
        object->set_name(registry_.get<Name>(entity).value);
        if (const auto *transform = registry_.try_get<Transform>(entity)) {
            auto *t = object->mutable_transform();
            render::WriteVector(transform->position, *t->mutable_position());
            render::WriteVector(transform->rotationDegrees, *t->mutable_rotation_degrees());
            render::WriteVector(transform->scale, *t->mutable_scale());
        }
        if (const auto *mesh = registry_.try_get<MeshRenderer>(entity)) {
            if (mesh->materialIndex >= materialIds.size())
                throw std::invalid_argument("对象引用的材质槽越界");
            if (object->mesh_id().empty()) {
                // 预设与已删除原对象的副本都通过不可变源范围恢复正式资源引用。
                for (const auto& source : objectSources_)
                    if (source.firstTriangle == mesh->firstTriangle && source.triangleCount == mesh->triangleCount) {
                        object->set_mesh_id(source.object.mesh_id());
                        object->set_first_triangle(source.object.first_triangle());
                        object->set_triangle_count(source.object.triangle_count());
                        break;
                    }
                // 复制实体按精确源范围继承网格引用，导入旧场景则引用嵌入载荷。
                for (const auto &[key, source] : originals) {
                    if (!object->mesh_id().empty()) break;
                    (void)key;
                    for (const auto &part : resources_.Resolve(meshHandle_)->Surfaces())
                        if (part.objectId == source.id() && part.firstTriangle == mesh->firstTriangle &&
                            part.triangleCount == mesh->triangleCount) {
                            object->set_mesh_id(source.mesh_id());
                            object->set_first_triangle(source.first_triangle());
                            object->set_triangle_count(source.triangle_count());
                            break;
                        }
                    if (!object->mesh_id().empty())
                        break;
                }
                if (object->mesh_id().empty()) {
                    object->set_mesh_id("imported-mesh");
                    object->set_first_triangle(mesh->firstTriangle);
                    object->set_triangle_count(mesh->triangleCount);
                }
            }
            writeMaterial(*object, mesh->materialIndex);
            object->set_visible(mesh->visible);
            object->clear_surface_materials();
            object->clear_use_surface_materials();
            const auto* resource = resources_.Resolve(mesh->meshResource ? mesh->meshResource : meshHandle_);
            // 即使实例暂时使用整体覆盖，也持久化源模型的表面，重新打开后仍可恢复。
            const bool hasSurfaces = mesh->useSurfaceMaterials || std::any_of(resource->Surfaces().begin(), resource->Surfaces().end(),
                [&](const auto& part) { return part.useSurfaceMaterials && part.firstTriangle == mesh->firstTriangle && part.triangleCount == mesh->triangleCount; });
            if (hasSurfaces) {
                object->set_use_surface_materials(mesh->useSurfaceMaterials);
                if (mesh->useSurfaceMaterials) {
                    object->clear_texture_bindings();
                    object->clear_material_override();
                    object->clear_material_script_source();
                    object->clear_material_script_path();
                }
                const auto triangles = resource->Triangles();
                for (std::uint32_t first = 0; first < mesh->triangleCount;) {
                    const auto index = static_cast<std::uint32_t>(triangles[mesh->firstTriangle + first].a.w);
                    std::uint32_t count = 1;
                    while (first + count < mesh->triangleCount &&
                        triangles[mesh->firstTriangle + first + count].a.w == static_cast<float>(index)) ++count;
                    auto* surface = object->add_surface_materials();
                    surface->set_first_triangle(first);
                    surface->set_triangle_count(count);
                    writeMaterial(*surface, index);
                    first += count;
                }
            }
        } else {
            object->clear_mesh_id();
            object->clear_material_id();
            object->clear_texture_bindings();
            object->clear_material_override();
            object->clear_material_script_source();
            object->clear_material_script_path();
            object->clear_surface_materials();
            object->clear_use_surface_materials();
        }
        if (const auto *camera = registry_.try_get<Camera>(entity)) {
            auto *c = object->mutable_camera();
            c->set_vertical_fov_degrees(camera->verticalFovDegrees);
            c->set_near_plane(camera->nearPlane);
            c->set_far_plane(camera->farPlane);
            c->set_active(camera->active);
        } else
            object->clear_camera();
        if (const auto *light = registry_.try_get<Light>(entity)) {
            auto *l = object->mutable_light();
            l->set_type(static_cast<render::config::Light_Type>(light->type));
            render::WriteVector(light->color, *l->mutable_color());
            l->set_intensity(light->intensity);
            l->set_width(light->width);
            l->set_height(light->height);
            l->set_range(light->range);
            l->set_enabled(light->enabled);
            l->set_source_radius(light->sourceRadius);
            l->set_outer_angle_degrees(light->outerAngleDegrees);
            l->set_inner_angle_degrees(light->innerAngleDegrees);
            render::WriteVector(light->direction, *l->mutable_direction());
            render::WriteVector(light->tangent, *l->mutable_tangent());
            render::WriteVector(light->bitangent, *l->mutable_bitangent());
            if (document_.levels().empty() && light->emitterMaterial != ~0u)
                for (auto owned : registry_.view<const MeshRenderer>()) {
                    const auto &mesh = registry_.get<MeshRenderer>(owned);
                    if (mesh.lightOwned && mesh.materialIndex == light->emitterMaterial)
                        l->set_emitter_object_id(output.id() + ".entity." + std::to_string(entt::to_integral(owned)));
                }
        } else
            object->clear_light();
        if (const auto *velocity = registry_.try_get<Velocity>(entity))
            render::WriteVector(velocity->linear, *object->mutable_velocity());
        else
            object->clear_velocity();
        if (const auto *script = registry_.try_get<LuaScript>(entity)) {
            auto *l = object->mutable_lua();
            l->set_source(script->source);
            l->set_path(script->path);
            l->set_enabled(script->enabled);
        } else
            object->clear_lua();
    }
    // 删除发光体对象时同时断开作者引用，让解析光源继续使用通用生成几何。
    std::set<std::string> existing;
    for (const auto &object : authored->objects())
        existing.insert(object.id());
    for (auto &object : *authored->mutable_objects())
        if (object.has_light() && !existing.contains(object.light().emitter_object_id()))
            object.mutable_light()->clear_emitter_object_id();
    output.set_default_scene_id(active);
    render::ValidateWorldDocument(output);
    return output;
}

void World::SwitchScene(std::string_view sceneId) {
    // 先提取当前编辑，只有目标场景完整实例化成功后才替换现有状态。
    auto document = ExportDocument();
    CreateDocument(document, sceneId);
}
} // 游戏逻辑命名空间
