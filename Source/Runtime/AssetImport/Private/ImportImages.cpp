#include "ImportImages.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fstream>
#include <limits>
#include <stdexcept>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace render::asset_detail {
namespace {
using Microsoft::WRL::ComPtr;
void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error("模型图像导入：" + message);
}
void Check(HRESULT result, const char *action) {
    if (FAILED(result))
        throw std::runtime_error(std::string("模型图像导入：") + action + "失败，HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)));
}
struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)}; // 成功或 S_FALSE 都拥有对应 COM 引用。
    Apartment() {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            Check(result, "初始化 COM");
    }
    ~Apartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
}
std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path) {
    // 不通过本地 ANSI 编码拼接诊断，中文和非 BMP 文件名也能原样打开。
    const auto encoded = path.u8string();
    const std::string label(encoded.begin(), encoded.end());
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    Require(bool(stream), "无法打开文件：" + label);
    const auto length = stream.tellg();
    Require(length >= 0 && static_cast<std::uint64_t>(length) <= std::numeric_limits<std::uint32_t>::max(),
            "文件超过四 GiB 限制");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!result.empty())
        stream.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size()));
    Require(bool(stream), "文件内容读取不完整：" + label);
    return result;
}
Uint4 DecodeImage(Scene &scene, std::span<const std::uint8_t> bytes) {
    // WIC 只把编码像素转换为 RGBA8；颜色空间由材质纹理槽的着色器约定解释。
    Require(!bytes.empty() && bytes.size() <= std::numeric_limits<DWORD>::max(), "图像为空或超过内存流容量");
    Apartment apartment;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "创建 WIC 工厂");
    Check(factory->CreateStream(&stream), "创建图像内存流");
    Check(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size())),
          "打开图像内存流");
    Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "解码图像");
    GUID container{};
    Check(decoder->GetContainerFormat(&container), "读取图像格式");
    Require(IsEqualGUID(container, GUID_ContainerFormatPng) || IsEqualGUID(container, GUID_ContainerFormatJpeg),
            "仅支持 PNG 和 JPEG 纹理，请先转换其他图像格式");
    Check(decoder->GetFrame(0, &frame), "读取首帧图像");
    UINT width{}, height{};
    Check(frame->GetSize(&width, &height), "读取图像尺寸");
    const std::uint64_t count = std::uint64_t(width) * height;
    Require(width && height && width <= 16384 && height <= 16384 && count <= std::numeric_limits<UINT>::max() / 4,
            "图像尺寸超过 RGBA8 容量");
    Require(scene.packedTexels.size() + count <= std::numeric_limits<std::uint32_t>::max(),
            "纹理表超过三十二位偏移范围");
    Check(factory->CreateFormatConverter(&converter), "创建 RGBA 转换器");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom),
          "转换 RGBA8");
    const auto offset = scene.packedTexels.size();
    scene.packedTexels.resize(offset + static_cast<std::size_t>(count));
    Check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(count * 4),
                                reinterpret_cast<BYTE *>(scene.packedTexels.data() + offset)),
          "复制图像像素");
    return {static_cast<std::uint32_t>(offset), width, height, 0};
}
}
