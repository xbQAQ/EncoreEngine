#pragma once
#include "render/GpuTypes.h"
#include "render/Math.h"
#include <vector>

namespace render {
struct ShadowProjection {
    float u{}; // Horizontal coordinate inside a shadow tile, before atlas packing.
    float v{}; // Downward-positive tile coordinate matching D3D raster viewport mapping.
    float depth{}; // Linear depth normalized between the view near and far planes.
    bool inside{}; // True only inside the complete finite projection volume.
};

// Assign only after complete validation, retaining light metadata when any view cannot be built.
std::vector<GpuShadowView> BuildShadowViews(std::vector<GpuLight>& lights, Aabb bounds,
    unsigned resolution = 256, unsigned tiles = 10);
ShadowProjection ProjectShadowPoint(const GpuShadowView& view, Vec3 position);
}
