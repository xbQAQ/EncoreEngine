#include "render/Bvh.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

namespace render {
namespace {
Vec3 Position(const Float4& vertex) { return {vertex.x, vertex.y, vertex.z}; }

Vec3 Centroid(const GpuTriangle& triangle) {
    return Position(triangle.a) / 3.0f + Position(triangle.b) / 3.0f + Position(triangle.c) / 3.0f;
}

Aabb Bounds(const GpuTriangle& triangle) {
    Aabb bounds;
    bounds.Expand(Position(triangle.a));
    bounds.Expand(Position(triangle.b));
    bounds.Expand(Position(triangle.c));
    return bounds;
}

void IntersectTriangle(const Ray& ray, const GpuTriangle& triangle, std::uint32_t index, Hit& closest) {
    const Vec3 a = Position(triangle.a);
    const Vec3 edge1 = Position(triangle.b) - a;
    const Vec3 edge2 = Position(triangle.c) - a;
    const Vec3 p = Cross(ray.direction, edge2);
    const double determinant = Dot(edge1, p);
    if (std::abs(determinant) < 1e-10) return;
    const double inverse = 1.0 / determinant;
    const Vec3 offset = ray.origin - a;
    const double u = Dot(offset, p) * inverse;
    if (u < 0 || u > 1) return;
    const Vec3 q = Cross(offset, edge1);
    const double v = Dot(ray.direction, q) * inverse;
    if (v < 0 || u + v > 1) return;
    const double distance = Dot(edge2, q) * inverse;
    if (distance < ray.tMin || distance > ray.tMax || distance >= closest.distance) return;
    closest.distance = static_cast<float>(distance);
    closest.position = ray.origin + ray.direction * closest.distance;
    closest.normal = Normalize(Cross(edge1, edge2));
    if (Dot(closest.normal, ray.direction) > 0) closest.normal = -closest.normal;
    closest.materialIndex = static_cast<std::uint32_t>(triangle.a.w);
    closest.triangleIndex = index;
}
}

std::vector<GpuBvhNode> Bvh::Build(std::vector<GpuTriangle>& triangles) const {
    if (triangles.empty()) return {};
    if (triangles.size() > std::numeric_limits<std::uint32_t>::max() / 2)
        throw std::length_error("BVH triangle count exceeds GPU index capacity");
    // Reject malformed geometry before sorting so failure cannot partially reorder the scene.
    for (const auto& triangle : triangles) {
        if (!IsFinite(Position(triangle.a)) || !IsFinite(Position(triangle.b)) || !IsFinite(Position(triangle.c)))
            throw std::invalid_argument("BVH contains a nonfinite triangle vertex");
        if (!std::isfinite(triangle.a.w) || triangle.a.w < 0 || triangle.a.w >= float(std::numeric_limits<std::uint32_t>::max()) ||
            std::floor(triangle.a.w) != triangle.a.w)
            throw std::invalid_argument("BVH triangle material index is invalid");
    }
    std::vector<GpuBvhNode> nodes;
    nodes.reserve(triangles.size() * 2);
    std::function<std::uint32_t(std::uint32_t, std::uint32_t)> buildRange;
    buildRange = [&](std::uint32_t first, std::uint32_t count) {
        const auto nodeIndex = static_cast<std::uint32_t>(nodes.size());
        nodes.emplace_back();
        Aabb bounds;
        Aabb centroids;
        for (std::uint32_t index = first; index < first + count; ++index) {
            const auto triangleBounds = Bounds(triangles[index]);
            bounds.Expand(triangleBounds.minimum);
            bounds.Expand(triangleBounds.maximum);
            centroids.Expand(Centroid(triangles[index]));
        }
        nodes[nodeIndex].minimum = {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, 0};
        nodes[nodeIndex].maximum = {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z, 0};
        if (count <= 4) {
            nodes[nodeIndex].links = {0, 0, first, count};
            return nodeIndex;
        }
        const Vec3 extent = centroids.maximum - centroids.minimum;
        std::size_t axis = extent.y > extent.x ? 1 : 0;
        if (extent.z > extent[axis]) axis = 2;
        // Stable median splits bound traversal depth and make identical scene builds reproducible.
        std::stable_sort(triangles.begin() + first, triangles.begin() + first + count,
                         [axis](const GpuTriangle& left, const GpuTriangle& right) { return Centroid(left)[axis] < Centroid(right)[axis]; });
        const std::uint32_t leftCount = count / 2;
        const auto left = buildRange(first, leftCount);
        const auto right = buildRange(first + leftCount, count - leftCount);
        nodes[nodeIndex].links = {left, right, 0, 0};
        return nodeIndex;
    };
    buildRange(0, static_cast<std::uint32_t>(triangles.size()));
    return nodes;
}

Hit Bvh::Trace(const Ray& ray, const std::vector<GpuTriangle>& triangles, const std::vector<GpuBvhNode>& nodes) {
    if (!IsFinite(ray.origin) || !IsFinite(ray.direction) || !std::isfinite(ray.tMin) ||
        std::isnan(ray.tMax) || ray.tMin < 0 || ray.tMax < ray.tMin)
        throw std::invalid_argument("BVH ray has invalid origin, direction or distance bounds");
    Hit closest;
    if (nodes.empty()) return closest;
    std::vector<std::uint32_t> stack{0};
    stack.reserve(64);
    // A dynamic stack accepts externally supplied trees without assuming the builder's depth.
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        if (index >= nodes.size()) throw std::invalid_argument("BVH child index is invalid");
        const auto& node = nodes[index];
        const Aabb bounds{Position(node.minimum), Position(node.maximum)};
        if (!bounds.Intersects(ray, closest.distance)) continue;
        if (node.links.w > 0) {
            const std::size_t first = node.links.z;
            const std::size_t end = first + node.links.w;
            if (end > triangles.size()) throw std::invalid_argument("BVH leaf triangle range is invalid");
            for (std::size_t triangle = first; triangle < end; ++triangle)
                IntersectTriangle(ray, triangles[triangle], static_cast<std::uint32_t>(triangle), closest);
        } else {
            if (node.links.x <= index || node.links.y <= index)
                throw std::invalid_argument("BVH children must follow their parent");
            stack.push_back(node.links.y);
            stack.push_back(node.links.x);
        }
    }
    return closest;
}
}
