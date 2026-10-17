#pragma once

#include "editor/EditorHistory.h"
#include <map>

namespace render::editor {
// 导入或创建时的作者基线独立于有长度上限的撤销历史保存。
class EditorDefaults {
public:
    void Capture(const gameplay::World& world);
    void Observe(const gameplay::World& world);
    void Clear();
    const EditorEntitySnapshot* EntityDefaults(gameplay::Entity entity) const;
    const GpuMaterial* MaterialDefaults(unsigned index) const;
    const MaterialShaderSource* MaterialScriptDefaults(unsigned index) const;
    bool ResetEntity(gameplay::World& world, gameplay::Entity entity);
    bool ResetMaterial(gameplay::World& world, unsigned index);

private:
    struct MaterialBaseline {
        GpuMaterial parameters; // 导入或首次观察的材质系数与纹理视图索引。
        MaterialShaderSource script; // 最初的材质源代码及本地路径，包含空源代码。
        engine::ResourceHandle<MaterialResource> resource; // 用于身份校验的非拥有材质句柄。
    };
    bool MatchesWorld(const gameplay::World& world) const;
    const gameplay::World* world_{}; // 非拥有场景边界；替换场景后调用方应再次 Capture。
    std::map<gameplay::Entity, EditorEntitySnapshot> entities_; // 按包含代次的完整实体身份保存基线。
    std::map<unsigned, MaterialBaseline> materials_; // 材质槽位及其权威资源身份对应的初始值。
};
}
