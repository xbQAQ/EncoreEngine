#pragma once

#include "core/Resource.h"
#include "render/Scene.h"
#include "render/MaterialScripts.h"

#include <array>
#include <iterator>
#include <span>
#include <stdexcept>

namespace render {
class TextureResource final : public engine::Resource {
public:
    TextureResource(std::string name, std::uint32_t width = 0, std::uint32_t height = 0,
                    std::vector<std::uint32_t> texels = {})
        : Resource(std::move(name)), width_(width), height_(height), texels_(std::move(texels)) {
        // CPU 图像存储完整且不可变，局部图集窗口不会越过导入边界。
        if (!width_ || !height_ || std::uint64_t(width_) * height_ != texels_.size())
            throw std::invalid_argument("texture resource requires a complete nonempty RGBA8 image");
    }
    std::string_view TypeName() const override { return "TextureResource"; }
    std::uint32_t Width() const { return width_; }
    std::uint32_t Height() const { return height_; }
    std::span<const std::uint32_t> Texels() const { return texels_; }

private:
    std::uint32_t width_{}; // 完整图像的宽度，单位为纹素。
    std::uint32_t height_{}; // 完整图像的高度，单位为纹素。
    std::vector<std::uint32_t> texels_; // 资源所有者内部此解码 RGBA8 图像的唯一 CPU 副本。
};

struct TextureBinding {
    engine::ResourceHandle<TextureResource> texture; // 由 World 所有者解析的非拥有型图像标识。
    GpuTextureView view; // 每次使用的 UV 变换及采样器配置，独立于共享图像。
};

class MaterialResource final : public engine::Resource {
public:
    MaterialResource(std::string name, GpuMaterial parameters = {}, std::span<const TextureBinding> textures = {})
        : Resource(std::move(name)), parameters_(parameters), textures_(textures) {}
    std::string_view TypeName() const override { return "MaterialResource"; }
    GpuMaterial& Parameters() { return parameters_; }
    const GpuMaterial& Parameters() const { return parameters_; }
    // 脚本源码属于编辑数据，GPU 管线只消费成功编译后的快照。
    std::string& ScriptSource() { return scriptSource_; }
    const std::string& ScriptSource() const { return scriptSource_; }
    std::string& ScriptPath() { return scriptPath_; }
    const std::string& ScriptPath() const { return scriptPath_; }
    const TextureBinding* Texture(std::size_t slot) const {
        // 从当前参数解析，避免旧接口直接编辑纹理索引后遗留过期绑定。
        const std::array indices{parameters_.textures.x, parameters_.textures.y, parameters_.textures.z, parameters_.textures.w,
                                 parameters_.textures2.x, parameters_.textures2.y, parameters_.textures2.z, parameters_.textures2.w};
        if (slot >= indices.size()) throw std::out_of_range("material texture slot is outside the supported eight slots");
        const auto index = indices[slot];
        if (index == ~0u) return nullptr;
        if (index >= textures_.size()) throw std::out_of_range("material texture view is outside its owning world");
        return &textures_[index];
    }

private:
    GpuMaterial parameters_; // 可编辑表面参数的唯一权威记录。
    std::span<const TextureBinding> textures_; // 借用的不可变纹理视图表，随所属 World 一起转移。
    std::string scriptSource_; // 标准纹理因子求值后执行的可选 HLSL Surface 函数。
    std::string scriptPath_; // 脚本编辑器使用的 UTF-8 本地源码路径。
};

class MeshResource final : public engine::Resource {
public:
    MeshResource(std::string name, std::vector<GpuTriangle> triangles = {},
                 std::vector<GpuTriangleAttributes> attributes = {}, std::vector<SceneMeshPart> surfaces = {})
        : Resource(std::move(name)), triangles_(std::move(triangles)), attributes_(std::move(attributes)), surfaces_(std::move(surfaces)) {}
    std::string_view TypeName() const override { return "MeshResource"; }
    std::span<const GpuTriangle> Triangles() const { return triangles_; }
    std::span<const GpuTriangleAttributes> Attributes() const { return attributes_; }
    std::span<const SceneMeshPart> Surfaces() const { return surfaces_; }

private:
    std::vector<GpuTriangle> triangles_; // 保留导入源三角形顺序的不可变图元流。
    std::vector<GpuTriangleAttributes> attributes_; // 不可变的共享属性，三角形 b.w 索引保持不变。
    std::vector<SceneMeshPart> surfaces_; // 带名称的源图元范围，用于选择此烘焙网格中的材质。
};

// 兼容视图向现有编辑器提供参数访问，不重复保存可编辑材质数据。
class MaterialParameters {
public:
    template<bool ReadOnly>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = GpuMaterial;
        using difference_type = std::ptrdiff_t;
        using reference = std::conditional_t<ReadOnly, const GpuMaterial&, GpuMaterial&>;
        using pointer = std::conditional_t<ReadOnly, const GpuMaterial*, GpuMaterial*>;
        explicit Iterator(std::vector<MaterialResource*>::const_iterator position) : position_(position) {}
        reference operator*() const { return (*position_)->Parameters(); }
        pointer operator->() const { return &**this; }
        Iterator& operator++() { ++position_; return *this; }
        Iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        bool operator==(const Iterator&) const = default;

    private:
        std::vector<MaterialResource*>::const_iterator position_; // 将当前资源项映射为其参数记录的迭代位置。
    };

    MaterialParameters() = default;
    explicit MaterialParameters(std::vector<MaterialResource*> materials) : materials_(std::move(materials)) {}
    GpuMaterial& operator[](std::size_t index) { return materials_[index]->Parameters(); }
    const GpuMaterial& operator[](std::size_t index) const { return materials_[index]->Parameters(); }
    std::size_t size() const { return materials_.size(); }
    bool empty() const { return materials_.empty(); }
    Iterator<false> begin() { return Iterator<false>(materials_.begin()); }
    Iterator<false> end() { return Iterator<false>(materials_.end()); }
    Iterator<true> begin() const { return Iterator<true>(materials_.begin()); }
    Iterator<true> end() const { return Iterator<true>(materials_.end()); }

private:
    std::vector<MaterialResource*> materials_; // 借用稳定的资源地址，随独占资源存储一起移动。
};
}
