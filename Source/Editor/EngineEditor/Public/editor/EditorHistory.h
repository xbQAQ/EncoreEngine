#pragma once

#include "gameplay/World.h"
#include "render/RenderSettings.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace render::editor {
// 只复制编辑值，不复制或持有 World 拥有的资源。
struct EditorEntitySnapshot {
    gameplay::Entity entity{entt::null}; // 包含代际信息的完整 EnTT 实体标识。
    std::optional<gameplay::Name> name; // 恢复实体时保留名称组件原来的缺省状态。
    std::optional<gameplay::Transform> transform; // 作者配置的位姿，独立于渲染提取结果。
    std::optional<gameplay::MeshRenderer> mesh; // 轻量几何范围设置及非拥有型资源句柄。
    std::optional<gameplay::Camera> camera; // 可选的相机投影与活动状态配置。
    std::optional<gameplay::Light> light; // 可选的解析光源编辑参数。
    std::optional<gameplay::Velocity> velocity; // 可选的模拟移动配置。
    std::optional<gameplay::LuaScript> script; // 游戏逻辑源码、路径及启用状态，不包含虚拟机状态。
    std::optional<gameplay::ObjectIdentity> identity; // 序列化层级及稳定对象编号，独立于运行时 EnTT 代次。
};

struct EditorDocument {
    std::vector<EditorEntitySnapshot> entities; // 按带代际标识排序的轻量 ECS 编辑记录。
    std::vector<GpuMaterial> materials; // 仅保存材质值，资源所有权仍归 World::Resources()。
    std::vector<MaterialShaderSource> materialScripts; // 所有现有槽位的表面源码，包含空源码槽位。
    RenderSettings settings; // 编辑控制项，reset 和 simulate 不参与历史差异。
    gameplay::Entity selection{entt::null}; // 随实际编辑操作共同恢复的选择状态。
};

EditorDocument CaptureEditorDocument(const gameplay::World& world, const RenderSettings& settings,
                                     gameplay::Entity selection);

class EditorHistory {
public:
    EditorHistory();
    ~EditorHistory();
    EditorHistory(EditorHistory&&) noexcept;
    EditorHistory& operator=(EditorHistory&&) noexcept;
    EditorHistory(const EditorHistory&) = delete;
    EditorHistory& operator=(const EditorHistory&) = delete;

    // merge 延长前一个同名交互，具体交互边界由调用者确定。
    bool Record(const EditorDocument& before, const EditorDocument& after, std::string label, bool merge = false);
    bool CanUndo() const;
    bool CanRedo() const;
    std::string_view UndoLabel() const;
    std::string_view RedoLabel() const;
    // 标识槽位被外部实体占用时，在任何修改发生前拒绝回放。
    bool Undo(gameplay::World& world, RenderSettings& settings, gameplay::Entity& selection);
    bool Redo(gameplay::World& world, RenderSettings& settings, gameplay::Entity& selection);
    void Clear();

private:
    struct State;
    std::unique_ptr<State> state_; // 有容量上限的稀疏事务，不长期保存完整文档。
};
}
