#pragma once

#include "render/GpuTypes.h"
#include "render/Math.h"
#include <string>
#include <vector>

namespace render {
struct SceneCamera {
    std::string name; // 摄像机对象的作者名称。
    Vec3 position; // 世界空间摄像机位置。
    Vec3 rotationDegrees; // 与 ECS 一致的 X、Y、Z 欧拉角。
    float verticalFovDegrees{43}; // 垂直透视视场角。
    float nearPlane{0.05f}; // 正的近裁剪距离。
    float farPlane{100}; // 大于近裁剪距离的远平面。
    bool active{true}; // 是否参与主摄像机选择。
};
struct SceneMeshPart {
    std::string name; // ECS 与编辑器使用的稳定源节点或原语名称。
    std::uint32_t firstTriangle{}; // 不可变源三角形序列中的起始位置。
    std::uint32_t triangleCount{}; // 此原语连续拥有的三角形数量。
    std::uint32_t materialIndex{}; // 默认材质槽；使用表面材质时仅作为初始编辑槽。
    std::string objectId; // 文档对象标识，用于将烘焙范围映射回作者数据。
    bool useSurfaceMaterials{}; // 一个完整模型可保留源三角形各自的材质，不再按材质拆成对象。
};
struct Scene {
    std::vector<GpuTriangle> triangles; // 渲染传输用三角形；BVH 可在自己的副本中重排。
    std::vector<GpuMaterial> materials; // 由三角形 a.w 寻址的线性材质参数传输表。
    std::vector<GpuTriangleAttributes> attributes; // 由三角形 b.w 减一寻址的法线、UV 与切线。
    std::vector<GpuTextureView> textureViews; // 材质采样使用的图像视图与包裹方式。
    std::vector<std::uint32_t> packedTexels; // 自持有的 RGBA8 图像像素。
    std::vector<std::string> materialNames; // 与材质传输槽对应的显示名称。
    std::vector<SceneMeshPart> meshParts; // 在 BVH 重排之前确定的源对象范围。
    std::vector<GpuLight> lights; // 创建 ECS 时复制的解析光源传输记录。
    std::vector<std::string> lightNames; // 与光源传输记录对应的源名称。
    std::string sourcePath; // UTF-8 来源路径，仅记录来源而不建立资源所有权。
    bool cornell{}; // 兼容旧测试的 Cornell 发光体约定。
    std::vector<std::string> materialIds; // 材质槽对应的世界共享目录引用。
    std::vector<std::string> materialObjectIds; // 非空时表示此槽属于对象专属变体。
    std::vector<std::string> textureIds; // 各采样视图引用的世界图像标识，保留同像素资源的作者身份。
    std::vector<SceneCamera> cameras; // 文档烘焙的摄像机传输记录，不建立作者对象所有权。
};

// 兼容入口从发布文档加载 Cornell，不在代码中维护房间几何常量。
Scene BuildCornellScene();
}
