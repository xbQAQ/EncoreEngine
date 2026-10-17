// 逻辑资源声明保持在资源层，物理分配交由图执行器决定。
#include "Resources/GraphResources.h"
#include "dx12/GraphExecutor.h"
#include <stdexcept>
#include <utility>

namespace render {
GraphTexture::GraphTexture(std::string name, const ResourceDescriptor& description) : description_(description) {
    // 纹理包装在逻辑声明时就拒绝缓冲区描述，避免原生分配后才暴露类型混用。
    if (description.kind != ResourceKind::Texture2D || !description.width || !description.height ||
        !description.format || description.stride)
        throw std::invalid_argument("GraphTexture requires a valid Texture2D descriptor");
    resource_.name = std::move(name);
}

void GraphTexture::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 每帧只登记逻辑纹理，保留图首次访问时才由执行器取得物理存储。
    const auto name = resource_.name;
    executor.Create(graph, resource_, name, description_);
}

GraphBuffer::GraphBuffer(std::string name, const ResourceDescriptor& description) : description_(description) {
    // 结构缓冲区必须包含完整元素，不接受借用纹理宽高或像素格式的描述。
    if (description.kind != ResourceKind::Buffer || !description.width || description.height != 1 || description.format ||
        (description.stride && description.width % description.stride))
        throw std::invalid_argument("GraphBuffer requires a valid buffer descriptor");
    resource_.name = std::move(name);
}

void GraphBuffer::Declare(RenderGraph& graph, dx12::GraphExecutor& executor) {
    // 缓冲区复用与纹理复用共享 RDG 寿命管理，但保留各自类型契约。
    const auto name = resource_.name;
    executor.Create(graph, resource_, name, description_);
}
}
