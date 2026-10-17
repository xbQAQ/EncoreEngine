#pragma once

#include "render/GpuTypes.h"
#include "render/Math.h"
#include <vector>

namespace render {
class Bvh {
public:
    // Leaves address contiguous ranges in the returned triangle ordering; root is node zero.
    std::vector<GpuBvhNode> Build(std::vector<GpuTriangle>& triangles) const;
    static Hit Trace(const Ray& ray, const std::vector<GpuTriangle>& triangles,
                     const std::vector<GpuBvhNode>& nodes);
};
}
