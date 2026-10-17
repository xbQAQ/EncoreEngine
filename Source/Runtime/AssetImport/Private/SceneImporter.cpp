#include "asset/SceneImporter.h"
#include "asset/FbxImporter.h"
#include "asset/GltfImporter.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace render {
Scene ImportSceneFile(const std::filesystem::path &path) {
    // Windows 文件筛选不区分大小写，路由入口采用相同扩展名约定。
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
    Scene scene;
    if (extension == L".fbx")
        scene = ImportFbx(path);
    else if (extension == L".gltf" || extension == L".glb")
        scene = ImportGltf(path);
    else
        throw std::invalid_argument("模型格式不受支持，请选择 FBX、glTF 或 GLB 文件");
    // 文件是模型实例的编辑边界，解析器的材质分面保留在源三角形内。
    if (!scene.triangles.empty()) {
        const auto name = path.stem().u8string();
        scene.meshParts = {{std::string(reinterpret_cast<const char*>(name.data()), name.size()), 0,
            static_cast<std::uint32_t>(scene.triangles.size()), static_cast<std::uint32_t>(scene.triangles.front().a.w), {}, true}};
    }
    return scene;
}
}
