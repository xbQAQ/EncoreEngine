// PSO 创建与复用属于管线层，材质源码编译通过独立接口完成。
#include "Pipeline/PipelineCache.h"
#include "dx12/Dx12Device.h"
#include "Materials/MaterialShaderCompiler.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>

namespace render {
using namespace dx12;
namespace {
std::vector<char> ReadShader(const std::wstring& name) {
    const auto path = std::filesystem::path(RENDER_SHADER_DIRECTORY) / (name + L".cso");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open compiled shader: " + path.string());
    std::vector<char> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}
}
PipelineCache::PipelineCache(Dx12Device& device) : device_(device) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &ranges[1]};
    // 通过视图索引选择图集瓦片，无需重写共享逐帧常量缓冲区。
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {1, 0, 4};
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_ROOT_SIGNATURE_DESC description{4, parameters, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "Root signature serialization");
    Check(device.Native()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)), "Root signature");
    const auto vertex = ReadShader(L"GBuffer_VS");
    const auto pixel = ReadShader(L"GBuffer_PS");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC raster{};
    raster.pRootSignature = root_.Get();
    raster.VS = {vertex.data(), vertex.size()};
    raster.PS = {pixel.data(), pixel.size()};
    raster.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    raster.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    raster.RasterizerState.DepthClipEnable = TRUE;
    for (auto& target : raster.BlendState.RenderTarget) target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    raster.SampleMask = UINT_MAX;
    raster.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    raster.NumRenderTargets = 5;
    for (unsigned i = 0; i < 5; ++i) raster.RTVFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    raster.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    raster.DepthStencilState.DepthEnable = TRUE;
    raster.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    raster.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    raster.SampleDesc.Count = 1;
    Check(device.Native()->CreateGraphicsPipelineState(&raster, IID_PPV_ARGS(&gbuffer_)), "GBuffer pipeline");
    const auto shadowVertex = ReadShader(L"ShadowDepth_VS");
    const auto shadowPixel = ReadShader(L"ShadowDepth_PS");
    raster.VS = {shadowVertex.data(), shadowVertex.size()};
    raster.PS = {shadowPixel.data(), shadowPixel.size()};
    raster.NumRenderTargets = 1;
    // 清除五目标 GBuffer 遗留的格式，避免 D3D12 拒绝阴影管线。
    for (unsigned index = 1; index < 8; ++index) raster.RTVFormats[index] = DXGI_FORMAT_UNKNOWN;
    raster.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    Check(device.Native()->CreateGraphicsPipelineState(&raster, IID_PPV_ARGS(&shadow_)), "Shadow pipeline");
    // 每个 GI 和重建阶段为各个查询后端维护独立编译程序。
    for (unsigned backend : {0u, 1u, 2u}) {
        if (backend == 1 && !device.DxrSupported()) continue;
        for (const auto* name : {L"DirectLighting", L"PathTrace", L"SSGI", L"Post", L"ReSTIR_Candidate",
             L"ReSTIR_Temporal", L"ReSTIR_Spatial", L"ReSTIR_Evaluate", L"RadianceResolve_Temporal", L"RadianceResolve_Spatial",
             L"ShadowBlurH", L"ShadowBlurV", L"SSAO", L"SSR", L"SSIL", L"SSS_Horizontal", L"SSS_Vertical", L"Sky"}) {
            // 纯光栅化不包含路径追踪、蓄水池传输排列或场景查询入口。
            if (backend == 2 && (std::wstring_view(name) == L"PathTrace" || std::wstring_view(name).starts_with(L"ReSTIR_"))) continue;
            const auto key = std::wstring(name) + L"_" + std::to_wstring(backend);
            const auto bytecode = ReadShader(key);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = root_.Get();
            pipeline.CS = {bytecode.data(), bytecode.size()};
            Check(device.Native()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compute_[key])), "Compute pipeline");
        }
    }
}
ID3D12PipelineState* PipelineCache::Compute(const std::wstring& name, unsigned backend) const {
    return compute_.at(name + L"_" + std::to_wstring(backend)).Get();
}
MaterialCompileResult PipelineCache::ReloadMaterials(std::span<const MaterialShaderSource> sources) {
    return PublishMaterials(CompileMaterialShaders(sources, device_.DxrSupported()));
}
void PipelineCache::StartMaterialCompilation(std::vector<MaterialShaderSource> sources) {
    const bool dxr = device_.DxrSupported();
    compilation_ = std::async(std::launch::async, [sources = std::move(sources), dxr] {
        return CompileMaterialShaders(sources, dxr);
    });
}
void PipelineCache::RequestMaterialReload(std::span<const MaterialShaderSource> sources) {
    std::vector<MaterialShaderSource> copy(sources.begin(), sources.end());
    if (compilation_.valid()) queued_ = std::move(copy);
    else StartMaterialCompilation(std::move(copy));
}
std::optional<MaterialCompileResult> PipelineCache::PollMaterialReload() {
    if (!compilation_.valid() || compilation_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
    auto candidate = compilation_.get();
    if (queued_) {
        // 存在更新草稿时跳过已过期的编译结果，避免旧版本短暂闪入视口。
        auto latest = std::move(*queued_); queued_.reset(); StartMaterialCompilation(std::move(latest));
        return std::nullopt;
    }
    return PublishMaterials(std::move(candidate));
}
MaterialCompileResult PipelineCache::PublishMaterials(MaterialShaderBundle candidate) {
    if (!candidate.result.success) return candidate.result;
    try {
        const auto code = [&](const wchar_t* key) {
            const auto& bytes = candidate.programs.at(key);
            return D3D12_SHADER_BYTECODE{bytes.data(), bytes.size()};
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC raster{};
        raster.pRootSignature = root_.Get();
        raster.VS = code(L"GBuffer_VS"); raster.PS = code(L"GBuffer_PS");
        raster.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        raster.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        raster.RasterizerState.DepthClipEnable = TRUE;
        for (auto& target : raster.BlendState.RenderTarget) target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        raster.SampleMask = UINT_MAX; raster.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        raster.NumRenderTargets = 5;
        for (unsigned index = 0; index < 5; ++index) raster.RTVFormats[index] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        raster.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        raster.DepthStencilState.DepthEnable = TRUE;
        raster.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        raster.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        raster.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> gbuffer, shadow;
        Check(device_.Native()->CreateGraphicsPipelineState(&raster, IID_PPV_ARGS(&gbuffer)), "Candidate GBuffer pipeline");
        raster.VS = code(L"ShadowDepth_VS"); raster.PS = code(L"ShadowDepth_PS");
        raster.NumRenderTargets = 1;
        for (unsigned index = 1; index < 8; ++index) raster.RTVFormats[index] = DXGI_FORMAT_UNKNOWN;
        raster.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        Check(device_.Native()->CreateGraphicsPipelineState(&raster, IID_PPV_ARGS(&shadow)), "Candidate shadow pipeline");
        std::map<std::wstring, ComPtr<ID3D12PipelineState>> compute;
        // 原生管线先在候选集合中完整创建，驱动拒绝时仍保留整组上一版可用管线。
        for (const auto& [key, bytes] : candidate.programs) {
            if (key == L"GBuffer_VS" || key == L"GBuffer_PS" || key == L"ShadowDepth_VS" || key == L"ShadowDepth_PS") continue;
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = root_.Get(); description.CS = {bytes.data(), bytes.size()};
            Check(device_.Native()->CreateComputePipelineState(&description, IID_PPV_ARGS(&compute[key])), "Candidate compute pipeline");
        }
        device_.WaitIdle();
        gbuffer_.Swap(gbuffer); shadow_.Swap(shadow); compute_.swap(compute); ++materialRevision_;
        return {true, "材质编译成功，预览已更新"};
    } catch (const std::exception& error) { return {false, std::string("材质管线更新失败：") + error.what()}; }
}
}
