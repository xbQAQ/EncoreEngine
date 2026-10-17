#pragma once

#include "render/Math.h"
#include "render/GpuTypes.h"
#include "core/Resource.h"

#include <entt/entity/entity.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace render { class MeshResource; }

namespace gameplay {
using Entity = entt::entity;
using Vec3 = render::Vec3;

struct Name {
    std::string value; // 场景属性栏显示的可读实体名称。
};

struct Transform {
    Vec3 position; // 以米为单位的世界空间平移。
    Vec3 rotationDegrees; // 绕局部 X、Y、Z 轴旋转的欧拉角，单位为度。
    Vec3 scale{1, 1, 1}; // 局部轴缩放，导入的 Cornell 几何默认不缩放。
};

struct MeshRenderer {
    std::uint32_t firstTriangle{}; // GPU BVH 重排前，不可变源几何流中的首个三角形。
    std::uint32_t triangleCount{}; // 此实例包含的连续三角形数量。
    std::uint32_t materialIndex{}; // 单材质覆盖槽，使用源表面材质时表示当前编辑的材质槽。
    bool visible{true}; // 此实例是否参与渲染快照提取。
    bool lightOwned{}; // 位姿及覆盖范围归解析光源控制的网格源标记，创建后保持不变。
    engine::ResourceHandle<render::MeshResource> meshResource; // 非拥有型烘焙网格资源句柄，三角形范围选择其表面。
    bool useSurfaceMaterials{}; // 保留同一模型内部逐三角形材质，所有表面共享同一个对象变换。
};

struct Camera {
    float verticalFovDegrees{43}; // 垂直透视视场角，单位为度。
    float nearPlane{0.05f}; // 正数近裁剪距离，单位为米。
    float farPlane{100}; // 以米为单位的远裁剪距离，必须大于近裁剪距离。
    bool active{true}; // 是否向渲染器提供此相机供视图选择。
};

enum class LightType : std::uint8_t {
    Directional, // 平行光照，方向由 Transform 的旋转决定。
    Point, // 位于 Transform 位置的全向发光体。
    Rectangle, // 局部法线为 -Y、边轴为 X 和 Z 的矩形面光源。
    Disk, // 宽度和高度分别表示完整直径的椭圆面光源。
    Spot, // 使用内外锥角衰减的有限锥形光源。
};

struct Light {
    LightType type{LightType::Rectangle}; // 渲染器解释的发光体几何类型。
    Vec3 color{1, 0.94f, 0.84f}; // 乘强度前的线性发光颜色。
    float intensity{15}; // 面光源的辐亮度倍率，其他类型使用渲染器约定的功率。
    float width{0.9f}; // 面光源沿局部 X 轴的完整边长，单位为米。
    float height{0.9f}; // 面光源沿局部 Z 轴的完整边长，单位为米。
    float range{10}; // 点光源的影响距离，单位为米；方向光忽略此项。
    bool enabled{true}; // 此光源是否出现在提取的渲染快照中。
    float sourceRadius{0.05f}; // 半影使用的世界空间半径，方向光则使用弧度角半径。
    float outerAngleDegrees{40}; // 聚光灯外锥半角，单位为度。
    float innerAngleDegrees{25}; // 聚光灯完全照亮区域的内锥半角，单位为度。
    Vec3 direction{0, -1, 0}; // 实体欧拉旋转前，作者配置的发光轴。
    Vec3 tangent{1, 0, 0}; // 实体欧拉旋转前，作者配置的宽度轴。
    Vec3 bitangent{0, 0, 1}; // 实体欧拉旋转前，作者配置的高度轴。
    std::uint32_t emitterMaterial{~0u}; // 现有发光网格的材质槽，UINT_MAX 表示生成面光源几何。
};

struct Velocity {
    Vec3 linear; // 世界空间线速度，单位为米每秒。
};

struct LuaScript {
    std::string source; // 定义 update(entity, deltaSeconds) 的 Lua 编辑源码。
    std::string path; // 编辑器加载及保存使用的可选 UTF-8 源文件路径。
    bool enabled{true}; // 单实体执行开关，独立于全局运行状态。
    bool operator==(const LuaScript&) const = default;
};

struct RenderInstance {
    Entity entity{entt::null}; // 用于选择和调试的带代际源实体标识。
    Transform transform; // 复制的实例位姿，不受后续注册表编辑影响。
    MeshRenderer mesh; // 复制的几何范围和可见性设置。
};

struct RenderCamera {
    Entity entity{entt::null}; // 带代际的源相机实体标识。
    Transform transform; // 复制的世界空间相机位姿，零旋转朝向 +Z。
    Camera camera; // 复制的投影参数及活动状态。
};

struct RenderLight {
    Entity entity{entt::null}; // 带代际的源发光体实体标识。
    Transform transform; // 复制的世界空间发光体位姿。
    Light light; // 复制的灯光形状、辐亮度及影响参数。
};

struct RenderSnapshot {
    std::vector<RenderInstance> instances; // 按实体标识排序的可见网格实例。
    std::vector<RenderCamera> cameras; // 按实体标识排序的活动相机。
    std::vector<RenderLight> lights; // 按实体标识排序的已启用光源。
    std::vector<render::GpuMaterial> materials; // 在提取边界复制的可编辑共享表面参数。
    std::uint64_t revision{}; // 这些不可变组件副本对应的 World 版本。
};
}
