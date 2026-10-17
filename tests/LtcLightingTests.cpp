#include "Lighting/LtcLuts.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/PassParameters.h"
#include "dx12/ResourcePool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace render;
using namespace render::dx12;
constexpr unsigned CaseCount = 20;
struct Result {
    std::array<float, 4> approximation; // 实际 GPU LTC 与 Disney 补偿的 RGB 和 Lambert 基线。
    std::array<float, 4> reference; // 高密度独立面积积分的 RGB。
};
static_assert(sizeof(Result) == 32);

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<Result> Run(const std::filesystem::path& shaderPath) {
    // 使用实际渲染器的 LUT 上传和 RDG 状态管理，避免 CPU 仿真掩盖矩阵转置或纹理格式错误。
    Dx12Device device(true);
    DescriptorArena descriptors(device);
    ResourcePool resources(device, descriptors);
    LtcLuts tables(resources);
    auto& output = resources.Buffer("LTC.Validation.Output", CaseCount * sizeof(Result), sizeof(Result), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& readback = resources.Buffer("LTC.Validation.Readback", CaseCount * sizeof(Result), 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK);
    std::ifstream input(shaderPath, std::ios::binary);
    Require(input.good(), "Cannot read LTC validation shader");
    std::vector<char> shader{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Require(!shader.empty(), "LTC validation shader is empty");

    // 验证入口只使用两个 LTC SRV 和一个结构 UAV，不借助场景或 FrameConstants。
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 24, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 4;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rootBytes, rootErrors;
    Check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &rootErrors), "Serialize LTC validation root");
    ComPtr<ID3D12RootSignature> root;
    Check(device.Native()->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)), "Create LTC validation root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = root.Get();
    pipelineDescription.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device.Native()->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)), "Create LTC validation pipeline");

    device.BeginCommands();
    descriptors.Bind(device.Commands());
    RenderGraph graph;
    GraphExecutor executor(device);
    executor.Import(graph, resources);
    tables.Upload(graph, executor, device);
    PassParameters values;
    values.Srv(24, tables.Matrix());
    values.Srv(25, tables.Amplitude());
    values.Uav(4, output);
    values.AddPass(graph, "LTC.Validation", [&](const PassParameters& bound) {
        bound.Validate(graph);
        const unsigned table = descriptors.AllocateTable(2);
        descriptors.CopyToVisible(table, bound.Srvs()[24]->srv);
        descriptors.CopyToVisible(table + 1, bound.Srvs()[25]->srv);
        device.Commands()->SetComputeRootSignature(root.Get());
        device.Commands()->SetPipelineState(pipeline.Get());
        device.Commands()->SetComputeRootDescriptorTable(0, descriptors.Gpu(table));
        device.Commands()->SetComputeRootUnorderedAccessView(1, output.object->GetGPUVirtualAddress());
        device.Commands()->Dispatch(CaseCount, 1, 1);
    });
    PassParameters copy;
    copy.Access(output, ResourceAccess::Read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy.Access(readback, ResourceAccess::Write, D3D12_RESOURCE_STATE_COPY_DEST);
    copy.AddPass(graph, "LTC.Validation.Readback", [&](const PassParameters&) {
        device.Commands()->CopyBufferRegion(readback.object.Get(), 0, output.object.Get(), 0, CaseCount * sizeof(Result));
    });
    graph.Export(readback.graphId, D3D12_RESOURCE_STATE_COPY_DEST);
    executor.Execute(graph);
    device.SubmitAndWait();
    Require(tables.Matrix().state == ShaderRead && tables.Amplitude().state == ShaderRead, "LTC upload must end in shader-read state");
    std::vector<Result> results(CaseCount);
    void* mapped{};
    const D3D12_RANGE readRange{0, results.size() * sizeof(Result)};
    Check(readback.object->Map(0, &readRange, &mapped), "Map LTC validation results");
    std::memcpy(results.data(), mapped, readRange.End);
    const D3D12_RANGE noWrite{0, 0};
    readback.object->Unmap(0, &noWrite);
    return results;
}

void Validate(const std::vector<Result>& results) {
    // 半精度拟合和有限 Disney 求积允许小幅误差；独立瓣分别核对，不能只检查非黑像素。
    constexpr std::array<const char*, CaseCount> names{
        "Lambert rectangle", "dielectric roughness=.6", "metal roughness=.35", "dielectric roughness=.95",
        "dielectric grazing NoV=.148", "off-axis rectangle", "dielectric disk", "metal disk grazing NoV=.148",
        "horizon-clipped rectangle", "back-facing emitter", "small distant rectangle", "GTR1 clearcoat",
        "sheen and subsurface", "anisotropic metal", "rough BTDF", "thin diffuse transmission",
        "ideal metal reflection", "ideal dielectric transmission", "tilted shading normal", "near large emitter"};
    unsigned failures = 0;
    for (unsigned index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        double error = 0, scale = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            Require(std::isfinite(result.approximation[channel]) && std::isfinite(result.reference[channel]), "LTC integration produced nonfinite values");
            Require(result.approximation[channel] >= 0, "LTC radiance must be nonnegative");
            error = std::max(error, std::abs(double(result.approximation[channel]) - result.reference[channel]));
            scale = std::max(scale, double(result.reference[channel]));
        }
        const bool supplementalLobe = index == 11 || index == 13 || index == 14;
        const double tolerance = index == 0 ? 0.0002 : supplementalLobe ? 0.12 : 0.065;
        // 完整打印后再失败，使同一 GPU 执行的全部材质和几何误差都可诊断。
        const bool passed = error <= 0.002 + tolerance * scale;
        failures += passed ? 0u : 1u;
        std::cout << "LTC case " << index << " (" << names[index] << "): " << (passed ? "PASS" : "FAIL")
            << " approx=(" << result.approximation[0] << ',' << result.approximation[1] << ',' << result.approximation[2]
            << ") reference=(" << result.reference[0] << ',' << result.reference[1] << ',' << result.reference[2]
            << ") max error=" << error << ", limit=" << 0.002 + tolerance * scale << '\n';
        if (index != 9) Require(scale > 0.0001, "A required Disney lobe disappeared from its reference case");
        if (index == 9) Require(scale == 0 && error == 0, "Area lights must not emit from their back face");
    }
    Require(failures == 0u, "LTC deviates from the independent Disney area integral; see all case diagnostics above");
    // 理想金属正入射的面积光镜像必须恢复基础色，不能被有限面积网格漏掉。
    Require(std::abs(results[16].approximation[0] - 0.7f) < 0.0001f, "Zero-roughness conductor lost its discrete reflection");
}
}

int main(int argc, char** argv) {
    try {
        const auto path = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(RENDER_SHADER_DIRECTORY) / L"LtcValidation_test_2.cso";
        Validate(Run(path));
        std::cout << "LTC GPU lighting validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LTC GPU lighting validation failed: " << error.what() << '\n';
        return 1;
    }
}
