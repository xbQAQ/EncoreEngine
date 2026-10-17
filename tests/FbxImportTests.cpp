#include "asset/FbxImporter.h"
#include "asset/SceneImporter.h"
#include "gameplay/World.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace {
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool Near(float a, float b) { return std::abs(a - b) < .0001f; }
template <class Function> void Rejects(Function function, const char *message) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    Require(bool(input), "测试源文件必须存在");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    Require(bool(output), "测试派生文件写入失败");
}
void Replace(std::string &text, const std::string &before, const std::string &after) {
    const auto at = text.find(before);
    Require(at != std::string::npos, "测试变体必须命中预期源字段");
    text.replace(at, before.size(), after);
}
std::filesystem::path Fixture() {
#ifdef RENDER_SOURCE_DIRECTORY
    return std::filesystem::path(RENDER_SOURCE_DIRECTORY) / "tests/fixtures/two_materials.fbx";
#elif defined(FBX_FIXTURE_DIRECTORY)
    return std::filesystem::path(FBX_FIXTURE_DIRECTORY) / "two_materials.fbx";
#else
    return std::filesystem::path("tests/fixtures/two_materials.fbx");
#endif
}
void SavePng(const std::filesystem::path &path) {
    using Microsoft::WRL::ComPtr;
    const auto check = [](HRESULT hr) { Require(SUCCEEDED(hr), "测试 PNG 编码失败"); };
    // 使用正式 WIC 编码生成外部测试图像，避免依赖额外图片程序或手写 PNG 编码器。
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    check(factory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(2, 1));
    auto format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    Require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "测试编码器必须保留四通道图像");
    std::uint32_t pixels[]{0xffff0000u, 0x8000ff00u};
    check(frame->WritePixels(1, 8, sizeof(pixels), reinterpret_cast<BYTE *>(pixels)));
    check(frame->Commit());
    check(encoder->Commit());
}
void CheckGeometry(const render::Scene &scene) {
    // 真实 FBX 的两个四边形必须经官方三角化得到四片，并保持每片材质关联。
    Require(scene.triangles.size() == 4 && scene.attributes.size() == 4 && scene.meshParts.size() == 2,
            "FBX 四边形与双材质范围必须完整导入");
    Require(scene.materials.size() == 3 && scene.materialNames[0] == "TexturedPhong" &&
                scene.materialNames[1] == "GreenLambert",
            "FBX 材质及独立默认槽必须保留");
    float minimumX = 100, maximumX = -100;
    for (const auto &part : scene.meshParts) {
        Require(part.triangleCount == 2, "每个作者多边形必须保留完整材质分组");
        for (std::size_t i = part.firstTriangle; i < part.firstTriangle + part.triangleCount; ++i) {
            const auto &t = scene.triangles[i];
            Require(t.a.w == static_cast<float>(part.materialIndex), "每片三角形必须关联相同材质槽");
            for (const auto &v : {t.a, t.b, t.c}) {
                if (!Near(v.z, 3.5f) || v.y < 2 || v.y > 3)
                    throw std::runtime_error("祖先变换与几何专用变换必须同时生效，实际坐标：" + std::to_string(v.x) +
                                             "," + std::to_string(v.y) + "," + std::to_string(v.z));
                minimumX = std::min(minimumX, v.x);
                maximumX = std::max(maximumX, v.x);
            }
        }
    }
    Require(Near(minimumX, 1) && Near(maximumX, 7), "节点非均匀缩放必须保留");
    for (const auto &a : scene.attributes) {
        Require(Near(a.normal0.z, 1) && Near(std::abs(a.tangent.x), 1), "作者法线和切线必须保留并归一化");
        Require(a.tangent.w == -1 && a.normal0.w == -1, "向下纹理坐标必须同步调整切线手性");
    }
    Require(Near(scene.materials[0].baseRoughness.x, .25f) && Near(scene.materials[1].baseRoughness.y, .8f),
            "FBX 线性材质常数不能重复做 sRGB 解码");
    const auto &image = scene.textureViews.at(scene.materials[0].textures.x).image;
    Require(image.y == 2 && image.z == 1 && scene.packedTexels.at(image.x) == 0xff0000ffu &&
                scene.packedTexels.at(image.x + 1) == 0x8000ff00u,
            "嵌入 PNG 的颜色与透明通道必须完整解码");
}
}
int wmain(int argc, wchar_t **argv) {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        // 可选真实资产探针复用相同导入路径，便于核对用户模型的完整材质与图像表。
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect") {
            const auto inspected = render::ImportSceneFile(std::filesystem::path(argv[2]));
            render::Aabb bounds;
            for (const auto &triangle : inspected.triangles)
                for (const auto &vertex : {triangle.a, triangle.b, triangle.c})
                    bounds.Expand({vertex.x, vertex.y, vertex.z});
            std::cout << "三角形 " << inspected.triangles.size() << "，表面 " << inspected.meshParts.size() << "，材质 "
                      << inspected.materials.size() << "，纹理视图 " << inspected.textureViews.size() << "，像素 "
                      << inspected.packedTexels.size() << "，包围盒 " << bounds.minimum.x << ',' << bounds.minimum.y
                      << ',' << bounds.minimum.z << " / " << bounds.maximum.x << ',' << bounds.maximum.y << ','
                      << bounds.maximum.z << '\n';
            for (std::size_t i = 0; i < inspected.materialNames.size(); ++i)
                std::cout << i << "：" << inspected.materialNames[i] << '\n';
            std::set<std::tuple<unsigned, unsigned, unsigned>> images;
            for (const auto &view : inspected.textureViews)
                images.emplace(view.image.x, view.image.y, view.image.z);
            std::cout << "唯一图像 " << images.size() << '\n';
            for (const auto &[offset, width, height] : images)
                std::cout << "图像 " << offset << "：" << width << 'x' << height << '\n';
            if (SUCCEEDED(apartment))
                CoUninitialize();
            return 0;
        }
        const auto source = Fixture();
        // 解析器保留原始分面，通用导入入口则将同一文件实例化为一个完整模型。
        CheckGeometry(render::ImportFbx(source));
        const auto scene = render::ImportSceneFile(source);
        Require(scene.meshParts.size() == 1 && scene.meshParts.front().triangleCount == 4,
                "同一 FBX 的多个材质必须属于一个完整模型对象");
        gameplay::World world;
        world.CreateScene(scene);
        const auto snapshot = world.ExtractRenderSnapshot();
        Require(snapshot.instances.size() == 1 && world.Material(0)->Texture(0), "完整模型必须作为一个 ECS 实例保留全部纹理资源");
        gameplay::World reloaded;
        reloaded.CreateDocument(world.ExportDocument());
        bool restoredTexture = false;
        for (std::size_t material = 0; material < reloaded.Materials().size(); ++material)
            if (reloaded.Material(material)->Texture(0))
                restoredTexture = true;
        Require(reloaded.CookResourceScene().triangles.size() == 4 && restoredTexture,
                "FBX 导入必须可保存到世界文档后恢复对象材质关联");
        const auto directory =
            std::filesystem::temp_directory_path() / (L"EncoreFBX_模型_" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        // 同一正式模型派生外部纹理版本，验证 Unicode 路径与嵌入/外部一致性。
        auto text = Read(source);
        // 多个材质的 sRGB 基础色和线性法线视图必须共享来源像素，不能按槽再次解码。
        auto shared = text;
        Replace(shared, "C: \"OP\",301,200,\"DiffuseColor\"",
                "C: \"OP\",301,200,\"DiffuseColor\"\n    C: \"OP\",301,201,\"DiffuseColor\"\n    C: "
                "\"OP\",301,201,\"NormalMap\"");
        Write(directory / "shared.fbx", shared);
        const auto sharing = render::ImportFbx(directory / "shared.fbx");
        Require(sharing.packedTexels.size() == 2 && sharing.textureViews.size() == 3,
                "同一来源必须只存一份完整 RGBA 像素");
        const auto colorView = sharing.materials[1].textures.x, normalView = sharing.materials[1].textures.z;
        Require(colorView != ~0u && normalView != ~0u &&
                    sharing.textureViews[colorView].image.x == sharing.textureViews[normalView].image.x,
                "颜色空间由使用槽决定，不得强制改变共享图像字节");
        // 模拟 Blender 的同图基础色与透明度接线，Alpha 只能读取一次，不能平方或重复存图。
        auto blender = text;
        // 从当前夹具的生成器标记切换到 Blender 分支，避免改名后测试静默跳过兼容逻辑。
        Replace(blender, "EncoreEngine deterministic FBX fixture", "Blender (stable FBX IO) - 4.2.0 - 4.12.0");
        Replace(blender, "C: \"OP\",301,200,\"DiffuseColor\"",
                "C: \"OP\",301,200,\"DiffuseColor\"\n    C: \"OP\",301,200,\"TransparencyFactor\"");
        Write(directory / "blender-alpha.fbx", blender);
        const auto alpha = render::ImportFbx(directory / "blender-alpha.fbx");
        Require(alpha.materials[0].flags.z == 1 && alpha.packedTexels.size() == 2 &&
                    alpha.packedTexels[alpha.textureViews.at(alpha.materials[0].textures.x).image.x + 1] == 0x8000ff00u,
                "Blender 同图透明度必须保留原 Alpha 并共享原图");
        auto external = text;
        const auto content = external.find("        Content:");
        Require(content != std::string::npos, "测试模型必须包含嵌入图像字段");
        external.erase(content, external.find('\n', content) - content + 1);
        SavePng(directory / "texture.png");
        Write(directory / "external.FBX", external);
        CheckGeometry(render::ImportFbx(directory / "external.FBX"));
        std::filesystem::remove(directory / "texture.png");
        Rejects([&] { render::ImportFbx(directory / "external.FBX"); }, "缺失外部图像必须明确失败");
        // 负缩放不能把作者正面变成背面，法线与几何绕序仍需一致。
        auto mirrored = text;
        Replace(mirrored, "\"A\",2,1,1", "\"A\",-2,1,1");
        Write(directory / "mirrored.fbx", mirrored);
        const auto reflected = render::ImportFbx(directory / "mirrored.fbx");
        for (const auto &t : reflected.triangles)
            Require(render::Cross({t.b.x - t.a.x, t.b.y - t.a.y, t.b.z - t.a.z},
                                  {t.c.x - t.a.x, t.c.y - t.a.y, t.c.z - t.a.z})
                            .z > 0,
                    "镜像节点必须翻转绕序以保留作者正面");
        auto singular = text;
        Replace(singular, "\"A\",2,1,1", "\"A\",0,1,1");
        Write(directory / "singular.fbx", singular);
        Rejects([&] { render::ImportFbx(directory / "singular.fbx"); }, "奇异缩放必须拒绝");
        auto brokenMaterial = text;
        Replace(brokenMaterial, "Materials: *2 { a: 0,1 }", "Materials: *2 { a: 0,99 }");
        Write(directory / "bad-material.fbx", brokenMaterial);
        // 官方解析器会将坏面材质索引归零，明确验证该兼容行为不会丢面或产生 GPU 越界引用。
        const auto repaired = render::ImportFbx(directory / "bad-material.fbx");
        Require(repaired.triangles.size() == 4 && repaired.meshParts.size() == 1 &&
                    repaired.meshParts[0].materialIndex == 0,
                "官方材质归零兼容必须保留全部表面与有效引用");
        Rejects([&] { render::ImportSceneFile(source.parent_path() / "invalid.fbx"); }, "截断 FBX 必须明确失败");
        Rejects([&] { render::ImportSceneFile(directory / "unknown.obj"); }, "统一入口必须拒绝未支持格式");
        std::filesystem::remove_all(directory);
        if (SUCCEEDED(apartment))
            CoUninitialize();
        std::cout << "FBX 几何、材质、嵌入与外部纹理、Unicode 路径、镜像和错误输入测试通过\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (SUCCEEDED(apartment))
            CoUninitialize();
        return 1;
    }
}
