#include "asset/SceneImporter.h"
#include "gameplay/World.h"
#include "render/WorldDocument.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <windows.h>

namespace {
struct Options {
    std::filesystem::path source; // 用户提供的 FBX 导出目录，原始文件仅以只读方式消费。
    std::filesystem::path output; // 独立展示世界的正式 Protobuf 二进制路径。
    float height{1.65f}; // 展示主体的目标身高，保持所有局部比例不变。
    bool mainOnly{}; // 只生成主模型，便于单独检查模型与材质。
};

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::string Utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

Options Arguments(int argc, wchar_t** argv) {
    Options options;
    // 必须显式指定输入与输出，工具可在任意工作目录重现同一份作者配置。
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--main-only") options.mainOnly = true;
        else {
            Require(i + 1 < argc, "命令行参数缺少值");
            const std::wstring value = argv[++i];
            if (argument == L"--source") options.source = value;
            else if (argument == L"--output") options.output = value;
            else if (argument == L"--height") {
                std::size_t consumed{};
                options.height = std::stof(value, &consumed);
                Require(consumed == value.size() && std::isfinite(options.height) && options.height >= .1f && options.height <= 10,
                    "展示身高必须为 0.1 到 10 米之间的有限数值");
            } else throw std::invalid_argument("未知命令行参数");
        }
    }
    Require(!options.source.empty() && !options.output.empty(),
        "用法：citlali_scene_tool --source <FBX目录> --output <citlali.pb> [--height 1.65] [--main-only]");
    Require(options.output.extension() == L".pb", "大型模型世界必须使用 .pb 二进制配置");
    options.source = std::filesystem::absolute(options.source).lexically_normal();
    options.output = std::filesystem::absolute(options.output).lexically_normal();
    return options;
}

render::Aabb Bounds(const render::Scene& scene) {
    render::Aabb bounds;
    // 尺度来自实际解码几何，不依赖 FBX 文件名或导出软件的单位假设。
    for (const auto& triangle : scene.triangles)
        for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) bounds.Expand({vertex.x, vertex.y, vertex.z});
    Require(render::IsFinite(bounds.minimum) && render::IsFinite(bounds.maximum), "导入模型没有有限的可见几何");
    return bounds;
}

void Append(render::Scene& destination, render::Scene source, const std::string& label, float scale) {
    const auto bounds = Bounds(source);
    const render::Vec3 origin{(bounds.minimum.x + bounds.maximum.x) * .5f, bounds.minimum.y,
        (bounds.minimum.z + bounds.maximum.z) * .5f};
    const auto materialOffset = static_cast<std::uint32_t>(destination.materials.size());
    const auto triangleOffset = static_cast<std::uint32_t>(destination.triangles.size());
    const auto attributeOffset = static_cast<std::uint32_t>(destination.attributes.size());
    const auto textureOffset = static_cast<std::uint32_t>(destination.textureViews.size());
    const auto pixelOffset = static_cast<std::uint32_t>(destination.packedTexels.size());

    // 统一尺度并把枢轴置于模型底部；展示位姿留给 ECS 变换，整体旋转时不会绕场景原点公转。
    for (auto& triangle : source.triangles) {
        for (auto* vertex : {&triangle.a, &triangle.b, &triangle.c}) {
            const auto p = (render::Vec3{vertex->x, vertex->y, vertex->z} - origin) * scale;
            vertex->x = p.x; vertex->y = p.y; vertex->z = p.z;
        }
        triangle.a.w += static_cast<float>(materialOffset);
        if (triangle.b.w > 0) triangle.b.w += static_cast<float>(attributeOffset);
        triangle.c.w = 0;
        destination.triangles.push_back(triangle);
    }
    // 正的统一缩放不改变法线与切线，所有几何及纹理索引只做资源重定位。
    destination.attributes.insert(destination.attributes.end(), source.attributes.begin(), source.attributes.end());
    for (auto& material : source.materials) {
        for (auto* index : {&material.textures.x, &material.textures.y, &material.textures.z, &material.textures.w,
            &material.textures2.x, &material.textures2.y, &material.textures2.z, &material.textures2.w})
            if (*index != ~0u) *index += textureOffset;
        destination.materials.push_back(material);
    }
    for (std::size_t i = 0; i < source.materials.size(); ++i)
        destination.materialNames.push_back(label + " / " +
            (i < source.materialNames.size() ? source.materialNames[i] : std::to_string(i)));
    for (auto view : source.textureViews) { view.image.x += pixelOffset; destination.textureViews.push_back(view); }
    destination.packedTexels.insert(destination.packedTexels.end(), source.packedTexels.begin(), source.packedTexels.end());
    for (auto part : source.meshParts) {
        part.name = label;
        part.firstTriangle += triangleOffset;
        part.materialIndex += materialOffset;
        destination.meshParts.push_back(std::move(part));
    }
    std::cout << label << "：三角形 " << source.triangles.size() << "，模型 " << source.meshParts.size()
        << "，材质 " << source.materials.size() << "，纹理视图 " << source.textureViews.size()
        << "，原始边界 " << bounds.minimum.x << ',' << bounds.minimum.y << ',' << bounds.minimum.z
        << " / " << bounds.maximum.x << ',' << bounds.maximum.y << ',' << bounds.maximum.z
        << "，展示缩放 " << scale << '\n';
}

render::Vec3 Aim(render::Vec3 position, render::Vec3 target) {
    // 与引擎零旋转朝 +Z 的欧拉约定一致，避免相机朝向模型背后。
    const auto direction = render::Normalize(target - position);
    constexpr float degrees = 180.0f / 3.141592654f;
    return {-std::asin(std::clamp(direction.y, -1.0f, 1.0f)) * degrees,
        std::atan2(direction.x, direction.z) * degrees, 0};
}

void AddLighting(gameplay::World& world) {
    auto& registry = world.Registry();
    // 主光和补光从镜头侧照亮面部，背光分离发丝；不引入 Cornell 房间或封闭墙体。
    for (const auto entity : registry.view<gameplay::Light>()) registry.get<gameplay::Light>(entity).enabled = false;
    const auto add = [&](const char* name, render::Vec3 position, render::Vec3 color, float power, float size) {
        const auto entity = world.CreateEntity(name);
        registry.get<gameplay::Transform>(entity).position = position;
        auto& light = registry.emplace<gameplay::Light>(entity);
        light.type = gameplay::LightType::Rectangle;
        light.color = color; light.intensity = power; light.width = size; light.height = size;
        light.direction = render::Normalize(render::Vec3{0, .9f, 0} - position);
        light.tangent = render::Normalize(render::Cross(light.direction, {0, 1, 0}));
        // 发射框架要求 tangent × bitangent 与 direction 同向。
        light.bitangent = render::Normalize(render::Cross(light.direction, light.tangent));
    };
    add("主光：柔和白光", {-1.6f, 2.6f, 2.1f}, {1, .95f, .91f}, 7, 1.8f);
    add("补光：冷白", {2.4f, 1.8f, 1.1f}, {.76f, .85f, 1}, 4, 1.2f);
    add("轮廓光", {2.8f, 3.5f, -1.5f}, {1, .87f, .97f}, 8, .8f);
}

void AddGround(render::WorldDocument& document) {
    auto* material = document.add_materials();
    material->set_id("display.ground-material"); material->set_name("展示地面");
    render::GpuMaterial parameters;
    parameters.baseRoughness = {.24f, .28f, .29f, .8f};
    parameters.transmissionIor.y = 1.5f;
    render::WriteMaterialParameters(parameters, *material->mutable_parameters());
    auto* mesh = document.add_meshes();
    mesh->set_id("display.ground-mesh"); mesh->set_name("开放展示地面");
    // 有限开放平面既接收软阴影，也保留地平线和角色轮廓。
    auto* quad = mesh->mutable_quad();
    render::WriteVector({-8, -.003f, -8}, *quad->mutable_a());
    render::WriteVector({-8, -.003f, 8}, *quad->mutable_b());
    render::WriteVector({8, -.003f, 8}, *quad->mutable_c());
    render::WriteVector({8, -.003f, -8}, *quad->mutable_d());
    auto* object = document.mutable_levels(0)->mutable_scenes(0)->add_objects();
    object->set_id("display.ground"); object->set_name("地面");
    object->set_mesh_id(mesh->id()); object->set_material_id(material->id());
}

void Externalize(render::WorldDocument& document, const std::filesystem::path& output) {
    const auto directory = output.parent_path().parent_path() / L"Models" / L"Citlali";
    std::filesystem::create_directories(directory);
    // 保留源 PNG 对应的完整解码像素，不降采样、不重新压缩，也不修改用户提供的模型文件。
    for (int i = 0; i < document.textures_size(); ++i) {
        auto& texture = *document.mutable_textures(i);
        // 内容寻址避免再次生成时破坏旧世界引用的图像；已有文件必须逐像素相同才能复用。
        std::uint64_t hash = 14695981039346656037ull;
        const auto hashWord = [&](std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8) { hash ^= (value >> shift) & 255u; hash *= 1099511628211ull; }
        };
        hashWord(texture.width()); hashWord(texture.height());
        for (const auto pixel : texture.rgba()) hashWord(pixel);
        std::ostringstream name;
        name << "image-" << std::hex << std::setw(16) << std::setfill('0') << hash << ".rgba";
        const auto path = directory / name.str();
        if (std::filesystem::exists(path)) {
            Require(std::filesystem::file_size(path) == std::uint64_t(texture.rgba_size()) * 4,
                "已有内容寻址纹理长度不同，拒绝覆盖");
            std::ifstream existing(path, std::ios::binary);
            std::array<std::uint32_t, 16384> chunk{};
            for (int first = 0; first < texture.rgba_size();) {
                const auto size = std::min<int>(static_cast<int>(chunk.size()), texture.rgba_size() - first);
                Require(bool(existing.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(size) * 4)),
                    "已有内容寻址纹理读取失败");
                for (int index = 0; index < size; ++index) {
                    auto pixel = chunk[index];
                    if constexpr (std::endian::native != std::endian::little)
                        pixel = (pixel >> 24) | ((pixel >> 8) & 0xff00u) | ((pixel << 8) & 0xff0000u) | (pixel << 24);
                    if (pixel != texture.rgba(first + index)) throw std::runtime_error("已有纹理与内容哈希不一致，拒绝覆盖");
                }
                first += size;
            }
            texture.set_rgba_path(Utf8(path));
            continue;
        }
        auto temporary = path;
        temporary += ".tmp." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(i);
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        Require(bool(file), "无法创建模型纹理载荷");
        if constexpr (std::endian::native == std::endian::little) {
            file.write(reinterpret_cast<const char*>(texture.rgba().data()), static_cast<std::streamsize>(texture.rgba_size()) * 4);
        } else {
            for (const auto pixel : texture.rgba()) {
                const std::array<char, 4> bytes{static_cast<char>(pixel), static_cast<char>(pixel >> 8),
                    static_cast<char>(pixel >> 16), static_cast<char>(pixel >> 24)};
                file.write(bytes.data(), bytes.size());
            }
        }
        Require(bool(file.flush()), "模型纹理载荷写入不完整");
        file.close();
        Require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
            "模型纹理载荷原子替换失败");
        texture.set_rgba_path(Utf8(path));
        std::cout << "纹理 " << i << "：" << texture.width() << 'x' << texture.height() << "，"
            << std::uint64_t(texture.rgba_size()) * 4 << " 字节\n";
    }
}
} // 匿名命名空间

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = Arguments(argc, argv);
        render::WorldDocument document;
        {
            gameplay::World world;
            {
                render::Scene combined;
                std::vector<std::tuple<std::string, render::Vec3, render::Vec3>> placements;
                combined.sourcePath = Utf8(options.source);
                auto main = render::ImportSceneFile(options.source / L"茜特拉莉_夏日.fbx");
                const auto bounds = Bounds(main);
                const auto height = bounds.maximum.y - bounds.minimum.y;
                Require(height > .0001f, "主模型高度过小，无法设置展示尺度");
                Append(combined, std::move(main), "茜特拉莉_夏日", options.height / height);
                if (!options.mainOnly) {
                    // 每个 FBX 作为独立完整模型摆放，书的俯仰让正面相机也能看到封面。
                    for (const auto& [filename, label, extent, position, rotation] :
                        {std::tuple{L"茜特菈琳.fbx", "茜特菈琳", .86f, render::Vec3{-1.03f, .22f, 0}, render::Vec3{}},
                         std::tuple{L"书.fbx", "书", .38f, render::Vec3{.93f, .77f, .06f}, render::Vec3{65, -18, 0}},
                         std::tuple{L"杯.fbx", "杯", .24f, render::Vec3{.68f, 0, .38f}, render::Vec3{0, 12, 0}},
                         std::tuple{L"武器.fbx", "武器", .82f, render::Vec3{1.15f, .02f, .03f}, render::Vec3{0, -14, 0}}}) {
                        auto accessory = render::ImportSceneFile(options.source / filename);
                        const auto box = Bounds(accessory);
                        const auto size = box.maximum - box.minimum;
                        Append(combined, std::move(accessory), label, extent / std::max({size.x, size.y, size.z}));
                        placements.emplace_back(label, position, rotation);
                    }
                }
                // 实际 FBX 的角色正面朝 +Z；相机从正面拍摄，并避开轮廓灯面板。
                combined.cameras.push_back({"角色展示相机", {0, 1.1f, 4.8f}, Aim({0, 1.1f, 4.8f}, {0, .86f, 0}), 36, .03f, 50, true});
                world.CreateScene(combined);
                // 位姿是作者数据，编辑器可直接选择并移动整个模型而不改写源几何。
                for (const auto entity : world.Registry().view<gameplay::Name, gameplay::Transform>())
                    for (const auto& [label, position, rotation] : placements)
                        if (world.Registry().get<gameplay::Name>(entity).value == label) {
                            auto& transform = world.Registry().get<gameplay::Transform>(entity);
                            transform.position = position;
                            transform.rotationDegrees = rotation;
                        }
            }
            AddLighting(world);
            document = world.ExportDocument();
        }
        document.set_id("citlali-world"); document.set_name("茜特拉莉角色展示");
        document.set_default_scene_id("citlali-display");
        document.mutable_levels(0)->set_id("citlali-level");
        document.mutable_levels(0)->set_name("角色展示关卡");
        document.mutable_levels(0)->mutable_scenes(0)->set_id("citlali-display");
        document.mutable_levels(0)->mutable_scenes(0)->set_name("茜特拉莉 · 夏日");
        AddGround(document);
        std::filesystem::create_directories(options.output.parent_path());
        Externalize(document, options.output);
        render::SaveWorldDocument(document, options.output);
        const auto triangles = document.meshes(0).data().triangles_size();
        document.Clear();
        // 最终产物通过正式加载和引用校验读回，缺失载荷或错误配置不会被报告为生成成功。
        const auto loaded = render::LoadWorldDocument(options.output);
        Require(loaded.meshes(0).data().triangles_size() == triangles && loaded.textures_size() > 0,
            "保存后的角色场景未完整读回");
        // 真实资产也验证文件级模型边界，防止生成工具重新按材质拆散实体。
        std::size_t models = 0;
        std::size_t surfaces = 0;
        for (const auto& object : render::FindDocumentScene(loaded, {}).objects())
            if (!object.surface_materials().empty()) { ++models; surfaces += object.surface_materials_size(); }
        Require(models == (options.mainOnly ? 1u : 5u), "生成世界的完整模型数量不符合输入文件数量");
        std::cout << "已生成 " << Utf8(options.output) << "，模型三角形 " << triangles
            << "，完整模型 " << models << "，材质表面 " << surfaces << "，图像 " << loaded.textures_size()
            << "，二进制配置 " << std::filesystem::file_size(options.output) << " 字节\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "角色场景生成失败：" << error.what() << '\n';
        return 1;
    }
}
