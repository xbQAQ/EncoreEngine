#include "gameplay/World.h"
#include "asset/SceneImporter.h"
#include "editor/EditorHistory.h"
#include "render/SceneExtraction.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    // 使用真实文档和资源范围验证放置行为，避免只检查接口是否存在。
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void Rejects(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
template<class World> void Run(World& world, const render::WorldDocument& library) {
    if constexpr (!requires { world.InstallObjectPresets(library); world.ObjectPresets(); world.CreatePresetObject(0, gameplay::Transform{}); }) {
        // 首次运行明确报告缺失功能，使测试在实现接口之前也能形成有效红灯。
        throw std::runtime_error("World 尚未实现对象预设安装与创建接口");
    } else {
        const auto camera = world.CameraEntity();
        const auto before = world.ExtractRenderSnapshot();
        const auto oldSource = world.CookResourceScene();
        const auto oldMesh = before.instances.front().mesh;
        const auto oldCount = world.Registry().template view<gameplay::Name>().size();
        world.InstallObjectPresets(library);
        const auto& presets = world.ObjectPresets();
        Require(presets.size() == 7, "正式对象库必须提供两种网格与五种灯光");
        Require(world.Registry().valid(camera) && world.Registry().template view<gameplay::Name>().size() == oldCount,
            "安装预设不能替换既有实体或创建模板实体");
        const auto installed = world.ExtractRenderSnapshot();
        const auto source = world.CookResourceScene();
        Require(installed.instances.size() == before.instances.size() && installed.lights.size() == before.lights.size(),
            "安装对象库不能改变可见几何实例或活动光源");
        Require(source.triangles.size() == oldSource.triangles.size() + 14 && source.attributes.size() > oldSource.attributes.size(),
            "安装必须只追加方块与平面的共享源几何和属性");
        Require(world.Resources().Resolve(oldMesh.meshResource) != nullptr, "安装不能使旧资源句柄失效");
        for (std::size_t i = 0; i < oldSource.triangles.size(); ++i) {
            const auto& a = source.triangles[i]; const auto& b = oldSource.triangles[i];
            Require(a.a.x == b.a.x && a.a.w == b.a.w && a.b.w == b.b.w && a.c.z == b.c.z,
                "追加预设必须保留旧源几何和属性索引");
        }
        const auto onceRevision = world.Revision();
        world.InstallObjectPresets(library);
        Require(world.CookResourceScene().triangles.size() == source.triangles.size() && world.Revision() == onceRevision,
            "重复安装同一对象库必须幂等");

        // 同一模板的多个实例共享不可变资源，但名称与稳定作者标识互不相同。
        gameplay::Transform placement; placement.position = {3, 2, 1}; placement.rotationDegrees.y = 30;
        const auto cube = world.CreatePresetObject(0, placement);
        const auto duplicate = world.CreatePresetObject(0, placement);
        const auto& firstMesh = world.Registry().template get<gameplay::MeshRenderer>(cube);
        const auto& secondMesh = world.Registry().template get<gameplay::MeshRenderer>(duplicate);
        Require(firstMesh.firstTriangle == presets[0].firstTriangle && firstMesh.triangleCount == 12 &&
            secondMesh.firstTriangle == firstMesh.firstTriangle && secondMesh.materialIndex == firstMesh.materialIndex &&
            secondMesh.meshResource == firstMesh.meshResource, "重复放置必须共享网格范围与材质");
        Require(world.Registry().template get<gameplay::Name>(cube).value != world.Registry().template get<gameplay::Name>(duplicate).value &&
            world.Registry().template get<gameplay::ObjectIdentity>(cube).objectId != world.Registry().template get<gameplay::ObjectIdentity>(duplicate).objectId,
            "重复放置必须生成唯一名称与稳定对象标识");
        Require(world.Registry().template get<gameplay::Transform>(cube).position.x == 3 &&
            world.Registry().template get<gameplay::Transform>(cube).rotationDegrees.y == 30,
            "创建对象必须消费调用者提供的放置姿态");
        world.CreatePresetObject(1, placement);
        Require(world.CookResourceScene().triangles.size() == source.triangles.size(), "放置实例不能扩充源几何");
        const auto extracted = render::ExtractRenderScene(world.CookResourceScene(), world.ExtractRenderSnapshot());
        Require(extracted.triangles.size() == render::ExtractRenderScene(oldSource, before).triangles.size() + 26,
            "预设源几何只能由实际放置实例加入渲染");
        const auto& plane = source.triangles[presets[1].firstTriangle];
        Require(render::Cross(render::Vec3{plane.b.x-plane.a.x, plane.b.y-plane.a.y, plane.b.z-plane.a.z},
            render::Vec3{plane.c.x-plane.a.x, plane.c.y-plane.a.y, plane.c.z-plane.a.z}).y > 0,
            "单位平面法线必须朝向正 Y");

        // 无效输入在生成名称、身份、实体或资源之前被拒绝。
        const auto stableRevision = world.Revision();
        const auto stableCount = world.Registry().template view<gameplay::Name>().size();
        Rejects([&] { world.CreatePresetObject(99, placement); }, "未知预设索引必须拒绝");
        auto invalid = placement; invalid.scale.y = 0;
        Rejects([&] { world.CreatePresetObject(0, invalid); }, "零缩放必须拒绝");
        invalid = placement; invalid.position.x = std::numeric_limits<float>::infinity();
        Rejects([&] { world.CreatePresetObject(0, invalid); }, "非有限放置姿态必须拒绝");
        Require(world.Revision() == stableRevision && world.Registry().template view<gameplay::Name>().size() == stableCount,
            "拒绝创建后不能留下实体或版本变化");
        auto conflict = library; conflict.mutable_meshes(0)->mutable_box()->mutable_size()->set_x(2);
        Rejects([&] { world.InstallObjectPresets(conflict); }, "同标识不同资源定义必须拒绝");
        conflict = library; conflict.mutable_materials(0)->mutable_parameters()->mutable_base_roughness()->set_x(0.1f);
        Rejects([&] { world.InstallObjectPresets(conflict); }, "已安装库的同标识材质定义改变必须拒绝");
        Require(world.Revision() == stableRevision, "资源冲突不能部分安装");

        // 即便原实例已删除，复制对象仍能通过源范围恢复正式模板网格引用。
        const auto clone = world.CreateEntity("复制方块");
        world.Registry().template get<gameplay::Transform>(clone) = placement;
        world.Registry().template emplace<gameplay::MeshRenderer>(clone, firstMesh);
        world.DestroyEntity(cube); world.DestroyEntity(duplicate);
        const auto saved = world.ExportDocument();
        bool found = false;
        for (const auto& object : render::FindDocumentScene(saved, world.ActiveSceneId()).objects())
            if (object.name() == "复制方块") { found = true; Require(object.mesh_id() == "editor.preset.mesh.cube", "复制对象必须保存模板网格标识"); }
        Require(found, "导出必须保留放置对象");
        // 往返文档写入引擎专属临时文件，避免覆盖旧版物体预设测试的输出。
        const auto path = std::filesystem::temp_directory_path() / "EncoreObjectPresetTest.pb";
        render::SaveWorldDocument(saved, path);
        World reopened; reopened.CreateDocument(render::LoadWorldDocument(path));
        const auto reopenedSource = reopened.CookResourceScene().triangles.size();
        reopened.InstallObjectPresets(library);
        Require(reopened.ObjectPresets().size() == 7 && reopened.CookResourceScene().triangles.size() == reopenedSource + 14,
            "保存重开后必须复用目录标识并重建预设源范围");
        reopened.CreatePresetObject(0, placement);
        render::ValidateWorldDocument(reopened.ExportDocument());
        std::filesystem::remove(path);

        // 五种解析灯光均从模板参数创建，并在达到十六盏上限后保持原子拒绝。
        std::set<gameplay::LightType> types;
        for (std::size_t i = 2; i < presets.size(); ++i) {
            const auto entity = world.CreatePresetObject(i, placement);
            types.insert(world.Registry().template get<gameplay::Light>(entity).type);
        }
        Require(types.size() == 5, "对象库必须覆盖所有灯光类型");
        while (world.ExtractRenderSnapshot().lights.size() < 16) world.CreatePresetObject(3, placement);
        const auto limitRevision = world.Revision();
        Rejects([&] { world.CreatePresetObject(3, placement); }, "第十七盏启用灯光必须拒绝");
        Require(world.Revision() == limitRevision && world.ExtractRenderSnapshot().lights.size() == 16,
            "灯光超限不能留下部分创建状态");
        render::ValidateWorldDocument(world.ExportDocument());
    }
}
template<class World> void ExtraCases(const render::WorldDocument& library) {
    // 多表面模板也保留整体覆盖开关，未支持的表面私有资源必须在安装前明确拒绝。
    auto surfaceLibrary = library;
    auto* surfaceObject = surfaceLibrary.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0);
    auto* firstSurface = surfaceObject->add_surface_materials();
    firstSurface->set_triangle_count(6);
    firstSurface->set_material_id(surfaceObject->material_id());
    auto* secondSurface = surfaceObject->add_surface_materials();
    secondSurface->set_first_triangle(6); secondSurface->set_triangle_count(6);
    auto* secondMaterial = surfaceLibrary.add_materials();
    *secondMaterial = surfaceLibrary.materials(0);
    secondMaterial->set_id("test.surface.secondary");
    secondMaterial->mutable_parameters()->mutable_base_roughness()->set_x(.12f);
    secondSurface->set_material_id(secondMaterial->id());
    surfaceObject->set_use_surface_materials(false);
    World surfaceWorld; surfaceWorld.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath()));
    surfaceWorld.InstallObjectPresets(surfaceLibrary);
    const auto surfaceEntity = surfaceWorld.CreatePresetObject(0, {});
    Require(!surfaceWorld.Registry().template get<gameplay::MeshRenderer>(surfaceEntity).useSurfaceMaterials,
        "多表面预设必须保留作者设置的整体材质覆盖模式");
    auto texturedLibrary = surfaceLibrary;
    auto* image = texturedLibrary.add_textures(); image->set_id("test.surface.image");
    image->set_width(1); image->set_height(1); image->add_rgba(0xffffffffu);
    auto* binding = texturedLibrary.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials(0)->add_texture_bindings();
    binding->set_texture_id(image->id());
    World rejected; rejected.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath()));
    const auto intact = rejected.ExportDocument().SerializeAsString();
    Rejects([&] { rejected.InstallObjectPresets(texturedLibrary); }, "表面纹理不能绕过预设安装的纹理限制");
    auto variantLibrary = surfaceLibrary;
    variantLibrary.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials(0)->mutable_material_override()->mutable_base_roughness()->set_w(.2f);
    Rejects([&] { rejected.InstallObjectPresets(variantLibrary); }, "预设尚未支持表面私有变体时必须明确拒绝");
    Require(rejected.ExportDocument().SerializeAsString() == intact, "拒绝表面私有资源不能部分安装预设");

    // 带配置默认姿态的模板仍应精确消费 UI 已合并完成的最终作者姿态。
    auto configured = library;
    auto* defaults = configured.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_transform();
    defaults->mutable_position()->set_y(0.25f);
    defaults->mutable_rotation_degrees()->set_y(25);
    defaults->mutable_scale()->set_x(2);
    World transformed; transformed.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath()));
    transformed.InstallObjectPresets(configured);
    gameplay::Transform finalPose; finalPose.position = {2, 3, 4}; finalPose.rotationDegrees.y = 55; finalPose.scale = {2, 3, 4};
    const auto posed = transformed.CreatePresetObject(0, finalPose);
    const auto actual = transformed.Registry().template get<gameplay::Transform>(posed);
    Require(actual.position.y == 3 && actual.rotationDegrees.y == 55 && actual.scale.x == 2 && actual.scale.z == 4,
        "创建接口必须精确消费最终作者姿态，不能再次应用模板变换");

    // 编辑后的预设共享材质属于作者数据，重复安装及保存重开不能恢复库默认值。
    auto unrelated = render::LoadWorldDocument(render::DefaultWorldPath());
    *unrelated.add_materials() = library.materials(0);
    unrelated.mutable_materials(unrelated.materials_size() - 1)->mutable_parameters()->mutable_base_roughness()->set_x(0.1f);
    World collision; collision.CreateDocument(unrelated);
    Rejects([&] { collision.InstallObjectPresets(library); }, "没有预设对象归属的同标识材质冲突必须拒绝");
    World edited; edited.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath()));
    edited.InstallObjectPresets(library);
    const auto placed = edited.CreatePresetObject(0, {});
    const auto materialSlot = edited.Registry().template get<gameplay::MeshRenderer>(placed).materialIndex;
    edited.Materials()[materialSlot].baseRoughness.x = 0.21f;
    edited.Material(materialSlot)->ScriptSource() = "void Surface(inout Material m, float2 uv, float4 t) {}";
    edited.MarkDirty();
    edited.InstallObjectPresets(library);
    Require(edited.Materials()[materialSlot].baseRoughness.x == 0.21f, "重复安装不能覆盖预设材质的作者编辑");
    const auto authoredEdit = edited.ExportDocument();
    World restored; restored.CreateDocument(authoredEdit); restored.InstallObjectPresets(library);
    const auto restoredSlot = restored.ObjectPresets()[0].materialIndex;
    Require(restored.Materials()[restoredSlot].baseRoughness.x == 0.21f && !restored.Material(restoredSlot)->ScriptSource().empty(),
        "重开并安装对象库后必须保留共享材质参数和脚本编辑");

    // 删除全部预设实例后目录仍归作者世界所有，重开不能把已编辑材质误判为陌生资源冲突。
    edited.DestroyEntity(placed);
    // 删除后重开使用独立文档，确保不会误读前一个创建测试的持久化结果。
    const auto deletedPath = std::filesystem::temp_directory_path() / "EncoreObjectPresetDeletedTest.pb";
    render::SaveWorldDocument(edited.ExportDocument(), deletedPath);
    World afterDeletion; afterDeletion.CreateDocument(render::LoadWorldDocument(deletedPath));
    afterDeletion.InstallObjectPresets(library);
    const auto afterDeletionSlot = afterDeletion.ObjectPresets()[0].materialIndex;
    Require(afterDeletion.Materials()[afterDeletionSlot].baseRoughness.x == 0.21f && !afterDeletion.Material(afterDeletionSlot)->ScriptSource().empty(),
        "删除全部预设实例后保存重开必须保留目录材质编辑并允许重新安装");
    const auto replacement = afterDeletion.CreatePresetObject(0, {});
    Require(afterDeletion.Registry().template get<gameplay::MeshRenderer>(replacement).materialIndex == afterDeletionSlot,
        "删除全部实例后重新放置必须使用保留的作者材质");
    std::filesystem::remove(deletedPath);

    // 模板专属覆盖材质也应在多个放置实例间共享，并在重开后复用已编辑的变体。
    auto overrideLibrary = library;
    overrideLibrary.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_material_override()->mutable_base_roughness()->set_w(0.25f);
    World variants; variants.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath())); variants.InstallObjectPresets(overrideLibrary);
    const auto variantFirst = variants.CreatePresetObject(0, {});
    const auto variantSecond = variants.CreatePresetObject(0, {});
    const auto variantSlot = variants.Registry().template get<gameplay::MeshRenderer>(variantFirst).materialIndex;
    Require(variants.Registry().template get<gameplay::MeshRenderer>(variantSecond).materialIndex == variantSlot,
        "模板材质覆盖的重复放置必须共享同一变体槽");
    variants.Materials()[variantSlot].baseRoughness.w = 0.37f;
    variants.Material(variantSlot)->ScriptSource() = "void Surface(inout Material m, float2 uv, float4 t) {}";
    World variantsRestored; variantsRestored.CreateDocument(variants.ExportDocument()); variantsRestored.InstallObjectPresets(overrideLibrary);
    const auto variantNew = variantsRestored.CreatePresetObject(0, {});
    const auto variantNewSlot = variantsRestored.Registry().template get<gameplay::MeshRenderer>(variantNew).materialIndex;
    Require(variantsRestored.Materials()[variantNewSlot].baseRoughness.w == 0.37f && !variantsRestored.Material(variantNewSlot)->ScriptSource().empty(),
        "覆盖材质经过保存重开后，新放置实例必须复用作者编辑结果");

    // 真实编辑历史仅保存普通组件，撤销和重做创建不重新安装几何或改变对象身份。
    render::RenderSettings settings;
    render::editor::EditorHistory history;
    auto selection = restored.CameraEntity();
    const auto historyBefore = render::editor::CaptureEditorDocument(restored, settings, selection);
    const auto created = restored.CreatePresetObject(1, finalPose);
    const auto identity = restored.Registry().template get<gameplay::ObjectIdentity>(created);
    selection = created;
    const auto sourceCount = restored.CookResourceScene().triangles.size();
    Require(history.Record(historyBefore, render::editor::CaptureEditorDocument(restored, settings, selection), "创建平面"),
        "预设创建必须可由通用组件历史记录");
    Require(history.Undo(restored, settings, selection) && !restored.Registry().valid(created), "撤销创建必须删除预设实体");
    Require(history.Redo(restored, settings, selection) && restored.Registry().valid(created) && selection == created,
        "重做创建必须恢复原实体句柄与选择");
    Require(restored.Registry().template get<gameplay::ObjectIdentity>(created) == identity &&
        restored.CookResourceScene().triangles.size() == sourceCount, "重做必须恢复稳定身份且不复制源资源");
    render::ValidateWorldDocument(restored.ExportDocument());

    // 旧导入入口在宿主观察之前提升为文档，正式源范围和预设范围随后保持互不重叠。
    World legacy; legacy.CreateCornell(render::BuildCornellScene());
    const auto legacyCount = legacy.CookResourceScene().triangles.size();
    legacy.InstallObjectPresets(library);
    Require(!legacy.ActiveSceneId().empty() && legacy.CookResourceScene().triangles.size() == legacyCount + 14 &&
        legacy.ObjectPresets()[0].firstTriangle >= legacyCount, "兼容场景必须先提升为文档再追加预设");
    legacy.CreatePresetObject(0, {});
    render::ValidateWorldDocument(legacy.ExportDocument());

    // 无网格的作者世界也能安装对象库，安装本身仍不能创建实例或灯光。
    render::WorldDocument empty;
    empty.set_id("empty-world"); empty.set_default_scene_id("empty-scene");
    auto* level = empty.add_levels(); level->set_id("empty-level"); level->add_scenes()->set_id("empty-scene");
    World blank; blank.CreateDocument(empty); blank.InstallObjectPresets(library);
    Require(blank.ExtractRenderSnapshot().instances.empty() && blank.ExtractRenderSnapshot().lights.empty(),
        "空世界安装对象库不能创建可见实例或活动光源");
    blank.CreatePresetObject(0, {}); blank.CreatePresetObject(2, {});
    render::ValidateWorldDocument(blank.ExportDocument());

    // 正式多光源 glTF 的非面光宽高 GPU 通道为零，提升为作者文档后仍须提供合法默认尺寸。
    const auto imported = render::ImportSceneFile(std::filesystem::path(RENDER_SOURCE_DIRECTORY) / "tests/fixtures/textured_multilight.gltf");
    Require(imported.lights.size() == 3 && imported.lights.front().tangentWidth.w == 0 && imported.lights.front().bitangentHeight.w == 0,
        "导入回归必须使用包含零面积通道的真实点光、聚光和方向光资源");
    World importedWorld; importedWorld.CreateScene(imported); importedWorld.InstallObjectPresets(library);
    const auto importedSnapshot = importedWorld.ExtractRenderSnapshot();
    Require(importedSnapshot.lights.size() == 3 && importedWorld.CookResourceScene().triangles.size() == imported.triangles.size() + 14,
        "多光源模型安装预设后必须保留原光源和网格，并只追加模板源范围");
    for (const auto& light : importedSnapshot.lights)
        Require(light.light.width > 0 && light.light.height > 0, "非面光的作者组件必须保留合法默认宽高");
    render::ValidateWorldDocument(importedWorld.ExportDocument());
    (void)render::ExtractRenderScene(importedWorld.CookResourceScene(), importedSnapshot);

    // 兼容导入调用者可以给非面光设置正尺寸，只有零值才表示应采用作者默认值。
    auto sizedPunctual = imported;
    for (auto& light : sizedPunctual.lights) {
        light.tangentWidth.w = 2 + light.positionType.w * 0.25f;
        light.bitangentHeight.w = 3 + light.positionType.w * 0.25f;
    }
    World sizedWorld; sizedWorld.CreateScene(sizedPunctual); sizedWorld.InstallObjectPresets(library);
    for (const auto& light : sizedWorld.ExtractRenderSnapshot().lights) {
        const auto type = static_cast<float>(light.light.type);
        Require(light.light.width == 2 + type * 0.25f && light.light.height == 3 + type * 0.25f,
            "非面光的已有正尺寸必须经过转换和文档提升原样保留");
    }

    // 面光尺寸是实际作者数据，合法值必须保持原样，非法零宽度不能被默认值掩盖。
    auto area = imported;
    area.lights.resize(1); area.lightNames.resize(1);
    area.lights[0].positionType.w = static_cast<float>(gameplay::LightType::Rectangle);
    area.lights[0].tangentWidth.w = 2.3f; area.lights[0].bitangentHeight.w = 1.2f;
    World areaWorld; areaWorld.CreateScene(area);
    Require(areaWorld.ExtractRenderSnapshot().lights.front().light.width == 2.3f &&
        areaWorld.ExtractRenderSnapshot().lights.front().light.height == 1.2f, "导入面光源的实际宽高必须原样保留");
    areaWorld.InstallObjectPresets(library);
    area.lights[0].tangentWidth.w = 0;
    World invalidArea; invalidArea.CreateScene(area);
    Require(invalidArea.ExtractRenderSnapshot().lights.front().light.width == 0, "无效面光宽度不能被非面光默认值覆盖");
    Rejects([&] { invalidArea.InstallObjectPresets(library); }, "无效面光尺寸必须在文档边界继续拒绝");
}
}

int main() {
    try {
        // 测试直接载入正式内容库，发布配置和运行时代码必须同时满足契约。
        auto library = render::LoadWorldDocument(std::filesystem::path(RENDER_SOURCE_DIRECTORY) / "Content/Objects/basic.textproto");
        gameplay::World world; world.CreateDocument(render::LoadWorldDocument(render::DefaultWorldPath()));
        Run(world, library);
        ExtraCases<gameplay::World>(library);
        std::cout << "对象预设资源、创建、边界与文档往返测试通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
