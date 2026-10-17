#include "GltfInternal.h"
#include "ImportImages.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace render::gltf {
namespace {
std::vector<std::uint8_t> ImageBytes(const ImportContext &context, const cgltf_image &image) {
    // GLB 嵌入片段和 data URI 先恢复编码图像，再进入共用 WIC 解码边界。
    if (image.buffer_view) {
        const auto *data = cgltf_buffer_view_data(image.buffer_view);
        Require(data != nullptr, "embedded image buffer is unavailable");
        return {data, data + image.buffer_view->size};
    }
    Require(image.uri != nullptr, "image has neither URI nor bufferView");
    std::string uri = image.uri;
    if (uri.starts_with("data:")) {
        const auto comma = uri.find(',');
        Require(comma != std::string::npos && uri.substr(0, comma).ends_with(";base64"),
                "image data URI must use base64 encoding");
        const auto payload = uri.substr(comma + 1);
        Require(!payload.empty() && payload.size() % 4 == 0, "image base64 payload has invalid length");
        const auto padding = payload.ends_with("==") ? 2u : payload.ends_with('=') ? 1u : 0u;
        const std::size_t size = payload.size() / 4 * 3 - padding;
        void *decoded = nullptr;
        cgltf_options options{};
        Require(cgltf_load_buffer_base64(&options, size, payload.c_str(), &decoded) == cgltf_result_success,
                "image base64 payload cannot be decoded");
        std::unique_ptr<void, decltype(&std::free)> owner(decoded, &std::free);
        const auto *bytes = static_cast<const std::uint8_t *>(decoded);
        return {bytes, bytes + size};
    }
    // 模型引用只读取本地路径，不把远端地址交给文件系统或网络组件。
    Require(uri.find("://") == std::string::npos, "network image URIs are unsupported; export local PNG/JPEG images");
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    return ReadBytes(context.source.parent_path() /
                     std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(uri.c_str()))));
}
std::uint32_t Wrap(cgltf_wrap_mode mode) {
    switch (mode) {
    case cgltf_wrap_mode_repeat:
        return 0;
    case cgltf_wrap_mode_clamp_to_edge:
        return 1;
    case cgltf_wrap_mode_mirrored_repeat:
        return 2;
    default:
        throw std::runtime_error("glTF sampler wrap mode is unsupported");
    }
}
}
std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path) {
    // 共用图像读取边界仍保留原有 glTF 诊断前缀，调用方可继续识别导入失败类别。
    try {
        return asset_detail::ReadBytes(path);
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string("glTF import: ") + error.what());
    }
}
Uint4 DecodeImage(ImportContext &context, const cgltf_image &image) {
    // 相同来源仅解码一次，独立 UV 视图仍可共享完全相同的 CPU 像素。
    if (const auto found = context.decodedImages.find(&image); found != context.decodedImages.end())
        return found->second;
    Uint4 result;
    // 图像错误仍带 glTF 分类前缀，便于已有调用方统一处理模型导入诊断。
    try {
        result = asset_detail::DecodeImage(context.scene, ImageBytes(context, image));
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string("glTF import: ") + error.what());
    }
    context.decodedImages.emplace(&image, result);
    return result;
}
std::uint32_t TextureView(ImportContext &context, const cgltf_texture_view &view) {
    if (!view.texture)
        return ~0u;
    // 扩展中的 texCoord 覆盖基础坐标集，不额外引入第二组 UV 要求。
    const auto texcoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
    Require(texcoord == 0, "only TEXCOORD_0 texture views are supported");
    Require(view.texture->image != nullptr, "texture has no supported PNG/JPEG image source");
    GpuTextureView output;
    output.image = DecodeImage(context, *view.texture->image);
    if (view.texture->sampler)
        output.image.w = Wrap(view.texture->sampler->wrap_s) | (Wrap(view.texture->sampler->wrap_t) << 2);
    if (view.has_transform) {
        const auto &transform = view.transform;
        output.offsetScale = {Finite(transform.offset[0], "UV offset"), Finite(transform.offset[1], "UV offset"),
                              Finite(transform.scale[0], "UV scale"), Finite(transform.scale[1], "UV scale")};
        output.rotation = {Finite(std::sin(transform.rotation), "UV rotation"),
                           Finite(std::cos(transform.rotation), "UV rotation"), 0, 0};
    }
    Require(context.scene.textureViews.size() < std::numeric_limits<std::uint32_t>::max(),
            "texture view count exceeds GPU index capacity");
    const auto index = static_cast<std::uint32_t>(context.scene.textureViews.size());
    context.scene.textureViews.push_back(output);
    return index;
}
}
