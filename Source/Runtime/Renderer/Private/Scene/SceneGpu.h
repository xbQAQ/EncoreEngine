#pragma once
#include "render/Bvh.h"
#include "render/Scene.h"
#include "gameplay/Components.h"
#include "dx12/Dx12Common.h"
#include <optional>

namespace render::dx12 { class Dx12Device; class DescriptorArena; class ResourcePool; class GraphExecutor; }
namespace render {
// 在玩法存储之外，将不可变 ECS 快照转换为光栅、BVH 和 DXR 共用的几何。
class SceneGpu {
public:
    SceneGpu(dx12::Dx12Device&, dx12::DescriptorArena&, dx12::ResourcePool&, Scene scene = BuildCornellScene());
    const Scene& ReferenceScene() const { return reference_; }
    bool Synchronize(const gameplay::RenderSnapshot& snapshot);
    // 诊断完整快照提取及软件 BVH 重建次数，不把无上传误判为没有 CPU 重建。
    std::uint64_t SceneBuildCount() const { return sceneBuildCount_; }
    void PrepareBackend(unsigned backend);
    void AddUploadPasses(RenderGraph&, dx12::GraphExecutor&, unsigned backend = 0);
    unsigned TriangleCount() const { return static_cast<unsigned>(scene_.triangles.size()); }
    unsigned NodeCount() const { return static_cast<unsigned>(nodes_.size()); }
    unsigned MaterialCount() const { return static_cast<unsigned>(scene_.materials.size()); }
    unsigned LightCount() const { return static_cast<unsigned>(scene_.lights.size()); }
    unsigned ShadowViewCount() const { return static_cast<unsigned>(shadowViewsCpu_.size()); }
    unsigned AttributeCount() const { return static_cast<unsigned>(scene_.attributes.size()); }
    unsigned TextureViewCount() const { return static_cast<unsigned>(scene_.textureViews.size()); }
    unsigned TexelCount() const { return static_cast<unsigned>(scene_.packedTexels.size()); }
    dx12::GpuResource& Triangles() const { return *triangles_; }
    dx12::GpuResource& BvhNodes() const { return *bvh_; }
    dx12::GpuResource& Materials() const { return *materials_; }
    dx12::GpuResource& Lights() const { return *lights_; }
    dx12::GpuResource& ShadowViews() const { return *shadowViews_; }
    dx12::GpuResource& TriangleAttributes() const { return *attributes_; }
    dx12::GpuResource& TextureViews() const { return *textureViews_; }
    dx12::GpuResource& Texels() const { return *texels_; }
    dx12::GpuResource* Tlas() const { return tlas_; }
private:
    void EnsureGeometryCapacity(std::size_t triangles, std::size_t attributes);
    void CreateAcceleration(dx12::GpuResource& triangles, unsigned capacity);
    void AddAccelerationPasses(RenderGraph&);
    D3D12_RAYTRACING_GEOMETRY_DESC Geometry(const dx12::GpuResource& triangles, unsigned count) const;
    dx12::Dx12Device& device_; // 原生设备与命令录制目标。
    dx12::DescriptorArena& descriptors_; // 拥有长期 TLAS 视图槽位。
    dx12::ResourcePool& pool_; // 拥有场景及上传暂存分配。
    Scene reference_; // ECS MeshRenderer 组件引用的原始几何范围。
    Scene scene_; // 最新的变换后、经 BVH 重排的渲染几何。
    std::vector<GpuBvhNode> nodes_; // 与当前 GPU 图元顺序匹配的软件 BVH。
    std::vector<GpuShadowView> shadowViewsCpu_; // 与光照共用的打包图集相机投影。
    unsigned triangleCapacity_{}; // 按倍增扩展的三角形容量，包含共享实例和面光源。
    unsigned attributeCapacity_{}; // 按倍增扩展的私有法线、UV 和切线记录容量。
    dx12::GpuResource* triangles_{}; // 光栅、BLAS 和软件遍历共用的三角形流。
    dx12::GpuResource* bvh_{}; // 按最坏情况容量分配的平衡节点缓冲区。
    dx12::GpuResource* materials_{}; // 当前线性材质参数。
    dx12::GpuResource* lights_{}; // 全部启用的解析光源及其阴影视图范围。
    dx12::GpuResource* shadowViews_{}; // 最多 96 个阴影视图的投影与图集位置。
    dx12::GpuResource* attributes_{}; // 稳定的三角形局部法线、UV 和切线记录。
    dx12::GpuResource* textureViews_{}; // UV 变换和打包图像偏移。
    dx12::GpuResource* texels_{}; // 供全部渲染路径采样的 RGBA8 源图像。
    dx12::GpuResource* blas_{}; // 底层几何加速结构。
    dx12::GpuResource* tlas_{}; // 使用单位变换的场景实例加速结构。
    dx12::GpuResource* scratch_{}; // 足够构建两级加速结构的临时空间容量。
    dx12::GpuResource* instance_{}; // 单位变换实例的上传分配。
    std::uint64_t revision_{~std::uint64_t{0}}; // 已在 CPU 完成提取的 ECS 版本。
    std::optional<gameplay::RenderSnapshot> sceneInputs_; // 最近成功完整同步的轻量输入，相机变化不替换几何缓存。
    std::uint64_t sceneBuildCount_{}; // 成功接受快照时发生的完整场景重建次数，不含源初始化。
    bool geometryDirty_{true}; // 请求几何上传和加速结构重建。
    bool materialsDirty_{true}; // 独立于几何变化的材质上传请求。
    bool lightsDirty_{true}; // ECS 编辑后的光源和阴影视图上传请求。
    bool assetsDirty_{true}; // 首次使用时上传不可变图像和视图。
    bool accelerationDirty_{true}; // 仍需硬件加速结构构建的几何变化。
    unsigned accelerationCapacity_{}; // 当前已分配 BLAS/TLAS 对应的三角形容量。
};
}
