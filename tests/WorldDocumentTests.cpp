#include "render/WorldDocument.h"
#include "gameplay/World.h"
#include "render/SceneExtraction.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char *message) {
    // 文档载入与场景切换必须验证真实状态，而非仅检查解析函数返回值。
    if (!condition)
        throw std::runtime_error(message);
}
template <class Function> void Rejects(Function function, const char *message) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void WholeModelsAndAdditiveImport() {
    // 两个颜色与纹理不同的表面必须共享一个模型身份和变换。
    render::Scene model;
    model.materials.resize(2);
    model.materials[0].baseRoughness = {1, 0, 0, .5f};
    model.materials[1].baseRoughness = {0, 1, 0, .5f};
    model.materialNames = {"红色表面", "绿色表面"};
    model.packedTexels = {0xffffffffu};
    render::GpuTextureView image;
    image.image = {0, 1, 1, 0};
    model.textureViews.push_back(image);
    model.materials[0].textures.x = 0;
    model.triangles = {{{0, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}},
                       {{2, 0, 0, 1}, {3, 0, 0, 0}, {2, 1, 0, 0}}};
    model.meshParts = {{"完整模型", 0, 2, 0, {}, true}};
    gameplay::World world;
    world.CreateScene(model);
    auto snapshot = world.ExtractRenderSnapshot();
    Require(snapshot.instances.size() == 1 && snapshot.instances[0].mesh.useSurfaceMaterials,
            "多个材质表面必须只创建一个模型实体");
    const auto entity = snapshot.instances[0].entity;
    world.Registry().get<gameplay::Transform>(entity).position = {2, 3, 4};
    for (auto light : world.Registry().view<gameplay::Light>()) world.Registry().get<gameplay::Light>(light).enabled = false;
    std::vector<gameplay::Entity> owners;
    auto rendered = render::ExtractRenderScene(world.CookResourceScene(), world.ExtractRenderSnapshot(), false, &owners);
    Require(rendered.triangles.size() == 2 && rendered.triangles[0].a.w != rendered.triangles[1].a.w &&
                rendered.triangles[0].a.x == 2 && rendered.triangles[1].a.x == 4 && owners[0] == entity && owners[1] == entity,
            "整体移动与拾取必须覆盖所有表面且不覆盖各自材质");

    // 模型副本共享不可变网格，导出后删除原实例仍必须保留所有表面。
    const auto duplicate = world.CreateEntity("模型副本");
    world.Registry().emplace<gameplay::MeshRenderer>(duplicate, world.Registry().get<gameplay::MeshRenderer>(entity));
    world.Registry().get<gameplay::Transform>(duplicate).position.x = -4;
    rendered = render::ExtractRenderScene(world.CookResourceScene(), world.ExtractRenderSnapshot());
    Require(rendered.triangles.size() == 4 && rendered.triangles[2].a.w != rendered.triangles[3].a.w,
            "整体模型副本必须保留全部材质");
    auto stored = world.ExportDocument();
    unsigned models = 0;
    for (const auto& object : render::FindDocumentScene(stored, {}).objects())
        if (!object.mesh_id().empty()) {
            ++models;
            Require(object.surface_materials_size() == 2, "完整模型必须保存两个表面绑定");
        }
    Require(models == 2, "保存不能再次将材质分面拆成独立模型");
    // 整体覆盖只改变实例的显示方式，保存后仍应能恢复原来的多材质表面。
    auto& mesh = world.Registry().get<gameplay::MeshRenderer>(entity);
    mesh.useSurfaceMaterials = false;
    mesh.materialIndex = 1;
    gameplay::World overridden;
    overridden.CreateDocument(world.ExportDocument());
    bool restoredOverride = false;
    for (const auto instance : overridden.ExtractRenderSnapshot().instances) {
        if (overridden.Registry().get<gameplay::Name>(instance.entity).value != "完整模型") continue;
        auto& restored = overridden.Registry().get<gameplay::MeshRenderer>(instance.entity);
        Require(!restored.useSurfaceMaterials, "整体材质覆盖状态必须在保存后保留");
        const auto source = overridden.CookResourceScene();
        Require(source.triangles[restored.firstTriangle].a.w != source.triangles[restored.firstTriangle + 1].a.w,
                "覆盖模式不能丢弃原始模型表面材质");
        restored.useSurfaceMaterials = true;
        restoredOverride = true;
    }
    Require(restoredOverride, "保存后的整体模型必须仍可恢复表面材质");
    mesh.useSurfaceMaterials = true;
    // 删除原对象后，副本必须仍通过源范围恢复网格和完整表面，而不依赖原对象存活。
    world.DestroyEntity(entity);
    gameplay::World cloneOnly;
    cloneOnly.CreateDocument(world.ExportDocument());
    Require(cloneOnly.ExtractRenderSnapshot().instances.size() == 1 &&
                cloneOnly.ExtractRenderSnapshot().instances[0].mesh.useSurfaceMaterials,
            "删除原模型后保存副本仍应保留完整模型结构");

    // 同一共享材质也可在两个表面拥有各自参数、脚本和纹理，不能按材质标识误合并。
    auto variants = stored;
    auto* variantObject = variants.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0);
    while (variantObject->mesh_id().empty()) {
        variants.mutable_levels(0)->mutable_scenes(0)->mutable_objects()->DeleteSubrange(0, 1);
        variantObject = variants.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0);
    }
    variantObject->set_name("独立表面覆盖模型");
    variantObject->set_use_surface_materials(false);
    variantObject->mutable_material_override()->mutable_base_roughness()->set_x(.95f);
    variantObject->set_material_script_source("void Surface(inout Material m, float2 uv, float4 t) { m.baseRoughness.x = .95; }");
    variantObject->set_material_script_path("whole.hlsl");
    const auto sharedMaterial = variantObject->material_id();
    for (int i = 0; i < 2; ++i) {
        auto* surface = variantObject->mutable_surface_materials(i);
        surface->set_material_id(sharedMaterial);
        surface->mutable_material_override()->mutable_base_roughness()->set_x(i ? .65f : .15f);
        surface->set_material_script_source(i ? "green surface script" : "red surface script");
        surface->set_material_script_path(i ? "green.hlsl" : "red.hlsl");
        if (surface->texture_bindings().empty())
            *surface->add_texture_bindings() = variantObject->surface_materials(0).texture_bindings(0);
    }
    const auto verifyVariants = [&](const gameplay::World& current) {
        const auto source = current.CookResourceScene();
        for (const auto& instance : current.ExtractRenderSnapshot().instances) {
            if (current.Registry().get<gameplay::Name>(instance.entity).value != "独立表面覆盖模型") continue;
            const auto first = static_cast<unsigned>(source.triangles[instance.mesh.firstTriangle].a.w);
            const auto second = static_cast<unsigned>(source.triangles[instance.mesh.firstTriangle + 1].a.w);
            Require(first != second && current.Material(first)->Parameters().baseRoughness.x == .15f &&
                        current.Material(second)->Parameters().baseRoughness.x == .65f,
                    "同一共享材质的两个表面覆盖必须保持独立参数");
            Require(current.Material(first)->ScriptSource() == "red surface script" &&
                        current.Material(second)->ScriptSource() == "green surface script" &&
                        current.Material(first)->ScriptPath() == "red.hlsl" && current.Material(second)->ScriptPath() == "green.hlsl" &&
                        current.Material(first)->Texture(0) && current.Material(second)->Texture(0),
                    "表面独立脚本、来源和纹理必须在保存及追加后保留");
            Require(!instance.mesh.useSurfaceMaterials && current.Material(instance.mesh.materialIndex)->ScriptPath() == "whole.hlsl" &&
                        current.Material(instance.mesh.materialIndex)->Parameters().baseRoughness.x == .95f,
                    "整体覆盖必须保持独立且不能污染原始表面");
            return;
        }
        throw std::runtime_error("独立表面覆盖模型丢失");
    };
    gameplay::World variantWorld;
    variantWorld.CreateDocument(variants);
    verifyVariants(variantWorld);
    variantWorld.CreateDocument(variantWorld.ExportDocument());
    verifyVariants(variantWorld);
    variantWorld.AppendScene(model);
    verifyVariants(variantWorld);

    // 范围元数据来自文件，空洞、重叠、不完整覆盖和失效引用都必须在创建前拒绝。
    auto malformed = variants;
    malformed.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials(1)->set_first_triangle(0);
    Rejects([&] { render::ValidateWorldDocument(malformed); }, "表面重叠必须拒绝");
    malformed = variants;
    malformed.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials(1)->set_first_triangle(2);
    Rejects([&] { render::ValidateWorldDocument(malformed); }, "表面空洞必须拒绝");
    malformed = variants;
    malformed.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials()->RemoveLast();
    Rejects([&] { render::ValidateWorldDocument(malformed); }, "未覆盖完整模型的表面必须拒绝");
    malformed = variants;
    malformed.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_surface_materials(0)->set_material_id("missing");
    Rejects([&] { render::ValidateWorldDocument(malformed); }, "表面材质失效引用必须拒绝");

    gameplay::World reload;
    reload.CreateDocument(stored);
    Require(reload.ExtractRenderSnapshot().instances.size() == 2, "重载必须保留模型数量");
    const auto before = reload.ExportDocument();

    // 连续导入追加模型，原有相机、场景身份与作者编辑必须保持。
    reload.AppendScene(model);
    Require(reload.ExtractRenderSnapshot().instances.size() == 3 && reload.ActiveSceneId() == before.default_scene_id(),
            "导入必须追加到当前场景而不是替换已有模型");
    const auto after = reload.ExportDocument();
    for (const auto& original : render::FindDocumentScene(before, {}).objects()) {
        bool found = false;
        for (const auto& object : render::FindDocumentScene(after, {}).objects())
            if (object.id() == original.id()) {
                found = true;
                Require(object.transform().SerializeAsString() == original.transform().SerializeAsString(), "追加导入必须保留已有对象变换");
            }
        Require(found, "追加导入必须保留已有对象及相机身份");
    }
    reload.AppendScene(model);
    Require(reload.ExtractRenderSnapshot().instances.size() == 4, "重复导入必须生成独立模型实例而不产生标识冲突");
    const auto revision = reload.Revision();
    const auto intact = reload.ExportDocument().SerializeAsString();
    auto invalid = model;
    invalid.triangles.clear();
    Rejects([&] { reload.AppendScene(invalid); }, "无效模型追加必须失败");
    Require(reload.Revision() == revision && reload.ExportDocument().SerializeAsString() == intact,
            "追加失败必须保留全部已有模型与编辑");
}
} // 匿名命名空间

int main() {
    try {
        WholeModelsAndAdditiveImport();
        // 发布的文本配置必须独立生成原有 Cornell 几何，并保留第二个可激活场景。
        auto document = render::LoadWorldDocument(render::DefaultWorldPath());
        Require(document.levels_size() > 0 && document.levels(0).scenes_size() >= 2, "默认配置必须包含多个场景");
        const auto scene = render::BakeScene(document, "cornell");
        Require(scene.triangles.size() == 36 && scene.materials.size() == 6, "Cornell 原语必须保留既有几何与材质数量");
        Require(scene.meshParts.size() == 8 && !scene.attributes.empty(), "配置对象必须生成可追踪的网格范围与 UV 属性");
        const auto laboratory =
            render::LoadWorldDocument(render::DefaultWorldPath().parent_path() / "material-lab.textproto");
        Require(render::BakeScene(laboratory, "material-lab").triangles.size() == 14,
                "独立第二份文档必须可通过相同原语工厂加载");

        // 使用引擎专属临时路径验证文本与二进制往返，避免覆盖旧版测试文档。
        const auto base = std::filesystem::temp_directory_path() / "EncoreWorldDocumentTest";
        const auto textPath = base.string() + ".textproto";
        const auto binaryPath = base.string() + ".pb";
        const auto uppercasePath = base.string() + ".PB";
        render::SaveWorldDocument(document, textPath);
        render::SaveWorldDocument(document, binaryPath);
        render::SaveWorldDocument(document, uppercasePath);
        Require(render::LoadWorldDocument(textPath).SerializeAsString() == document.SerializeAsString(),
                "文本往返不能丢失字段");
        Require(render::LoadWorldDocument(binaryPath).SerializeAsString() == document.SerializeAsString(),
                "二进制往返不能丢失字段");
        Require(render::LoadWorldDocument(uppercasePath).SerializeAsString() == document.SerializeAsString(),
                "大写二进制扩展名必须与文件浏览器的识别规则一致");

        // 导入导出阶段允许完整大图留在内存，未外置的大载荷仍不能越过实际文件长度限制。
        {
            auto large = document;
            auto* texture = large.add_textures();
            texture->set_id("large-import-image");
            texture->set_width(4096); texture->set_height(4096);
            texture->mutable_rgba()->Resize(4096 * 4096, 0xffbbaa99u);
            Require(large.ByteSizeLong() > 64u * 1024u * 1024u, "大图回归必须真正超过六十四 MiB");
            render::ValidateWorldDocument(large);
            Rejects([&] { render::SaveWorldDocument(large, binaryPath); }, "未外置的完整像素不能写入超限配置");
            Require(render::LoadWorldDocument(binaryPath).SerializeAsString() == document.SerializeAsString(),
                "超限图像保存失败必须保留已有文档");
        }

        // 所有层级标识、对象资源引用、有限数值和源路径都在替换当前世界之前校验。
        auto invalid = document;
        *invalid.mutable_levels(0)->mutable_scenes(0)->add_objects() = invalid.levels(0).scenes(0).objects(0);
        Rejects([&] { render::ValidateWorldDocument(invalid); }, "重复对象标识必须被拒绝");
        invalid = document;
        invalid.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->set_material_id("missing-material");
        Rejects([&] { render::ValidateWorldDocument(invalid); }, "不存在的材质引用必须被拒绝");
        invalid = document;
        invalid.mutable_levels(0)
            ->mutable_scenes(0)
            ->mutable_objects(0)
            ->mutable_transform()
            ->mutable_position()
            ->set_x(std::nanf(""));
        Rejects([&] { render::ValidateWorldDocument(invalid); }, "非有限变换必须被拒绝");
        invalid = document;
        invalid.mutable_materials(0)->set_script_path(std::string("bad\0path", 8));
        Rejects([&] { render::ValidateWorldDocument(invalid); }, "包含空字符的路径必须被拒绝");
        render::SaveWorldDocument(document, binaryPath);
        Rejects([&] { render::SaveWorldDocument(invalid, binaryPath); }, "无效编辑不能覆盖已保存文档");
        Require(render::LoadWorldDocument(binaryPath).SerializeAsString() == document.SerializeAsString(),
                "失败保存必须保持已有文件完整");

        // 同一基础材质的对象可绑定不同图像，不能因共享目录而串改采样状态。
        auto textured = document;
        for (int i = 0; i < 2; ++i) {
            auto *texture = textured.add_textures();
            texture->set_id("test-image-" + std::to_string(i));
            texture->set_name("测试图像");
            texture->set_width(1);
            texture->set_height(1);
            texture->add_rgba(i == 0 ? 0xff0000ffu : 0xff00ff00u);
            auto *binding = textured.mutable_levels(0)->mutable_scenes(0)->mutable_objects(i)->add_texture_bindings();
            binding->set_slot(0);
            binding->set_texture_id(texture->id());
        }
        const auto texturedScene = render::BakeScene(textured, "cornell");
        Require(texturedScene.materials.size() == 8 && texturedScene.materials[0].textures.x == ~0u &&
                    texturedScene.materials[6].textures.x != texturedScene.materials[7].textures.x,
                "对象纹理必须生成独立采样视图，不能污染共享基础材质");
        gameplay::World texturedWorld;
        texturedWorld.CreateDocument(textured, "cornell");
        texturedWorld.Material(6)->ScriptSource() = "void Surface(inout Material m, float2 uv, float4 t) {}";
        const auto texturedExport = texturedWorld.ExportDocument();
        gameplay::World texturedReload;
        texturedReload.CreateDocument(texturedExport, "cornell");
        Require(!texturedReload.Material(6)->ScriptSource().empty() &&
                    texturedReload.Material(7)->ScriptSource().empty() &&
                    texturedReload.Material(6)->Texture(0)->texture != texturedReload.Material(7)->Texture(0)->texture,
                "对象材质脚本与图像关联必须分别往返，不影响同基础材质的其他对象");
        auto samePixels = textured;
        samePixels.mutable_textures(1)->set_rgba(0, samePixels.textures(0).rgba(0));
        gameplay::World samePixelWorld;
        samePixelWorld.CreateDocument(samePixels, "cornell");
        const auto samePixelExport = samePixelWorld.ExportDocument();
        for (const auto &object : render::FindDocumentScene(samePixelExport, "cornell").objects()) {
            if (object.id() == "cornell.floor")
                Require(object.texture_bindings(0).texture_id() == "test-image-0",
                        "相同像素图像必须保留第一个作者标识");
            if (object.id() == "cornell.ceiling")
                Require(object.texture_bindings(0).texture_id() == "test-image-1",
                        "相同像素图像不能合并第二个作者标识");
        }

        // 外置纹理在独立测试目录写入小端 RGBA8，验证中文路径、完整内存图像和对象关联。
        const auto externalRoot = std::filesystem::temp_directory_path() / L"EncoreWorldExternal_测试";
        const auto externalWorlds = externalRoot / "Worlds";
        const auto externalModels = externalRoot / "Models";
        std::filesystem::create_directories(externalWorlds / "Saved");
        std::filesystem::create_directories(externalModels);
        const auto rawPath = externalModels / L"图像.rgba";
        const auto writeRaw = [&](std::string bytes) {
            std::ofstream file(rawPath, std::ios::binary | std::ios::trunc);
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        writeRaw(std::string("\xff\0\0\xff", 4));
        auto external = textured;
        external.mutable_textures(0)->set_rgba_path("../Models/图像.rgba");
        const auto externalPath = externalWorlds / "character.pb";
        render::SaveWorldDocument(external, externalPath);
        render::WorldDocument storedExternal;
        {
            std::ifstream input(externalPath, std::ios::binary);
            Require(storedExternal.ParseFromIstream(&input), "外置纹理文档必须仍为正式 Protobuf 二进制");
        }
        Require(storedExternal.textures(0).rgba_size() == 0 && storedExternal.textures(1).rgba_size() == 1 &&
            storedExternal.textures(0).rgba_path() == "../Models/图像.rgba",
            "外置与内嵌图像必须兼容，保存文件不能重复写入外置纹素");
        const auto hydrated = render::LoadWorldDocument(externalPath);
        Require(hydrated.textures(0).rgba(0) == 0xff0000ffu &&
            render::BakeScene(hydrated, "cornell").packedTexels.front() == 0xff0000ffu,
            "小端原始像素必须在真实场景烘焙前恢复颜色与透明度");
        gameplay::World externalWorld;
        externalWorld.CreateDocument(hydrated);
        const auto savedElsewhere = externalWorlds / "Saved" / "character.textproto";
        render::SaveWorldDocument(externalWorld.ExportDocument(), savedElsewhere);
        Require(render::LoadWorldDocument(savedElsewhere).textures(0).rgba(0) == 0xff0000ffu,
            "另存为必须重算 sidecar 相对路径并保留对象纹理引用");

        // 错误载荷不能越过内存预算，也不能以保存操作静默覆盖已经验证的图像数据。
        writeRaw(std::string("\0\xff\0\xff", 4));
        Rejects([&] { render::SaveWorldDocument(hydrated, externalPath); }, "已变化的外置像素不能覆盖当前作者数据");
        writeRaw(std::string("\xff\0\0", 3));
        Rejects([&] { render::LoadWorldDocument(externalPath); }, "截断 RGBA 载荷必须拒绝");
        writeRaw(std::string("\xff\0\0\xff\0", 5));
        Rejects([&] { render::LoadWorldDocument(externalPath); }, "附加 RGBA 字节必须拒绝");
        std::filesystem::remove(rawPath);
        Rejects([&] { render::LoadWorldDocument(externalPath); }, "缺失 RGBA 载荷必须明确失败");
        storedExternal.mutable_textures(0)->set_width(~0u);
        {
            std::ofstream output(externalPath, std::ios::binary | std::ios::trunc);
            Require(storedExternal.SerializeToOstream(&output), "尺寸边界测试配置必须可编码");
        }
        Rejects([&] { render::LoadWorldDocument(externalPath); }, "畸形尺寸必须在图像内存分配之前拒绝");
        std::filesystem::remove_all(externalRoot);

        // 兼容模型入口也能提升为文档，并以嵌入载荷独立恢复几何。
        gameplay::World importedWorld;
        importedWorld.CreateCornell(scene);
        const auto importedDocument = importedWorld.ExportDocument();
        gameplay::World importedReload;
        importedReload.CreateDocument(importedDocument);
        Require(importedReload.CookResourceScene().triangles.size() == scene.triangles.size(),
                "旧导入入口必须可无损提升为嵌入网格文档");

        // 切换场景先保存当前对象编辑，并使用不同版本的 EnTT 句柄创建新场景实体。
        gameplay::World world;
        world.CreateDocument(document, "cornell");
        const auto extracted = render::ExtractRenderScene(world.CookResourceScene(), world.ExtractRenderSnapshot());
        std::size_t taggedEmitters = 0;
        for (const auto &triangle : extracted.triangles)
            if (triangle.c.w > 0)
                ++taggedEmitters;
        Require(taggedEmitters == 2 && extracted.lights.size() == 1 && extracted.triangles.size() == 38,
                "配置灯板必须只保留一份具有正确 MIS 标签的可见解析发光体");
        const auto originalCamera = world.CameraEntity();
        gameplay::Entity edited = entt::null;
        for (auto entity : world.Registry().view<gameplay::ObjectIdentity>())
            if (world.Registry().get<gameplay::ObjectIdentity>(entity).objectId == "cornell.small-box")
                edited = entity;
        Require(edited != entt::null, "配置对象必须以稳定标识映射到 ECS 实体");
        world.Registry().get<gameplay::Transform>(edited).position.x += 0.2f;
        auto &mesh = world.Registry().get<gameplay::MeshRenderer>(edited);
        mesh.materialIndex = 1;
        world.Registry().emplace<gameplay::LuaScript>(edited, "function update(self, dt) end", "custom.lua", true);
        world.Material(1)->ScriptSource() = "void Surface(inout Material m, float2 uv, float4 t) {}";
        world.MarkDirty();
        const auto beforeFailure = world.Revision();
        Rejects([&] { world.CreateDocument(invalid, "cornell"); }, "无效文档不能替换当前世界");
        Require(world.Revision() == beforeFailure && world.Registry().valid(edited), "载入失败必须保留当前实体及编辑");
        world.SwitchScene("material-lab");
        Require(world.ActiveSceneId() == "material-lab" && !world.Registry().valid(originalCamera),
                "场景切换必须使旧实体句柄失效");
        world.SwitchScene("cornell");
        bool restored = false;
        for (auto entity : world.Registry().view<gameplay::ObjectIdentity>()) {
            const auto &identity = world.Registry().get<gameplay::ObjectIdentity>(entity);
            if (identity.objectId == "cornell.small-box") {
                restored = true;
                Require(std::abs(world.Registry().get<gameplay::Transform>(entity).position.x + 0.37f) < 0.0001f,
                        "对象变换必须在切回场景后恢复");
                Require(world.Registry().get<gameplay::MeshRenderer>(entity).materialIndex == 1 &&
                            world.Registry().all_of<gameplay::LuaScript>(entity),
                        "材质关联与 Lua 组件必须跨场景保存");
            }
        }
        Require(restored && !world.Material(1)->ScriptSource().empty(), "共享材质脚本必须保留权威编辑结果");
        const auto exported = world.ExportDocument();
        render::SaveWorldDocument(exported, binaryPath);
        Require(render::LoadWorldDocument(binaryPath).SerializeAsString() == exported.SerializeAsString(),
                "编辑后的文档必须可保存并重新读取");
        std::filesystem::remove(textPath);
        std::filesystem::remove(binaryPath);
        std::filesystem::remove(uppercasePath);
        std::cout << "世界文档的 Protobuf、引用校验、场景切换和编辑往返测试通过\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
