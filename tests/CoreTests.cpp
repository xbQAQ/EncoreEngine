#include "render/Bvh.h"
#include "render/Octree.h"
#include "render/RenderGraph.h"
#include "render/Reservoir.h"
#include "render/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Near(float a, float b, float tolerance = 0.0002f) {
    return std::abs(a - b) <= tolerance;
}

template <typename Function> void Rejects(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}

render::Vec3 Position(const render::Float4& value) {
    return {value.x, value.y, value.z};
}

// 独立平面和重心求交实现作为 BVH 遍历的参考结果。
render::Hit BruteForce(const render::Ray& ray, const std::vector<render::GpuTriangle>& triangles) {
    render::Hit closest;
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        const auto& triangle = triangles[index];
        const auto a = Position(triangle.a);
        const auto b = Position(triangle.b);
        const auto c = Position(triangle.c);
        const auto normal = render::Cross(b - a, c - a);
        const double denominator = render::Dot(normal, ray.direction);
        if (std::abs(denominator) < 1e-10) continue;
        const float distance = static_cast<float>(render::Dot(normal, a - ray.origin) / denominator);
        if (distance < ray.tMin || distance > ray.tMax || distance >= closest.distance) continue;
        const auto point = ray.origin + ray.direction * distance;
        const float first = render::Dot(normal, render::Cross(b - a, point - a));
        const float second = render::Dot(normal, render::Cross(c - b, point - b));
        const float third = render::Dot(normal, render::Cross(a - c, point - c));
        if (first < -1e-6f || second < -1e-6f || third < -1e-6f) continue;
        closest.distance = distance;
        closest.position = point;
        closest.normal = render::Normalize(normal);
        if (render::Dot(closest.normal, ray.direction) > 0) closest.normal = -closest.normal;
        closest.materialIndex = static_cast<std::uint32_t>(triangle.a.w);
        closest.triangleIndex = static_cast<std::uint32_t>(index);
    }
    return closest;
}

void MathAndBounds() {
    using namespace render;
    Check(Near(Dot(Cross({1, 0, 0}, {0, 1, 0}), {0, 0, 1}), 1), "cross product handedness");
    Check(Near(Length(Normalize({2, 3, 4})), 1), "normalized vector length");
    Check(Length(Normalize({0, 0, 0})) == 0, "zero vector normalization must stay finite");
    Check(!Aabb{}.Intersects({{0, 0, 0}, {1, 1, 1}}, 100), "empty bounds must never intersect");
    Aabb bounds;
    bounds.Expand({-1, -1, -1});
    bounds.Expand({1, 1, 1});
    Check(bounds.Intersects({{1, 0, -2}, {0, 0, 1}}, 100), "parallel ray on bounds edge");
    Check(!bounds.Intersects({{2, 0, -2}, {0, 0, 1}}, 100), "parallel ray outside slab");
    Check(!bounds.Intersects({{0, 0, -2}, {0, 0, 1}, 0.0001f, 0.5f}, 100), "ray max distance respected");
    // 相机旋转必须遵守与网格变换相同的带符号 XYZ 约定。
    const auto rolled = MakeCameraBasis(0, 0, 90);
    Check(Near(rolled.forward.z, 1) && Near(rolled.forward.x, 0), "camera basis keeps forward direction");
    Check(Near(rolled.right.y, 1) && Near(rolled.right.x, 0), "camera roll rotates right vector");
    Check(Near(rolled.up.x, -1) && Near(rolled.up.y, 0), "camera roll rotates up vector");
    Check(Near(MakeCameraBasis(90, 0, 0).forward.y, -1), "camera positive pitch follows right-handed X");
    Check(Near(MakeCameraBasis(0, 90, 0).forward.x, 1), "camera positive yaw follows right-handed Y");
    const auto combined = MakeCameraBasis(90, 90, 90);
    Check(Near(combined.forward.x, 1), "camera rotations apply in XYZ order");
    Check(Near(Dot(combined.forward, combined.right), 0) && Near(Length(combined.up), 1), "camera basis remains orthonormal");
}

void SceneAndBvh() {
    using namespace render;
    auto scene = BuildCornellScene();
    Check(scene.triangles.size() >= 36, "Cornell scene must contain room, light and objects");
    Check(scene.materials.size() >= 5, "Cornell scene materials");
    Bvh builder;
    const auto nodes = builder.Build(scene.triangles);
    Check(!nodes.empty(), "BVH root exists");
    for (const auto& node : nodes) {
        if (node.links.w > 0) {
            Check(static_cast<std::size_t>(node.links.z) + node.links.w <= scene.triangles.size(), "leaf range valid after reordering");
        } else {
            Check(node.links.x < nodes.size() && node.links.y < nodes.size(), "internal children valid");
        }
    }
    const auto backWall = Bvh::Trace({{0, 2.5f, -4.6f}, {0, 0, 1}}, scene.triangles, nodes);
    Check(backWall.Found() && Near(backWall.position.z, 1.4f), "front opening must reveal back wall");
    const auto lamp = Bvh::Trace({{0, 2, 0}, {0, 1, 0}}, scene.triangles, nodes);
    Check(lamp.Found() && Near(lamp.position.y, 2.74f), "light geometry position");
    Check(scene.materials[lamp.materialIndex].emissionMetallic.x > 0, "light uses emissive material");

    // 固定随机种子可复现未命中、内部起点和近乎平行的光线。
    std::mt19937 generator(0x6b7668);
    std::uniform_real_distribution<float> position(-4.5f, 4.5f);
    std::uniform_real_distribution<float> direction(-1, 1);
    for (int index = 0; index < 7000; ++index) {
        Ray ray{{position(generator), position(generator), position(generator)},
                Normalize({direction(generator), direction(generator), direction(generator)})};
        if (index % 7 == 0) ray.direction.x = 0;
        if (index % 11 == 0) ray.direction.y = 0;
        const auto accelerated = Bvh::Trace(ray, scene.triangles, nodes);
        const auto reference = BruteForce(ray, scene.triangles);
        Check(accelerated.Found() == reference.Found(), "BVH and brute force hit agreement");
        if (!reference.Found()) continue;
        Check(Near(accelerated.distance, reference.distance, 0.001f), "BVH closest hit distance");
        const auto& selectedTriangle = scene.triangles[accelerated.triangleIndex];
        const auto& referenceTriangle = scene.triangles[reference.triangleIndex];
        const auto liesOnFloor = [](const GpuTriangle& triangle) {
            return triangle.a.y == 0 && triangle.b.y == 0 && triangle.c.y == 0;
        };
        // 房间地板与闭合盒底完全重合，等距命中任一材质均有效。
        const bool coincidentFloor = liesOnFloor(selectedTriangle) && liesOnFloor(referenceTriangle) &&
                                     Near(accelerated.distance, reference.distance, 0.0001f);
        Check(accelerated.materialIndex == reference.materialIndex || coincidentFloor, "BVH material agreement for unambiguous hits");
        Check(accelerated.materialIndex == static_cast<std::uint32_t>(selectedTriangle.a.w), "hit material belongs to selected triangle");
        Check(Near(Length(accelerated.normal), 1), "BVH normal must be unit length");
        Check(Dot(accelerated.normal, ray.direction) <= 0, "hit normal faces receiving ray");
    }
    std::vector<GpuTriangle> empty;
    Check(builder.Build(empty).empty(), "empty BVH has no nodes");
    Check(!Bvh::Trace({{0, 0, 0}, {0, 1, 0}}, empty, {}).Found(), "empty BVH misses");
    auto invalid = scene.triangles;
    invalid.front().a.x = std::numeric_limits<float>::quiet_NaN();
    Rejects([&] { builder.Build(invalid); }, "nonfinite geometry rejected");
}

void GraphScheduling() {
    using namespace render;
    RenderGraph graph;
    const auto texture = graph.Import("scene", 0, false);
    // 无外部写入副作用的资源允许裁掉无人消费的生产者。
    const auto unused = graph.Import("unused", 0, false, false);
    const auto output = graph.Import("output", 7, true);
    std::vector<std::string> executed;
    graph.AddPass("discarded", {{unused, 1, true}}, [&] { executed.emplace_back("discarded"); });
    graph.AddPass("produce", {{texture, 1, true}}, [&] { executed.emplace_back("produce"); });
    graph.AddPass("consume", {{texture, 2, false}, {output, 1, true}}, [&] { executed.emplace_back("consume"); });
    graph.Export(output, 7);
    const auto& plan = graph.Compile();
    Check(plan.size() == 2, "unreachable pass culled");
    Check(plan[0].name == "produce" && plan[1].name == "consume", "producer before consumer");
    Check(plan[0].barriers.size() == 1 && plan[0].barriers[0].before == 0 && plan[0].barriers[0].after == 1, "initial state transition");
    Check(graph.FinalBarriers().size() == 1 && graph.FinalBarriers()[0].after == 7, "export final state transition");
    std::size_t barrierCount = 0;
    graph.Execute([&](const Barrier&) { ++barrierCount; });
    Check(executed == std::vector<std::string>({"produce", "consume"}), "compiled callbacks executed in dependency order");
    Check(barrierCount == 4, "all transition callbacks executed including exports");
    Check(graph.Resources()[texture].name == "scene", "resource debug names inspectable");
}

void OctreeQueries() {
    using namespace render;
    const auto scene = BuildCornellScene();
    Octree octree;
    octree.Build(scene.triangles, 2, 6);
    Check(!octree.Nodes().empty(), "Octree root exists");
    const auto all = octree.Query({{-10, -10, -10}, {10, 10, 10}});
    Check(all.size() == scene.triangles.size(), "Octree retains each triangle once");
    Check(octree.Query({{10, 10, 10}, {11, 11, 11}}).empty(), "outside Octree query is empty");
    std::mt19937 generator(0x6f637472);
    std::uniform_real_distribution<float> center(-2, 3);
    for (int trial = 0; trial < 800; ++trial) {
        const Vec3 point{center(generator), center(generator), center(generator)};
        const Aabb query{point - Vec3{0.15f, 0.2f, 0.15f}, point + Vec3{0.15f, 0.2f, 0.15f}};
        std::vector<std::uint32_t> reference;
        for (std::uint32_t index = 0; index < scene.triangles.size(); ++index) {
            Aabb bounds;
            bounds.Expand(Position(scene.triangles[index].a));
            bounds.Expand(Position(scene.triangles[index].b));
            bounds.Expand(Position(scene.triangles[index].c));
            if (bounds.minimum.x <= query.maximum.x && bounds.maximum.x >= query.minimum.x &&
                bounds.minimum.y <= query.maximum.y && bounds.maximum.y >= query.minimum.y &&
                bounds.minimum.z <= query.maximum.z && bounds.maximum.z >= query.minimum.z) reference.push_back(index);
        }
        auto found = octree.Query(query);
        std::sort(found.begin(), found.end());
        Check(found == reference, "Octree range query matches brute force AABB overlap");
    }
    Rejects([&] { octree.Build(scene.triangles, 0); }, "Octree rejects zero capacity");
    Rejects([&] { octree.Query({{1, 0, 0}, {0, 1, 1}}); }, "Octree rejects inverted query bounds");
    octree.Build({});
    Check(octree.Nodes().empty() && octree.Query({{-1, -1, -1}, {1, 1, 1}}).empty(), "empty Octree handles queries");
}

void GraphHazards() {
    using namespace render;
    RenderGraph graph;
    // 完整覆盖不消费旧值，历史排序边不能把无用旧值和读者保活。
    const auto value = graph.Import("history", UnorderedAccessState, true, false);
    const auto sink = graph.Import("sink", 0, false, false);
    graph.AddPass("first writer", {{value, UnorderedAccessState, true}}, [] {});
    graph.AddPass("old value reader", {{value, 2, false}, {sink, 1, true}}, [] {});
    graph.AddPass("overwrite", {{value, UnorderedAccessState, true}}, [] {});
    graph.AddPass("second overwrite", {{value, UnorderedAccessState, true}}, [] {});
    graph.Export(value, UnorderedAccessState);
    const auto& plan = graph.Compile();
    Check(plan.size() == 1 && plan[0].name == "second overwrite", "overwritten values and unused WAR readers are culled");
    Check(plan[0].barriers.size() == 1 && plan[0].barriers[0].uav, "write after imported writable resource synchronizes");
    Check(plan.back().barriers.size() == 1 && plan.back().barriers[0].uav, "same state write requires UAV ordering");

    // 保留的旧值读者仍必须先于覆盖执行，但不再保留夹在中间的无用写入。
    RenderGraph activeReaders;
    const auto shared = activeReaders.Import("shared", 0, false, false);
    const auto observed = activeReaders.Import("observed", 0, false, false);
    activeReaders.AddPass("producer", {{shared, 1, true}}, [] {});
    activeReaders.AddPass("live reader", {{shared, 2, false}, {observed, 1, true}}, [] {});
    activeReaders.AddPass("dead overwrite", {{shared, 1, true}}, [] {});
    activeReaders.AddPass("final overwrite", {{shared, 1, true}}, [] {});
    activeReaders.Export(shared, 2);
    activeReaders.Export(observed, 2);
    const auto& activePlan = activeReaders.Compile();
    Check(activePlan.size() == 3 && activePlan[0].name == "producer" && activePlan[1].name == "live reader" &&
        activePlan[2].name == "final overwrite", "live RAW and WAR ordering survives dead overwrite culling");

    // 外部资源写入即使没有 Export，仍然会改变图外能够观察到的内容。
    RenderGraph externalWrites;
    const auto external = externalWrites.Import("external output", 0, false);
    externalWrites.AddPass("external write", {{external, 1, true}}, [] {});
    Check(externalWrites.Compile().size() == 1, "imported writes are observable roots by default");

    // 原地 UAV 修改既消费旧值也生产新值，不能按完整覆盖裁掉原生产者。
    RenderGraph readWrite;
    const auto accumulated = readWrite.Import("accumulated", 0, false, false);
    readWrite.AddPass("initialize", {{accumulated, UnorderedAccessState, ResourceAccess::Write}}, [] {});
    readWrite.AddPass("accumulate", {{accumulated, UnorderedAccessState, ResourceAccess::ReadWrite}}, [] {});
    readWrite.Export(accumulated, UnorderedAccessState);
    const auto& accumulatedPlan = readWrite.Compile();
    Check(accumulatedPlan.size() == 2 && accumulatedPlan[1].barriers[0].uav,
        "read write keeps the prior value and orders UAV modifications");

    RenderGraph sideEffects;
    sideEffects.AddPass("external action", {}, [] {}, true);
    Check(sideEffects.Compile().size() == 1, "side effect root retained");

    RenderGraph readers;
    const auto imported = readers.Import("valid input", 3);
    readers.AddPass("first read", {{imported, 3, false}}, [] {}, true);
    readers.AddPass("second read", {{imported, 3, false}}, [] {}, true);
    const auto& readPlan = readers.Compile();
    Check(readPlan[0].barriers.empty() && readPlan[1].barriers.empty(), "same state read needs no barriers");

    RenderGraph renderTargets;
    const auto target = renderTargets.Import("render target", 4);
    renderTargets.AddPass("draw", {{target, 4, true}}, [] {});
    renderTargets.AddPass("overlay", {{target, 4, true}}, [] {});
    renderTargets.Export(target, 4);
    const auto& draws = renderTargets.Compile();
    Check(draws[0].barriers.empty() && draws[1].barriers.empty(), "same-state render target writes do not use UAV barriers");

    RenderGraph explicitOrder;
    const auto first = explicitOrder.AddPass("first declared", {}, [] {}, true);
    explicitOrder.AddPass("independent", {}, [] {}, true);
    const auto last = explicitOrder.AddPass("explicit predecessor", {}, [] {}, true);
    explicitOrder.AddDependency(last, first);
    const auto& sorted = explicitOrder.Compile();
    Check(sorted[0].name == "independent" && sorted[1].id == last && sorted[2].id == first,
          "explicit edges override declaration order while independent nodes remain stable");
}

// 最终覆盖层只约束仍然存活的工作顺序，不能把未被观察的中间结果保活。
void GraphOrdering() {
    using namespace render;
    RenderGraph culled;
    const ResourceDescriptor buffer{ResourceKind::Buffer, 256};
    const auto unused = culled.Create("unused intermediate", buffer);
    std::vector<std::string> executed;
    const auto dead = culled.AddPass("dead intermediate", {{unused, 1, ResourceAccess::Write}},
        [&] { executed.emplace_back("dead"); });
    const auto overlay = culled.AddPass("live overlay", {}, [&] { executed.emplace_back("overlay"); }, true);
    culled.AddOrdering(dead, overlay);
    const auto& culledPlan = culled.Compile();
    Check(culledPlan.size() == 1 && culledPlan[0].id == overlay,
        "ordering before a live overlay must not retain a dead pass");
    Check(culled.Lifetimes()[unused].allocation == InvalidGraphIndex && culled.Allocations().empty(),
        "ordering edges must not allocate storage for discarded work");
    culled.Execute([](const Barrier&) {});
    Check(executed == std::vector<std::string>{"overlay"}, "culled ordering predecessors must never execute");

    // 两端独立保活时，顺序边必须覆盖声明顺序，并产生真实回调执行顺序。
    RenderGraph ordered;
    executed.clear();
    const auto keepA = ordered.AddPass("keep A", {}, [&] { executed.emplace_back("A"); }, true);
    const auto keepB = ordered.AddPass("keep B", {}, [&] { executed.emplace_back("B"); }, true);
    ordered.AddOrdering(keepB, keepA);
    const auto& orderedPlan = ordered.Compile();
    Check(orderedPlan.size() == 2 && orderedPlan[0].id == keepB && orderedPlan[1].id == keepA,
        "ordering edges must order independently retained passes");
    ordered.Execute([](const Barrier&) {});
    Check(executed == std::vector<std::string>({"B", "A"}), "ordering edges must control execution order");
    ordered.AddOrdering(keepB, keepA);
    Rejects([&] { ordered.Execute([](const Barrier&) {}); }, "adding ordering invalidates the compiled plan");

    // 既有依赖仍表达消费者必须执行的前置工作，不能退化为纯顺序提示。
    RenderGraph dependency;
    const auto producer = dependency.AddPass("required producer", {}, [] {});
    const auto consumer = dependency.AddPass("live consumer", {}, [] {}, true);
    dependency.AddDependency(producer, consumer);
    const auto& dependencyPlan = dependency.Compile();
    Check(dependencyPlan.size() == 2 && dependencyPlan[0].id == producer && dependencyPlan[1].id == consumer,
        "AddDependency must retain its explicit producer");

    // 无效边和周期必须在裁剪之前报错，即使全部相关节点原本都不存活。
    RenderGraph invalidOrdering;
    const auto validOrdering = invalidOrdering.AddPass("unused ordering node", {}, [] {});
    invalidOrdering.AddOrdering(validOrdering, validOrdering + 1);
    Rejects([&] { invalidOrdering.Compile(); }, "ordering edges reject invalid destination indices before culling");
    RenderGraph invalidDependency;
    const auto validDependency = invalidDependency.AddPass("unused dependency node", {}, [] {});
    invalidDependency.AddDependency(validDependency + 1, validDependency);
    Rejects([&] { invalidDependency.Compile(); }, "dependency edges reject invalid source indices before culling");
    RenderGraph cycle;
    const auto first = cycle.AddPass("dead A", {}, [] {});
    const auto second = cycle.AddPass("dead B", {}, [] {});
    cycle.AddOrdering(first, second);
    cycle.AddOrdering(second, first);
    Rejects([&] { cycle.Compile(); }, "ordering cycles between discarded nodes must still be rejected");
    RenderGraph mixedCycle;
    const auto before = mixedCycle.AddPass("mixed A", {}, [] {});
    const auto after = mixedCycle.AddPass("mixed B", {}, [] {});
    mixedCycle.AddOrdering(before, after);
    mixedCycle.AddDependency(after, before);
    Rejects([&] { mixedCycle.Compile(); }, "cycles spanning ordering and dependency edges must be rejected");
    RenderGraph selfCycle;
    const auto self = selfCycle.AddPass("self ordering", {}, [] {});
    selfCycle.AddOrdering(self, self);
    Rejects([&] { selfCycle.Compile(); }, "self ordering must be rejected before culling");
}

void GraphValidation() {
    using namespace render;
    RenderGraph uninitialized;
    const auto temporary = uninitialized.Import("uninitialized", 0, false);
    uninitialized.AddPass("bad read", {{temporary, 2, false}}, [] {}, true);
    Rejects([&] { uninitialized.Compile(); }, "uninitialized reads rejected");
    // 不能用声明为 UAV 写入来掩盖着色器对尚未初始化内容的读取。
    RenderGraph uninitializedReadWrite;
    const auto emptyUav = uninitializedReadWrite.Import("empty UAV", 0, false, false);
    uninitializedReadWrite.AddPass("bad modify", {{emptyUav, UnorderedAccessState, ResourceAccess::ReadWrite}}, [] {}, true);
    Rejects([&] { uninitializedReadWrite.Compile(); }, "read write requires initialized contents");
    RenderGraph deadInvalid;
    const auto dead = deadInvalid.Import("dead input", 0, false);
    deadInvalid.AddPass("dead invalid read", {{dead, 2, false}}, [] {});
    Rejects([&] { deadInvalid.Compile(); }, "validation occurs before culling");
    RenderGraph duplicates;
    const auto repeated = duplicates.Import("duplicate", 0);
    duplicates.AddPass("bad use", {{repeated, 1, false}, {repeated, 1, true}}, [] {}, true);
    Rejects([&] { duplicates.Compile(); }, "conflicting duplicate uses rejected");
    RenderGraph invalidHandle;
    invalidHandle.AddPass("invalid handle", {{99, 1, false}}, [] {}, true);
    Rejects([&] { invalidHandle.Compile(); }, "invalid resource handle rejected");
    RenderGraph cycle;
    const auto first = cycle.AddPass("first", {}, [] {}, true);
    const auto second = cycle.AddPass("second", {}, [] {}, true);
    cycle.AddDependency(first, second);
    cycle.AddDependency(second, first);
    Rejects([&] { cycle.Compile(); }, "explicit dependency cycle rejected");
    RenderGraph badExport;
    const auto unwritten = badExport.Import("unwritten output", 0, false);
    badExport.Export(unwritten, 0);
    Rejects([&] { badExport.Compile(); }, "uninitialized exported contents rejected");
    RenderGraph dirty;
    dirty.AddPass("first", {}, [] {}, true);
    dirty.Compile();
    dirty.AddPass("second", {}, [] {}, true);
    Rejects([&] { dirty.Execute([](const Barrier&) {}); }, "mutating graph invalidates compiled plan");
}

void GraphAccessValidation() {
    using namespace render;
    RenderGraph graph;
    const auto input = graph.Import("input", 6);
    const auto hidden = graph.Import("hidden input", 2);
    bool executed = false;
    // 只有正在录制且已声明此资源的 Pass，才能解析相符的原生访问。
    Rejects([&] { graph.AssertAccess(input, ResourceAccess::Read, 2); }, "setup cannot resolve native resources");
    graph.AddPass("validated read", {{input, 6, ResourceAccess::Read}}, [&] {
        graph.AssertAccess(input, ResourceAccess::Read, 2);
        Rejects([&] { graph.AssertAccess(input, ResourceAccess::Write, 2); }, "read declaration cannot resolve writes");
        Rejects([&] { graph.AssertAccess(input, ResourceAccess::Read, 8); }, "native access requires matching state bits");
        Rejects([&] { graph.AssertAccess(input, ResourceAccess::Read, 0); }, "COMMON is not a wildcard state");
        Rejects([&] { graph.AssertAccess(hidden, ResourceAccess::Read, 2); }, "undeclared native resource access is rejected");
        Rejects([&] { graph.AddPass("reentrant", {}, [] {}); }, "pass callbacks cannot mutate their graph");
        Rejects([&] { graph.Import("reentrant input", 0); }, "pass callbacks cannot import resources");
        Rejects([&] { graph.Export(input, 6); }, "pass callbacks cannot change graph outputs");
        Rejects([&] { graph.Compile(); }, "pass callbacks cannot recompile their graph");
        Rejects([&] { graph.Execute([](const Barrier&) {}); }, "graph execution cannot reenter itself");
        executed = true;
    }, true);
    graph.Compile();
    Check(graph.Plan()[0].uses.size() == 1, "compiled pass exposes authoritative accesses");
    graph.Execute([](const Barrier&) {}, {}, [&](const CompiledPass&) {
        Rejects([&] { graph.AssertAccess(input, ResourceAccess::Read, 2); }, "execution hooks do not grant native access");
    });
    Check(executed, "declared resource resolves during pass recording");
    Rejects([&] { graph.AssertAccess(input, ResourceAccess::Read, 2); }, "native access expires after execution");

    // 回调抛异常也必须撤销访问授权，并允许调用方随后重新建图。
    RenderGraph failing;
    const auto resource = failing.Import("failure input", 2);
    failing.AddPass("throws", {{resource, 2, ResourceAccess::Read}}, [] { throw std::runtime_error("recording failed"); }, true);
    failing.Compile();
    Rejects([&] { failing.Execute([](const Barrier&) {}); }, "recording exception propagates");
    Rejects([&] { failing.AssertAccess(resource, ResourceAccess::Read, 2); }, "failed pass cannot retain resource access");
    failing.AddPass("after failure", {}, [] {}, true);
}

void GraphTransientLifetimes() {
    using namespace render;
    RenderGraph graph;
    ResourceDescriptor description;
    description.kind = ResourceKind::Texture2D;
    description.width = 32;
    description.height = 16;
    description.format = 2;
    const auto first = graph.Create("first temporary", description);
    const auto second = graph.Create("second temporary", description);
    const auto overlapping = graph.Create("overlapping temporary", description);
    const auto culled = graph.Create("unreachable temporary", description);
    graph.AddPass("first producer", {{first, 1, true}, {overlapping, 1, true}}, [] {}, true);
    graph.AddPass("first consumer", {{first, 2, false}}, [] {}, true);
    graph.AddPass("second producer", {{second, 1, true}, {overlapping, 2, false}}, [] {}, true);
    graph.AddPass("discarded producer", {{culled, 1, true}}, [] {});
    graph.Export(second, 2);
    graph.Compile();
    // 仅规格兼容仍不足以复用，重叠寿命必须保留独立分配。
    const auto& lifetimes = graph.Lifetimes();
    Check(lifetimes[first].allocation == lifetimes[second].allocation, "nonoverlapping compatible temporaries reuse storage");
    Check(lifetimes[overlapping].allocation != lifetimes[first].allocation, "overlapping temporaries cannot share storage");
    Check(lifetimes[culled].allocation == InvalidGraphIndex, "culled temporaries never allocate");
    Check(lifetimes[second].lastPass == graph.Plan().size(), "export extends resource lifetime through final transitions");
    Check(graph.Allocations().size() == 2, "physical slot count reflects retained lifetimes");
    std::vector<bool> resident(graph.Resources().size());
    std::size_t acquisitions{}, releases{};
    graph.Execute([&](const Barrier& barrier) { Check(resident[barrier.resource], "barriers require resident storage"); },
        [&](ResourceId id, bool acquire) {
            Check(resident[id] != acquire, "each transient lifetime has one acquire and release");
            resident[id] = acquire;
            acquire ? ++acquisitions : ++releases;
        });
    Check(acquisitions == 3 && releases == 3, "only live resources are acquired and released");

    // 新的图逻辑资源不能继承旧池分配中的可读内容。
    RenderGraph invalid;
    const auto temporary = invalid.Create("unwritten", description);
    invalid.AddPass("invalid reuse read", {{temporary, 2, false}}, [] {}, true);
    Rejects([&] { invalid.Compile(); }, "transient first reads require a graph writer");
    description.width = 0;
    Rejects([&] { invalid.Create("invalid extent", description); }, "zero extent transient rejected");

    // 不重叠寿命只有在全部分配属性兼容时才允许复用。
    RenderGraph incompatible;
    description.width = 16;
    const auto small = incompatible.Create("small", description);
    description.width = 32;
    const auto large = incompatible.Create("large", description);
    incompatible.AddPass("small writer", {{small, 1, true}}, [] {}, true);
    incompatible.AddPass("large writer", {{large, 1, true}}, [] {}, true);
    incompatible.Compile();
    Check(incompatible.Allocations().size() == 2, "different extents cannot share one resource allocation");
}

void ReservoirValidation() {
    using namespace render;
    WeightedReservoir<int> reservoir;
    reservoir.Update(7, 0, 0, 0.5);
    Check(reservoir.Members() == 1 && !reservoir.HasSample(), "zero weight candidate counts without selection");
    reservoir.Update(9, 4, 2, 0.2);
    Check(reservoir.Sample() == 9 && reservoir.Members() == 2, "positive candidate selection");
    Check(std::abs(reservoir.Normalization() - 1) < 1e-12, "reservoir normalized weight");
    const auto weightBefore = reservoir.WeightSum();
    Rejects([&] { reservoir.Update(0, std::numeric_limits<double>::quiet_NaN(), 1, 0); }, "NaN weight rejected");
    Rejects([&] { reservoir.Update(0, -1, 1, 0); }, "negative weight rejected");
    Rejects([&] { reservoir.Update(0, 1, 0, 0); }, "positive weight requires positive target");
    Rejects([&] { reservoir.Update(0, 1, 1, 1); }, "random number must be below one");
    Check(reservoir.WeightSum() == weightBefore && reservoir.Members() == 2, "invalid update is transactional");
    reservoir.ClampHistory(1);
    Check(reservoir.Members() == 1 && std::abs(reservoir.WeightSum() - 2) < 1e-12, "clamp rescales mass proportionally");
    Check(std::abs(reservoir.Normalization() - 1) < 1e-12, "clamp preserves estimator normalization");
    WeightedReservoir<int> merged;
    merged.Merge(reservoir, 4, 0.1);
    Check(merged.Sample() == 9 && merged.Members() == 1, "merge carries sample count");
    Check(std::abs(merged.WeightSum() - 4) < 1e-12, "merge reevaluates receiver target");
    WeightedReservoir<int> zero;
    zero.Update(0, 0, 0, 0);
    merged.Merge(zero, 0, 0);
    Check(merged.Members() == 2, "zero weight merged stream still counts candidates");
    Check(std::abs(merged.Normalization() - 0.5) < 1e-12, "zero stream affects denominator");
}

void ReservoirStatistics() {
    using namespace render;
    std::mt19937_64 generator(0x726573746972);
    std::uniform_real_distribution<double> random(0, 1);
    int selected[3] = {};
    double integrandEstimate = 0;
    constexpr int trials = 100000;
    const double targets[] = {1, 3, 6};
    for (int trial = 0; trial < trials; ++trial) {
        WeightedReservoir<int> reservoir;
        for (int candidate = 0; candidate < 3; ++candidate) {
            reservoir.Update(candidate, targets[candidate], targets[candidate], random(generator));
        }
        ++selected[reservoir.Sample()];
        // 三个等概率离散提案上的常数被积函数积分应为一。
        integrandEstimate += reservoir.Normalization();
    }
    Check(std::abs(selected[0] / double(trials) - 0.1) < 0.006, "weighted frequency for first candidate");
    Check(std::abs(selected[1] / double(trials) - 0.3) < 0.006, "weighted frequency for middle candidate");
    Check(std::abs(selected[2] / double(trials) - 0.6) < 0.006, "weighted frequency for final candidate");
    Check(std::abs(integrandEstimate / trials - 1) < 0.015, "constant integrand expectation");

    // 流合并后的概率质量分布必须匹配完整单流。
    int mergedSecond = 0;
    for (int trial = 0; trial < trials; ++trial) {
        WeightedReservoir<int> first;
        WeightedReservoir<int> second;
        first.Update(0, 1, 1, random(generator));
        second.Update(1, 3, 3, random(generator));
        first.Merge(second, 3, random(generator));
        mergedSecond += first.Sample() == 1;
    }
    Check(std::abs(mergedSecond / double(trials) - 0.75) < 0.006, "merge selection frequency");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"math and bounds", MathAndBounds}, {"Cornell scene and BVH", SceneAndBvh},
        {"Octree range queries", OctreeQueries},
        {"graph scheduling", GraphScheduling}, {"graph hazards", GraphHazards},
        {"graph ordering", GraphOrdering},
        {"graph validation", GraphValidation}, {"graph access validation", GraphAccessValidation},
        {"graph transient lifetimes", GraphTransientLifetimes},
        {"reservoir validation", ReservoirValidation},
        {"reservoir statistics", ReservoirStatistics},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failed) << "/" << tests.size() << " core tests passed\n";
    return failed ? 1 : 0;
}
