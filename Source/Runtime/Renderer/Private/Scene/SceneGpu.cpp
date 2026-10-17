#include "SceneGpu.h"
#include "render/SceneExtraction.h"
#include "render/ShadowViews.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include "dx12/GraphExecutor.h"
#include "dx12/ResourcePool.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace render {
using namespace dx12;
namespace {
template<class T> bool SameBytes(const std::vector<T>& left, const std::vector<T>& right) {
    return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

constexpr unsigned maximumTriangles = std::numeric_limits<unsigned>::max() / 3;
constexpr unsigned maximumAttributes = 1u << 24;

unsigned GrownCapacity(unsigned current, std::size_t required, unsigned maximum) {
    // 分配之前确保 DXR 顶点计数和从一开始的浮点属性索引可精确表示。
    if (required > maximum) throw std::invalid_argument("scene geometry exceeds GPU primitive or exact attribute index capacity");
    if (!current) return static_cast<unsigned>(std::max(required, std::size_t{1}));
    auto result = std::uint64_t{current};
    while (result < required) result = std::min(result * 2, std::uint64_t{maximum});
    return static_cast<unsigned>(result);
}
}
SceneGpu::SceneGpu(Dx12Device& device, DescriptorArena& descriptors, ResourcePool& pool, Scene source)
    : device_(device), descriptors_(descriptors), pool_(pool), reference_(std::move(source)) {
    // 参考场景保留源对象顺序，只有 GPU 副本由 BVH 构建器重排。
    if (reference_.triangles.size() > maximumTriangles)
        throw std::invalid_argument("scene triangle count exceeds DXR vertex capacity");
    if (reference_.materials.size() > std::numeric_limits<unsigned>::max() - 16ull ||
        reference_.textureViews.size() > std::numeric_limits<unsigned>::max() ||
        reference_.packedTexels.size() > std::numeric_limits<unsigned>::max())
        throw std::invalid_argument("scene asset count exceeds GPU buffer index capacity");
    scene_ = reference_;
    nodes_ = Bvh{}.Build(scene_.triangles);
    const auto initialTriangles = static_cast<std::size_t>(std::min(std::uint64_t{maximumTriangles},
        std::uint64_t{reference_.triangles.size()} + 16 * 64));
    EnsureGeometryCapacity(initialTriangles, reference_.attributes.size());
    materials_ = &pool_.Buffer("Scene.Materials", (scene_.materials.size() + 16) * sizeof(GpuMaterial), sizeof(GpuMaterial));
    lights_ = &pool_.Buffer("Scene.Lights", 16 * sizeof(GpuLight), sizeof(GpuLight));
    shadowViews_ = &pool_.Buffer("Scene.ShadowViews", 96 * sizeof(GpuShadowView), sizeof(GpuShadowView));
    textureViews_ = &pool_.Buffer("Scene.TextureViews", std::max(size_t{1}, reference_.textureViews.size()) * sizeof(GpuTextureView), sizeof(GpuTextureView));
    texels_ = &pool_.Buffer("Scene.Texels", std::max(size_t{1}, reference_.packedTexels.size()) * sizeof(std::uint32_t), sizeof(std::uint32_t));
}

bool SceneGpu::Synchronize(const gameplay::RenderSnapshot& snapshot) {
    if (snapshot.revision == revision_) return false;
    // 相机和其他非渲染作者修改仍使视图历史失效，但不复制资产或重建软件 BVH。
    if (sceneInputs_ && SameRenderSceneInputs(*sceneInputs_, snapshot)) {
        ValidateRenderCameras(snapshot.cameras);
        revision_ = snapshot.revision;
        return true;
    }
    // 输入副本先准备好，分配或验证失败时不提前接受新的世界版本。
    auto inputs = snapshot;
    Scene updated = ExtractRenderScene(reference_, snapshot, reference_.cornell);
    Aabb bounds;
    for (const auto& triangle : updated.triangles) {
        const Vec3 a{triangle.a.x, triangle.a.y, triangle.a.z}, b{triangle.b.x, triangle.b.y, triangle.b.z}, c{triangle.c.x, triangle.c.y, triangle.c.z};
        if (Length(Cross(b - a, c - a)) < 1e-10f) continue;
        bounds.Expand(a); bounds.Expand(b); bounds.Expand(c);
    }
    if (!IsFinite(bounds.minimum)) { bounds.minimum = {-1, -1, -1}; bounds.maximum = {1, 1, 1}; }
    auto shadowViews = BuildShadowViews(updated.lights, bounds);
    auto nodes = Bvh{}.Build(updated.triangles);
    // 提交新快照之前完成分配，避免上传或加速结构构建使用容量不足的目标。
    EnsureGeometryCapacity(updated.triangles.size(), updated.attributes.size());
    geometryDirty_ = geometryDirty_ || !SameBytes(updated.triangles, scene_.triangles);
    materialsDirty_ = materialsDirty_ || !SameBytes(updated.materials, scene_.materials);
    lightsDirty_ = lightsDirty_ || !SameBytes(updated.lights, scene_.lights) || !SameBytes(shadowViews, shadowViewsCpu_);
    if (!SameBytes(updated.attributes, scene_.attributes)) geometryDirty_ = true;
    shadowViewsCpu_ = std::move(shadowViews);
    scene_ = std::move(updated);
    nodes_ = std::move(nodes);
    sceneInputs_ = std::move(inputs);
    // 统计完整 CPU 重建，独立于稍后是否需要上传或构建硬件加速结构。
    ++sceneBuildCount_;
    revision_ = snapshot.revision;
    return true;
}

D3D12_RAYTRACING_GEOMETRY_DESC SceneGpu::Geometry(const GpuResource& triangles, unsigned count) const {
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // RayQuery 显式提交通过 alpha 测试的候选，保持与光栅裁剪一致。
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.VertexBuffer = {triangles.object->GetGPUVirtualAddress(), 16};
    geometry.Triangles.VertexCount = count * 3;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    return geometry;
}

void SceneGpu::EnsureGeometryCapacity(std::size_t triangles, std::size_t attributes) {
    const auto triangleCapacity = GrownCapacity(triangleCapacity_, triangles, maximumTriangles);
    const auto attributeCapacity = GrownCapacity(attributeCapacity_, attributes, maximumAttributes);
    const bool growTriangles = triangleCapacity != triangleCapacity_;
    const bool growAttributes = attributeCapacity != attributeCapacity_;
    if (!growTriangles && !growAttributes) return;
    // 当前帧同步等待提交；额外 fence 也保护其他调用方的描述符替换。
    if (triangles_ || attributes_) device_.WaitIdle();
    auto* newTriangles = triangles_;
    auto* newBvh = bvh_;
    auto* newAttributes = attributes_;
    if (growTriangles) {
        newTriangles = &pool_.Buffer("Scene.Triangles", std::uint64_t{triangleCapacity} * sizeof(GpuTriangle), sizeof(GpuTriangle));
        newBvh = &pool_.Buffer("Scene.SoftwareBVH", std::uint64_t{triangleCapacity} * 2 * sizeof(GpuBvhNode), sizeof(GpuBvhNode));
    }
    if (growAttributes)
        newAttributes = &pool_.Buffer("Scene.Attributes", std::uint64_t{attributeCapacity} * sizeof(GpuTriangleAttributes), sizeof(GpuTriangleAttributes));

    // 资源池保留旧分配直到场景重置；倍增限制保留字节数和长期描述符增长。
    triangles_ = newTriangles;
    bvh_ = newBvh;
    attributes_ = newAttributes;
    triangleCapacity_ = triangleCapacity;
    attributeCapacity_ = attributeCapacity;
    geometryDirty_ = true;
}

void SceneGpu::CreateAcceleration(GpuResource& triangles, unsigned capacity) {
    const auto geometry = Geometry(triangles, capacity);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom{}, top{};
    device_.Native()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &bottom);
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.pGeometryDescs = nullptr;
    device_.Native()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &top);
    auto* blas = &pool_.Buffer("Scene.BLAS", bottom.ResultDataMaxSizeInBytes, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto* tlas = &pool_.Buffer("Scene.TLAS", top.ResultDataMaxSizeInBytes, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    auto* scratch = &pool_.Buffer("Scene.ASScratch", std::max(bottom.ScratchDataSizeInBytes, top.ScratchDataSizeInBytes), 0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* instanceBuffer = &pool_.Buffer("Scene.Instance", sizeof(D3D12_RAYTRACING_INSTANCE_DESC), 0, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
    instance.InstanceMask = 0xff;
    instance.AccelerationStructure = blas->object->GetGPUVirtualAddress();
    void* mapped{};
    Check(instanceBuffer->object->Map(0, nullptr, &mapped), "Map instance");
    std::memcpy(mapped, &instance, sizeof(instance));
    instanceBuffer->object->Unmap(0, nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.RaytracingAccelerationStructure.Location = tlas->object->GetGPUVirtualAddress();
    device_.Native()->CreateShaderResourceView(nullptr, &view, descriptors_.Cpu(2));
    // 分配和实例上传成功之后，统一发布新的加速结构集合。
    blas_ = blas;
    tlas_ = tlas;
    scratch_ = scratch;
    instance_ = instanceBuffer;
}

void SceneGpu::AddAccelerationPasses(RenderGraph& graph) {
    if (!blas_) return;
    graph.AddPass("DXR.BuildBLAS", {Read(*triangles_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), Write(*scratch_),
        Write(*blas_, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)}, [this] {
        const auto geometry = Geometry(*triangles_, TriangleCount());
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs = 1;
        build.Inputs.pGeometryDescs = &geometry;
        build.ScratchAccelerationStructureData = scratch_->object->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = blas_->object->GetGPUVirtualAddress();
        device_.Commands()->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        // 加速结构内存始终保持专用状态，因此使用 UAV 顺序屏障发布内容。
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blas_->object.Get();
        device_.Commands()->ResourceBarrier(1, &barrier);
    });
    graph.AddPass("DXR.BuildTLAS", {Read(*blas_, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE),
        Read(*instance_, D3D12_RESOURCE_STATE_GENERIC_READ), Write(*scratch_), Write(*tlas_, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)}, [this] {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs = 1;
        build.Inputs.InstanceDescs = instance_->object->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch_->object->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = tlas_->object->GetGPUVirtualAddress();
        device_.Commands()->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = tlas_->object.Get();
        device_.Commands()->ResourceBarrier(1, &barrier);
    });
    graph.Export(tlas_->graphId, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
}

void SceneGpu::PrepareBackend(unsigned backend) {
    // 纯光栅或软件求交帧不分配、不构建硬件光线追踪加速结构。
    accelerationDirty_ = accelerationDirty_ || geometryDirty_;
    if (backend == 1 && accelerationCapacity_ != triangleCapacity_) {
        if (!device_.DxrSupported()) throw std::runtime_error("DXR 1.1 is unavailable");
        CreateAcceleration(*triangles_, triangleCapacity_);
        accelerationCapacity_ = triangleCapacity_;
        accelerationDirty_ = true;
    }
}

void SceneGpu::AddUploadPasses(RenderGraph& graph, GraphExecutor& executor, unsigned backend) {
    if (geometryDirty_) {
        pool_.Upload(graph, executor, *triangles_, scene_.triangles.data(), scene_.triangles.size() * sizeof(GpuTriangle));
        pool_.Upload(graph, executor, *bvh_, nodes_.data(), nodes_.size() * sizeof(GpuBvhNode));
        if (!scene_.attributes.empty()) pool_.Upload(graph, executor, *attributes_, scene_.attributes.data(), scene_.attributes.size() * sizeof(GpuTriangleAttributes));
    }
    // 延迟 DXR 构建也覆盖连续光栅帧后切回硬件光线追踪的情况。
    if (backend == 1 && accelerationDirty_) {
        AddAccelerationPasses(graph);
        accelerationDirty_ = false;
    }
    if (materialsDirty_) pool_.Upload(graph, executor, *materials_, scene_.materials.data(), scene_.materials.size() * sizeof(GpuMaterial));
    if (lightsDirty_) {
        if (!scene_.lights.empty()) pool_.Upload(graph, executor, *lights_, scene_.lights.data(), scene_.lights.size() * sizeof(GpuLight));
        if (!shadowViewsCpu_.empty()) pool_.Upload(graph, executor, *shadowViews_, shadowViewsCpu_.data(), shadowViewsCpu_.size() * sizeof(GpuShadowView));
    }
    if (assetsDirty_) {
        if (!scene_.textureViews.empty()) pool_.Upload(graph, executor, *textureViews_, scene_.textureViews.data(), scene_.textureViews.size() * sizeof(GpuTextureView));
        if (!scene_.packedTexels.empty()) pool_.Upload(graph, executor, *texels_, scene_.packedTexels.data(), scene_.packedTexels.size() * sizeof(std::uint32_t));
    }
    geometryDirty_ = false;
    materialsDirty_ = false;
    lightsDirty_ = false;
    assetsDirty_ = false;
}
}
