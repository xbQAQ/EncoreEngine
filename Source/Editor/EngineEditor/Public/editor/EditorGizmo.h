#pragma once

#include "gameplay/Components.h"
#include "render/Scene.h"
#include <array>
#include <optional>

namespace gameplay { class World; }

namespace render::editor {
// ImGuizmo 使用列主序仿射矩阵，转换只发生在编辑器边界。
using GizmoMatrix = std::array<float, 16>;

struct ViewportRect {
    float x{}; // 实际渲染图像的屏幕左边缘，不包含留黑边。
    float y{}; // 实际渲染图像的屏幕上边缘。
    float width{}; // ImGui 屏幕坐标下为正的图像显示宽度。
    float height{}; // ImGui 屏幕坐标下为正的图像显示高度。

    bool Contains(float screenX, float screenY) const;
};

struct GizmoView {
    GizmoMatrix view{}; // 使用渲染器正 Z 前向约定的世界到相机矩阵。
    GizmoMatrix projection{}; // 使用 DirectX 零到一深度区间的透视矩阵。
};

struct SurfaceHit {
    gameplay::Entity entity{entt::null}; // 可选的交点所属实体，光源代理会解析到其拥有者。
    Vec3 position; // 渲染器接受的最近世界空间交点。
    Vec3 normal; // 朝向入射射线起点的几何单位法线。
    float distance{}; // 沿输入射线参数测量的交点距离。
};

// 复用世界空间拾取场景，仅非相机输入变化或显式世界替换时重建。
class SurfacePickCache {
public:
    std::optional<SurfaceHit> Pick(const Scene& source, const gameplay::World& world, const Ray& ray);
    void Reset();
    // 暴露查询缓存的重建次数，便于区分射线测试与大块场景提取成本。
    std::uint64_t SceneBuildCount() const { return sceneBuildCount_; }

private:
    const Scene* source_{}; // 不可变源场景的身份，宿主重建源数据时同时失效。
    const gameplay::World* world_{}; // 当前缓存所属世界的非拥有指针。
    std::uint64_t revision_{}; // 已提取的世界作者版本，覆盖材质、变换和创建修改。
    std::uint64_t sceneBuildCount_{}; // 当前缓存生命周期内成功建立拾取场景的次数。
    gameplay::RenderSnapshot sceneInputs_; // 已接受的非相机输入，避免导航失效大块拾取几何。
    Scene extracted_; // 当前版本的世界空间几何与采样数据，逐条射线查询期间保持不变。
    std::vector<gameplay::Entity> owners_; // 与提取后图元索引一致的实体拥有者。
    std::vector<GpuTriangle> candidates_; // 每条新射线重新填充的可见性筛选工作区。
    std::vector<GpuBvhNode> leaf_; // 保留原始图元顺序的单叶查询范围。
    std::optional<Ray> ray_; // 最近查询的完整射线，包含近远裁剪边界。
    std::optional<SurfaceHit> hit_; // 最近射线的接受结果，空值同样可以缓存。
    bool ready_{}; // 区分初次查询与世界初始零版本。
};

// 网格轴心使用不可变源包围盒，ECS 位姿仍围绕原点变换源顶点。
Vec3 MeshPivot(const Scene& source, const gameplay::MeshRenderer& mesh);
GizmoMatrix GizmoPose(const gameplay::Transform& transform, Vec3 pivot = {});
std::optional<gameplay::Transform> GizmoTransform(const GizmoMatrix& matrix, Vec3 pivot,
    const gameplay::Transform& previous);
GizmoView MakeGizmoView(const gameplay::Transform& transform, const gameplay::Camera& camera, float aspect);
std::optional<Ray> ViewportRay(const ViewportRect& image, float screenX, float screenY,
    const gameplay::Transform& transform, const gameplay::Camera& camera);

// 拾取与放置共用渲染器的提取几何、解析光源标记以及材质可见性判定。
std::optional<SurfaceHit> PickSurface(const Scene& source, const gameplay::World& world, const Ray& ray);
gameplay::Entity PickEntity(const Scene& source, const gameplay::World& world, const Ray& ray, SurfacePickCache* cache = nullptr);
gameplay::Entity GizmoTarget(const gameplay::World& world, gameplay::Entity entity);
}
