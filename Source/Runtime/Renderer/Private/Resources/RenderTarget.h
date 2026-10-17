#pragma once
// 附件类型复用图资源代理，不引入整帧或算法依赖。
#include "Resources/GraphResources.h"

namespace render {
// RenderTarget 专指一张能够作为颜色或深度附件的图内纹理，不代表帧资源集合。
class RenderTarget final : public GraphTexture {
public:
    RenderTarget(std::string name, const ResourceDescriptor& description);
};
}
