#include "PrimitiveFactory.h"
#include <cmath>
#include <stdexcept>

namespace render {
namespace {
Float4 Position(Vec3 p, float tag = 0) {
    return {p.x, p.y, p.z, tag};
}
Float4 Lane(const config::Vec4 &v) {
    return {v.x(), v.y(), v.z(), v.w()};
}
void Quad(Scene &scene, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    // 每个三角形拥有稳定的 UV 和切线记录，避免材质脚本依赖临时几何回退。
    const auto n = Normalize(Cross(b - a, c - a));
    const auto t = Normalize(d - a);
    const Float4 normal{n.x, n.y, n.z, -1}, tangent{t.x, t.y, t.z, -1};
    scene.attributes.push_back({normal, normal, normal, {0, 0, 0, 1}, {1, 1, 0, 0}, tangent});
    scene.triangles.push_back({Position(a), Position(b, static_cast<float>(scene.attributes.size())), Position(c)});
    scene.attributes.push_back({normal, normal, normal, {0, 0, 1, 1}, {1, 0, 0, 0}, tangent});
    scene.triangles.push_back({Position(a), Position(c, static_cast<float>(scene.attributes.size())), Position(d)});
}
Vec3 Rotate(Vec3 p, const config::Vec3 &r) {
    // 变换顺序与 ECS 提取一致：先绕 X，再绕 Y，最后绕 Z。
    const auto basis = MakeCameraBasis(r.x(), r.y(), r.z());
    return basis.right * p.x + basis.up * p.y + basis.forward * p.z;
}
} // 匿名命名空间

Scene BuildPrimitive(const config::MeshAsset &mesh) {
    Scene result;
    if (mesh.has_quad()) {
        const auto &q = mesh.quad();
        Quad(result, ReadVector(q.a()), ReadVector(q.b()), ReadVector(q.c()), ReadVector(q.d()));
    } else if (mesh.has_box()) {
        const auto size = ReadVector(mesh.box().size());
        const float x = size.x * 0.5f, y = size.y, z = size.z * 0.5f;
        // 盒体以底面中心为原点，所有面保持朝外绕序。
        Quad(result, {-x, y, -z}, {-x, y, z}, {x, y, z}, {x, y, -z});
        Quad(result, {-x, 0, -z}, {x, 0, -z}, {x, 0, z}, {-x, 0, z});
        Quad(result, {-x, 0, -z}, {-x, y, -z}, {x, y, -z}, {x, 0, -z});
        Quad(result, {x, 0, z}, {x, y, z}, {-x, y, z}, {-x, 0, z});
        Quad(result, {-x, 0, z}, {-x, y, z}, {-x, y, -z}, {-x, 0, -z});
        Quad(result, {x, 0, -z}, {x, y, -z}, {x, y, z}, {x, 0, z});
    } else if (mesh.has_data()) {
        // 模型导入器通过嵌入数据边界接入，不使 RenderCore 反向依赖 AssetImport。
        for (const auto &a : mesh.data().attributes())
            result.attributes.push_back({Lane(a.normal0()), Lane(a.normal1()), Lane(a.normal2()), Lane(a.uv01()),
                                         Lane(a.uv2()), Lane(a.tangent())});
        for (const auto &t : mesh.data().triangles())
            result.triangles.push_back({Position(ReadVector(t.a())),
                                        Position(ReadVector(t.b()), static_cast<float>(t.attribute())),
                                        Position(ReadVector(t.c()))});
    } else
        throw std::invalid_argument("网格没有可生成的原语或嵌入几何");
    return result;
}

void ApplyDocumentTransform(Scene &geometry, const config::Transform &transform) {
    const auto &s = transform.scale();
    const Vec3 scale{s.has_x() ? s.x() : 1, s.has_y() ? s.y() : 1, s.has_z() ? s.z() : 1};
    const auto position = ReadVector(transform.position());
    const bool mirrored = double(scale.x) * scale.y * scale.z < 0;
    // 法线使用逆缩放，镜像变换同时调整绕序、UV 和切线手性。
    for (auto &triangle : geometry.triangles) {
        for (auto *vertex : {&triangle.a, &triangle.b, &triangle.c}) {
            const auto p =
                Rotate({vertex->x * scale.x, vertex->y * scale.y, vertex->z * scale.z}, transform.rotation_degrees()) +
                position;
            if (!IsFinite(p))
                throw std::invalid_argument("对象变换超出有限浮点坐标范围");
            *vertex = Position(p, vertex->w);
        }
        if (mirrored) {
            const auto b = triangle.b;
            triangle.b = {triangle.c.x, triangle.c.y, triangle.c.z, b.w};
            triangle.c = {b.x, b.y, b.z, triangle.c.w};
        }
    }
    for (auto &attribute : geometry.attributes) {
        for (auto *n : {&attribute.normal0, &attribute.normal1, &attribute.normal2}) {
            const auto value =
                Normalize(Rotate({n->x / scale.x, n->y / scale.y, n->z / scale.z}, transform.rotation_degrees()));
            *n = Position(value, n->w);
        }
        auto &t = attribute.tangent;
        t = Position(Normalize(Rotate({t.x * scale.x, t.y * scale.y, t.z * scale.z}, transform.rotation_degrees())),
                     t.w);
        if (mirrored) {
            std::swap(attribute.normal1, attribute.normal2);
            const auto uv = attribute.uv01;
            attribute.uv01 = {uv.x, uv.y, attribute.uv2.x, attribute.uv2.y};
            attribute.uv2 = {uv.z, uv.w, 0, 0};
            attribute.normal0.w *= -1;
            attribute.tangent.w *= -1;
        }
    }
}
} // 渲染命名空间
