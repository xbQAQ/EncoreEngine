#include "render/WorldDocument.h"

namespace render {
Scene BuildCornellScene() {
    // 历史入口保留兼容标记，所有几何与材质数值均来自发布文档。
    auto scene = BakeScene(LoadWorldDocument(DefaultWorldPath()), "cornell");
    scene.cornell = true;
    return scene;
}
} // 渲染命名空间
