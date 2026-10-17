#include "Lighting/LtcLuts.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include "dx12/ResourcePool.h"
#include "LtcTables.generated.h"
#include <cstring>
#include <span>
#include <stdexcept>

namespace render {
namespace {
std::vector<std::byte> ReadTable(std::span<const unsigned char> bytes) {
    // 上游原始 DDS 在构建时嵌入程序，发布后无需源代码目录或运行路径约定。
    constexpr std::size_t headerBytes = 128, pixelBytes = 64 * 64 * 8;
    if (bytes.size() != headerBytes + pixelBytes) throw std::runtime_error("Invalid embedded LTC table size");
    std::array<std::uint32_t, 32> header{};
    std::memcpy(header.data(), bytes.data(), headerBytes);
    if (header[0] != 0x20534444u || header[1] != 124u || header[3] != 64u || header[4] != 64u ||
        header[19] != 32u || header[20] != 4u || header[21] != 113u)
        throw std::runtime_error("LTC requires the official 64x64 A16B16G16R16F DDS layout");
    std::vector<std::byte> pixels(pixelBytes);
    std::memcpy(pixels.data(), bytes.data() + headerBytes, pixelBytes);
    return pixels;
}
}

LtcLuts::LtcLuts(dx12::ResourcePool& resources) {
    // 光照表不是 RenderTarget，也不需要 UAV；保持线性半精度采样语义。
    pixels_[0] = ReadTable(ltc_data::LtcMatrixDds);
    pixels_[1] = ReadTable(ltc_data::LtcAmplitudeDds);
    textures_[0] = &resources.Texture("Lighting.LTC.Matrix", 64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    textures_[1] = &resources.Texture("Lighting.LTC.Amplitude", 64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
}

void LtcLuts::Upload(RenderGraph& graph, dx12::GraphExecutor& executor, dx12::Dx12Device& device) {
    // 上传暂存也由图持有，两个表的源数据不会与逐 Pass 常量或另一张图的 Map 写入别名。
    for (unsigned index = 0; index < textures_.size(); ++index) {
        auto* target = textures_[index];
        auto& staging = executor.UploadParameters(graph, target->name + ".Upload", pixels_[index]);
        dx12::PassParameters parameters;
        parameters.Access(staging, ResourceAccess::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
        parameters.Access(*target, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
        parameters.AddPass(graph, target->name + ".Copy",
            [&device, target, &staging](const dx12::PassParameters&) {
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = staging.object.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64, 1, 512};
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = target->object.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                device.Commands()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            });
        graph.Export(target->graphId, dx12::ShaderRead);
    }
}
}
