// 管线上下文显式借用资源和视图，避免依赖帧渲染器的完整定义。
#include "Pipeline/PassContext.h"
#include "Pipeline/PipelineCache.h"
#include "Resources/FrameResources.h"
#include "View/ViewState.h"
#include "render/RenderSettings.h"
#include "Scene/SceneGpu.h"
#include "Lighting/LtcLuts.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"

namespace render {
using namespace dx12;
PassContext::PassContext(Dx12Device& deviceValue, DescriptorArena& descriptorValue, GraphExecutor& executorValue, PipelineCache& pipelineValue,
    SceneGpu& sceneValue, FrameResources& targetValue, const ViewState& viewValue, const RenderSettings& settingsValue, float deltaValue)
    : device(deviceValue), descriptors(descriptorValue), executor(executorValue), pipelines(pipelineValue), scene(sceneValue),
      targets(targetValue), view(viewValue), settings(settingsValue), delta(deltaValue), backend(settingsValue.backend), giMode(settingsValue.gi) {}

FrameConstants PassContext::ViewParameters() const {
    // 通用视图和场景身份可共享 CPU 值；算法开关与数值由各 Pass 入口自行设置。
    auto values = view.Parameters();
    values.settings.y = giMode;
    values.settings.z = backend;
    values.counts = {scene.TriangleCount(), scene.NodeCount(), scene.MaterialCount(), 0};
    values.assets = {scene.AttributeCount(), scene.TextureViewCount(), scene.TexelCount(), 0};
    return values;
}

PassParameters PassContext::Parameters(RenderGraph& graph, const char* label, FrameConstants values,
    const std::vector<GpuResource*>& reads, GpuResource* output,
    bool reservoirOutput, GpuResource* selectedReservoir, GpuResource* postInput, GpuResource* shadowInput) const {
    const std::array<GpuResource*, 26> inputs{&scene.Triangles(), &scene.BvhNodes(), &scene.Materials(), scene.Tlas(),
        targets.gbuffer.position.Pointer(), targets.gbuffer.normal.Pointer(), targets.gbuffer.base.Pointer(), targets.lighting.direct.Pointer(),
        targets.history.gi, targets.history.position, targets.history.normal,
        selectedReservoir ? selectedReservoir : targets.restir.candidates.Pointer(), targets.restir.temporal.Pointer(),
        targets.history.reservoir, targets.lighting.gi.Pointer(), postInput ? postInput : targets.history.resolved,
        &scene.Lights(), shadowInput ? shadowInput : shadowAtlas, &scene.TriangleAttributes(), &scene.TextureViews(),
        &scene.Texels(), &scene.ShadowViews(), targets.gbuffer.tangent.Pointer(), targets.gbuffer.uv.Pointer(),
        ltc ? &ltc->Matrix() : nullptr, ltc ? &ltc->Amplitude() : nullptr};
    PassParameters parameters;
    // 每个 Pass 的参数独立上传，创建、写入和读取都由 RDG 的同一条资源链管理。
    auto& constants = executor.UploadParameters(graph, label, std::as_bytes(std::span{&values, 1}));
    parameters.Constants(constants);
    // 仅记录此算法声明的槽位，未使用槽位不会意外取得全局资源的原生视图。
    for (auto* resource : reads) {
        bool found = false;
        for (unsigned slot = 0; slot < inputs.size(); ++slot) {
            if (resource && inputs[slot] == resource) {
                // TLAS 只有显式出现在本 Pass 输入中才绑定，并保持其不可变专用状态。
                parameters.Srv(slot, *resource, slot == 3 ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE : ShaderRead);
                found = true;
            }
        }
        if (!found) throw std::invalid_argument("Shader input has no declared RDG parameter slot");
    }
    if (output) parameters.Uav(reservoirOutput ? 3u : 0u, *output);
    return parameters;
}

void PassContext::Bind(const RenderGraph& graph, const PassParameters& parameters, bool graphics) {
    // 原生资源只能在声明它的 Pass lambda 内解析；漏声明和过期访问直接报错。
    parameters.Validate(graph);
    if (!parameters.ConstantBuffer()) throw std::logic_error("Shader pass has no frame constant parameter");
    const unsigned table = descriptors.AllocateTable(40);
    for (unsigned slot = 0; slot < parameters.Srvs().size(); ++slot) {
        const auto* resource = parameters.Srvs()[slot];
        const unsigned source = resource ? (slot == 3 ? 2u : resource->srv) : 3u;
        if (resource && !source) throw std::logic_error("Declared RDG shader input has no SRV");
        descriptors.CopyToVisible(table + slot, source);
    }
    for (unsigned slot = 0; slot < parameters.Uavs().size(); ++slot) {
        const auto* resource = parameters.Uavs()[slot];
        if (resource && !resource->uav) throw std::logic_error("Declared RDG shader output has no UAV");
        descriptors.CopyToVisible(table + 32 + slot, resource ? resource->uav : 4u);
    }
    auto* commands = device.Commands();
    const auto constantAddress = parameters.ConstantBuffer()->object->GetGPUVirtualAddress();
    if (graphics) {
        commands->SetGraphicsRootSignature(pipelines.Root());
        commands->SetGraphicsRootConstantBufferView(0, constantAddress);
        commands->SetGraphicsRootDescriptorTable(1, descriptors.Gpu(table));
        commands->SetGraphicsRootDescriptorTable(2, descriptors.Gpu(table + 32));
    } else {
        commands->SetComputeRootSignature(pipelines.Root());
        commands->SetComputeRootConstantBufferView(0, constantAddress);
        commands->SetComputeRootDescriptorTable(1, descriptors.Gpu(table));
        commands->SetComputeRootDescriptorTable(2, descriptors.Gpu(table + 32));
    }
}

std::vector<GpuResource*> PassContext::MaterialReads() const {
    // EvaluateMaterial 的纹理查询不依赖插值属性；切线与 UV 由调用方提供。
    return {&scene.Materials(), &scene.TextureViews(), &scene.Texels()};
}

std::vector<GpuResource*> PassContext::ReceiverReads() const {
    // LoadReceiver 使用图元恢复几何法线，其余可见表面数据来自本帧附件。
    auto result = MaterialReads();
    result.insert(result.end(), {&scene.Triangles(), targets.gbuffer.position.Pointer(), targets.gbuffer.normal.Pointer(),
        targets.gbuffer.tangent.Pointer(), targets.gbuffer.uv.Pointer()});
    return result;
}

std::vector<GpuResource*> PassContext::RaySceneReads() const {
    // Alpha 接受测试和命中法线恢复共享材质资产，但加速结构严格选择当前求交后端。
    auto result = MaterialReads();
    result.insert(result.end(), {&scene.Triangles(), &scene.TriangleAttributes()});
    if (backend == 0) result.push_back(&scene.BvhNodes());
    else if (backend == 1) {
        if (!scene.Tlas()) throw std::logic_error("RDG ray query requires a prepared TLAS");
        result.push_back(scene.Tlas());
    } else throw std::logic_error("Raster-only passes cannot declare scene ray queries");
    return result;
}

std::vector<GpuResource*> PassContext::LightingReads(bool visibility) const {
    // 光源身份与采样都读取 Lights；只有可见性分支取得阴影图或场景求交资源。
    std::vector<GpuResource*> result{&scene.Lights()};
    if (!visibility || !settings.shadows) return result;
    if (settings.shadowMode != 0) {
        if (!shadowAtlas) throw std::logic_error("RDG filtered lighting requires a selected shadow atlas");
        result.insert(result.end(), {&scene.ShadowViews(), shadowAtlas});
    } else if (backend != 2) {
        const auto queries = RaySceneReads();
        result.insert(result.end(), queries.begin(), queries.end());
    }
    return result;
}

void PassContext::Compute(RenderGraph& graph, const char* label, const std::wstring& shader, FrameConstants values,
    std::vector<GpuResource*> reads, GpuResource& output, bool reservoirOutput, GpuResource* selectedReservoir, GpuResource* postInput) {
    // 算法提供完整输入集合；通用分派不追加隐藏资源，逐 Pass 快照仍由 RDG 延迟上传。
    auto parameters = Parameters(graph, label, values, reads, &output, reservoirOutput, selectedReservoir, postInput);
    parameters.AddPass(graph, label, [this, &graph, shader](const PassParameters& bound) {
        Bind(graph, bound);
        device.Commands()->SetPipelineState(pipelines.Compute(shader, backend));
        device.Commands()->Dispatch((targets.width + 7) / 8, (targets.height + 7) / 8, 1);
    });
}
}
