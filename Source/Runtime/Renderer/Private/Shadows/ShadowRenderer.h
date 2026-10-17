#pragma once
#include "dx12/Dx12Common.h"

namespace render::dx12 { class Dx12Device; class DescriptorArena; class ResourcePool; }
namespace render {
class PassContext;

// Allocate before graph import; the resource pool owns all four stable allocations.
class ShadowRenderer {
public:
    ShadowRenderer(dx12::Dx12Device&, dx12::DescriptorArena&, dx12::ResourcePool&, unsigned resolution = 256, unsigned tiles = 10);
    dx12::GpuResource& AddPasses(RenderGraph&, PassContext&, unsigned viewCount, unsigned shadowMode);
    dx12::GpuResource& Atlas(unsigned shadowMode) const;
    unsigned Resolution() const { return resolution_; }
    unsigned Tiles() const { return tiles_; }
private:
    unsigned resolution_; // Pixel width/height of each square shadow tile.
    unsigned tiles_; // Fixed number of atlas columns and rows.
    dx12::GpuResource* moments_{}; // Rasterized normalized linear depth and second moment.
    dx12::GpuResource* depth_{}; // Hardware depth-test image covering the complete atlas.
    dx12::GpuResource* horizontal_{}; // VSM horizontal convolution output, never an in-place source.
    dx12::GpuResource* filtered_{}; // VSM vertical convolution output consumed by lighting.
};
}
