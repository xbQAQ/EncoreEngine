#pragma once
#include "render/WorldDocument.h"

namespace render {
// 原语工厂只处理通用局部几何，不持有任何场景专属尺寸或材质。
Scene BuildPrimitive(const config::MeshAsset &mesh);
void ApplyDocumentTransform(Scene &geometry, const config::Transform &transform);
} // 命名空间 render
