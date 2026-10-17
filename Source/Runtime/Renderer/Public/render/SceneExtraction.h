#pragma once

#include "gameplay/Components.h"
#include "render/Scene.h"
#include <span>

namespace render {
// 仅比较决定场景几何、材质和灯光的数据；相机集合与世界版本独立影响视图历史。
bool SameRenderSceneInputs(const gameplay::RenderSnapshot& left, const gameplay::RenderSnapshot& right);
// 快路径仍校验全部活动相机，不能因未重建场景而跳过有限位姿和投影边界。
void ValidateRenderCameras(std::span<const gameplay::RenderCamera> cameras);
// 源槽保持稳定，共享网格实例和解析发光体追加独立变换后的几何。
// 可选所有权映射跟随未排序三角形，仅在提取完整成功后替换。
Scene ExtractRenderScene(const Scene& reference, const gameplay::RenderSnapshot& snapshot, bool cornell = false,
    std::vector<gameplay::Entity>* triangleOwners = nullptr);
// 历史 Cornell 入口保留源原语索引，共享网格复制实例追加几何。
// 兼容源材质三标识旧灯板，其姿态、尺寸与材质由光源实体控制。
// 配置文档使用通用入口和显式发光体归属，不依赖此兼容约定。
Scene ExtractCornellRenderScene(const Scene& reference, const gameplay::RenderSnapshot& snapshot);
}
