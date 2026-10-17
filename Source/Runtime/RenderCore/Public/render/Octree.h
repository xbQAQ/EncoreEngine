#pragma once

#include "render/GpuTypes.h"
#include "render/Math.h"

#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace render {
inline constexpr std::uint32_t InvalidOctreeNode = std::numeric_limits<std::uint32_t>::max();

struct OctreeNode {
    Aabb bounds; // Spatial cell covered by this node, including its boundaries.
    std::array<std::uint32_t, 8> children{InvalidOctreeNode, InvalidOctreeNode, InvalidOctreeNode, InvalidOctreeNode,
                                          InvalidOctreeNode, InvalidOctreeNode, InvalidOctreeNode, InvalidOctreeNode}; // Octant child indices; the sentinel denotes an empty octant.
    std::vector<std::uint32_t> triangleIndices; // Triangles stored here because they are leaves or straddle child cells.
};

class Octree {
public:
    void Build(const std::vector<GpuTriangle>& triangles, std::uint32_t leafCapacity = 8, std::uint32_t maximumDepth = 8) {
        if (leafCapacity == 0 || maximumDepth > 32) throw std::invalid_argument("Octree capacity must be positive and depth at most 32");
        if (triangles.size() >= InvalidOctreeNode) throw std::length_error("Octree triangle count exceeds index capacity");
        std::vector<Aabb> triangleBounds;
        Aabb rootBounds;
        triangleBounds.reserve(triangles.size());
        for (const auto& triangle : triangles) {
            Aabb bounds;
            for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) {
                const Vec3 position{vertex.x, vertex.y, vertex.z};
                if (!IsFinite(position)) throw std::invalid_argument("Octree contains a nonfinite triangle vertex");
                bounds.Expand(position);
            }
            triangleBounds.push_back(bounds);
            rootBounds.Expand(bounds.minimum);
            rootBounds.Expand(bounds.maximum);
        }
        std::vector<OctreeNode> nodes;
        std::function<std::uint32_t(const Aabb&, std::vector<std::uint32_t>, std::uint32_t)> buildCell;
        buildCell = [&](const Aabb& cell, std::vector<std::uint32_t> indices, std::uint32_t depth) {
            if (nodes.size() >= InvalidOctreeNode) throw std::length_error("Octree node count exceeds index capacity");
            const auto node = static_cast<std::uint32_t>(nodes.size());
            nodes.emplace_back();
            nodes[node].bounds = cell;
            if (indices.size() <= leafCapacity || depth >= maximumDepth) {
                nodes[node].triangleIndices = std::move(indices);
                return node;
            }
            const Vec3 middle = cell.minimum * 0.5f + cell.maximum * 0.5f;
            std::array<std::vector<std::uint32_t>, 8> groups;
            // Store straddlers once at the parent to avoid duplicating large room triangles.
            for (const auto triangle : indices) {
                const auto& bounds = triangleBounds[triangle];
                std::uint32_t octant = 0;
                bool straddles = false;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (bounds.maximum[axis] <= middle[axis]) continue;
                    if (bounds.minimum[axis] >= middle[axis]) octant |= 1u << axis;
                    else { straddles = true; break; }
                }
                if (straddles) nodes[node].triangleIndices.push_back(triangle);
                else groups[octant].push_back(triangle);
            }
            for (std::uint32_t octant = 0; octant < 8; ++octant) {
                if (groups[octant].empty()) continue;
                const Vec3 childMinimum{(octant & 1) ? middle.x : cell.minimum.x,
                                        (octant & 2) ? middle.y : cell.minimum.y,
                                        (octant & 4) ? middle.z : cell.minimum.z};
                const Vec3 childMaximum{(octant & 1) ? cell.maximum.x : middle.x,
                                        (octant & 2) ? cell.maximum.y : middle.y,
                                        (octant & 4) ? cell.maximum.z : middle.z};
                const auto child = buildCell({childMinimum, childMaximum}, std::move(groups[octant]), depth + 1);
                nodes[node].children[octant] = child;
            }
            return node;
        };
        std::vector<std::uint32_t> all;
        all.reserve(triangles.size());
        for (std::uint32_t index = 0; index < triangles.size(); ++index) all.push_back(index);
        if (!all.empty()) buildCell(rootBounds, std::move(all), 0);
        // Commit only after construction succeeds, preserving the old index on invalid input.
        nodes_ = std::move(nodes);
        triangleBounds_ = std::move(triangleBounds);
    }

    std::vector<std::uint32_t> Query(const Aabb& bounds) const {
        if (!IsFinite(bounds.minimum) || !IsFinite(bounds.maximum) || bounds.minimum.x > bounds.maximum.x ||
            bounds.minimum.y > bounds.maximum.y || bounds.minimum.z > bounds.maximum.z)
            throw std::invalid_argument("Octree query bounds must be finite and ordered");
        std::vector<std::uint32_t> result;
        if (nodes_.empty()) return result;
        std::vector<std::uint32_t> stack{0};
        while (!stack.empty()) {
            const auto index = stack.back();
            stack.pop_back();
            const auto& node = nodes_[index];
            if (!Overlaps(bounds, node.bounds)) continue;
            // This is a conservative broad phase: exact triangle tests belong to the caller.
            for (const auto triangle : node.triangleIndices)
                if (Overlaps(bounds, triangleBounds_[triangle])) result.push_back(triangle);
            for (const auto child : node.children) if (child != InvalidOctreeNode) stack.push_back(child);
        }
        return result;
    }

    const std::vector<OctreeNode>& Nodes() const { return nodes_; }

private:
    std::vector<OctreeNode> nodes_; // Sparse octants in a flat array; root is zero when nonempty.
    std::vector<Aabb> triangleBounds_; // Original scene-order triangle bounds for precise broad-phase filtering.

    static bool Overlaps(const Aabb& a, const Aabb& b) {
        return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x &&
               a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y &&
               a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
    }
};
}
