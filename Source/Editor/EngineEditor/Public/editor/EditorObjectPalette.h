#pragma once

#include "editor/EditorGizmo.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace gameplay { struct ObjectPreset; }

namespace render::editor {
// 创建预设的拖放协议使用独立引擎前缀，避免与材质等其他负载混用。
inline constexpr const char* objectPresetPayload = "ENCORE_OBJECT_PRESET";

enum class PlacementAnchor {
    Surface, // 使用通过材质透明与背面判定的最近场景表面。
    Ground, // 场景没有交点时使用视锥内的世界零高度平面。
    Camera, // 地面不可达时使用相机正前方三米处的垂直平面。
};

struct ObjectPlacement {
    gameplay::Transform transform; // 已考虑预设变换、吸附和表面支撑的最终作者位姿。
    Vec3 contact; // 预览标记使用的表面或回退平面接触点。
    Vec3 normal; // 接触平面朝向鼠标射线起点的单位法线。
    PlacementAnchor anchor{PlacementAnchor::Camera}; // 本次放置选择的实际或回退平面类型。
};

// 纯 CPU 放置查询与界面交互分离，使裁剪、遮罩和支撑偏移可以独立验证。
std::optional<ObjectPlacement> ResolveObjectPlacement(const Scene& source, const gameplay::World& world,
    const gameplay::ObjectPreset& preset, const Ray& ray, const gameplay::Transform& camera, float snapStep = 0,
    SurfacePickCache* cache = nullptr);
void DrawObjectPlacementPreview(const ObjectPlacement& placement, const gameplay::ObjectPreset& preset,
    const Scene& source, const ViewportRect& image, const gameplay::RenderCamera& camera);

// 面板仅发布稳定预设索引；实体创建、选择与历史事务仍归 EditorUi 管理。
class EditorObjectPalette {
public:
    void Draw(const gameplay::World& world, bool enabled);
    bool Dragging() const { return blocking_; }
    bool AllowDrop() const { return !cancelled_; }
    void SetError(std::string error) { error_ = std::move(error); }
    void Reset();

private:
    std::array<char, 128> search_{}; // 保留面板搜索文本，不修改预设的稳定索引。
    std::string error_; // 最近一次后端创建错误，在下一次有效拖动前保持可见。
    bool gesture_{}; // 原生拖动开始后持续到释放完成，防止源结束时误触视口。
    bool blocking_{}; // 本帧拖动及其释放帧都占有视口输入。
    bool cancelled_{}; // Escape 或失焦后等待左键释放，禁止同次按压重新创建。
};
}
