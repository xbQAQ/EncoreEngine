#include "FbxInternal.h"
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace render::fbx {
namespace {
Vec3 Vector(ufbx_vec3 value) {
    return {Finite(value.x, "几何分量"), Finite(value.y, "几何分量"), Finite(value.z, "几何分量")};
}
Float4 Lane(Vec3 value, float w = 0) { return {value.x, value.y, value.z, w}; }
bool Textured(const GpuMaterial &m) {
    return m.textures.x != ~0u || m.textures.y != ~0u || m.textures.z != ~0u || m.textures.w != ~0u ||
           m.textures2.x != ~0u || m.textures2.y != ~0u || m.textures2.z != ~0u || m.textures2.w != ~0u;
}
void Triangle(ImportContext &context, const ufbx_node &node, std::array<std::uint32_t, 3> ids, std::uint32_t material,
              const ufbx_matrix &normals, double determinant) {
    const auto &mesh = *node.mesh;
    // 负缩放需要同步翻转角点顺序，法线仍按逆转置保留作者正面。
    if (determinant < 0)
        std::swap(ids[1], ids[2]);
    std::array<Vec3, 3> p{}, n{};
    std::array<std::array<float, 2>, 3> uv{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto position = ufbx_get_vertex_vec3(&mesh.skinned_position, ids[i]);
        const auto normal = ufbx_get_vertex_vec3(&mesh.skinned_normal, ids[i]);
        // 骨骼结果可能已在世界空间，不能再次叠加节点或几何专用变换。
        p[i] = Vector(mesh.skinned_is_local ? ufbx_transform_position(&node.geometry_to_world, position) : position);
        n[i] = Normalize(Vector(mesh.skinned_is_local ? ufbx_transform_direction(&normals, normal) : normal));
        Require(Length(n[i]) > 0, "网格包含零长度法线");
        if (mesh.vertex_uv.exists) {
            const auto source = ufbx_get_vertex_vec2(&mesh.vertex_uv, ids[i]);
            uv[i] = {Finite(source.x, "UV 横坐标"), Finite(1 - source.y, "UV 纵坐标")};
        }
    }
    const auto geometric = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
    Require(Length(geometric) > 0, "网格包含退化三角形");
    Vec3 average = Normalize(n[0] + n[1] + n[2]);
    if (Length(average) == 0)
        average = geometric;
    // GPU 每个三角形保留一组切线，优先消费作者切线并配合 V 翻转恢复手性。
    Vec3 tangent{}, bitangent{};
    if (mesh.vertex_tangent.exists && mesh.all_deformers.count == 0) {
        for (unsigned i = 0; i < 3; ++i) {
            tangent += Vector(
                ufbx_transform_direction(&node.geometry_to_world, ufbx_get_vertex_vec3(&mesh.vertex_tangent, ids[i])));
            if (mesh.vertex_bitangent.exists)
                bitangent += Vector(ufbx_transform_direction(&node.geometry_to_world,
                                                             ufbx_get_vertex_vec3(&mesh.vertex_bitangent, ids[i]))) *
                             -1;
        }
    }
    const double du1 = double(uv[1][0]) - uv[0][0], dv1 = double(uv[1][1]) - uv[0][1];
    const double du2 = double(uv[2][0]) - uv[0][0], dv2 = double(uv[2][1]) - uv[0][1];
    const auto uvDeterminant = du1 * dv2 - du2 * dv1;
    if (std::abs(uvDeterminant) > 1e-12) {
        if (Length(tangent) == 0)
            tangent = (p[1] - p[0]) * Finite(dv2 / uvDeterminant, "切线系数") -
                      (p[2] - p[0]) * Finite(dv1 / uvDeterminant, "切线系数");
        if (Length(bitangent) == 0)
            bitangent = (p[2] - p[0]) * Finite(du1 / uvDeterminant, "副切线系数") -
                        (p[1] - p[0]) * Finite(du2 / uvDeterminant, "副切线系数");
    }
    tangent = Normalize(tangent - average * Dot(tangent, average));
    if (Length(tangent) == 0)
        tangent = Normalize(Cross(std::abs(average.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, average));
    const float handedness = Dot(Cross(average, tangent), bitangent) < 0 ? -1.0f : 1.0f;
    // 属性索引随三角形保存，后续 BVH 重排不会割裂平滑法线或 UV。
    Require(context.scene.attributes.size() < 0x00ffffffu, "三角形属性超过精确浮点索引容量");
    context.scene.attributes.push_back({Lane(n[0], handedness),
                                        Lane(n[1]),
                                        Lane(n[2]),
                                        {uv[0][0], uv[0][1], uv[1][0], uv[1][1]},
                                        {uv[2][0], uv[2][1], 0, 0},
                                        Lane(tangent, handedness)});
    context.scene.triangles.push_back({Lane(p[0], static_cast<float>(material)),
                                       Lane(p[1], static_cast<float>(context.scene.attributes.size())), Lane(p[2])});
}
}
void ImportGeometry(ImportContext &context) {
    for (const auto *node : context.data.nodes) {
        if (!node->mesh)
            continue;
        const auto &mesh = *node->mesh;
        // 实例材质以节点为准，不能误用共享几何上的默认材质表。
        Require(!mesh.vertex_color.exists, "暂不支持顶点颜色，请先烘焙到基础色纹理");
        Require(mesh.num_empty_faces == 0 && mesh.num_line_faces == 0 && mesh.num_point_faces == 0,
                "网格包含非表面原语");
        const double determinant = ufbx_matrix_determinant(&node->geometry_to_world);
        Require(std::isfinite(determinant) && std::abs(determinant) > 1e-20, "节点变换包含奇异缩放");
        const auto normals = ufbx_matrix_for_normals(&node->geometry_to_world);
        Require(mesh.vertex_position.exists && mesh.vertex_normal.exists, "网格缺少有效位置或法线");
        Require(mesh.max_face_triangles <= std::numeric_limits<std::size_t>::max() / 3, "多边形三角化缓冲超过容量");
        std::vector<std::uint32_t> indices(mesh.max_face_triangles * 3);
        for (const auto &part : mesh.material_parts) {
            Require(node->materials.count == 0 || part.index < node->materials.count, "节点未绑定几何使用的材质槽");
            const auto *source = part.index < node->materials.count ? node->materials[part.index] : nullptr;
            const auto material = source ? source->typed_id : static_cast<std::uint32_t>(context.data.materials.count);
            Require(material < context.scene.materials.size(), "网格材质引用越界");
            Require(mesh.vertex_uv.exists || !Textured(context.scene.materials[material]), "带纹理的表面缺少 UV");
            for (const auto &required : context.materialUvSets[material])
                Require(mesh.uv_sets.count && Name(mesh.uv_sets[0].name, "") == required,
                        "材质需要第一 UV 集之外的坐标集：" + required);
            const auto first = static_cast<std::uint32_t>(context.scene.triangles.size());
            // 官方三角化器处理凹多边形；每组材质单独输出连续范围供 ECS 精确关联。
            for (const auto faceIndex : part.face_indices) {
                Require(faceIndex < mesh.faces.count, "材质表面引用不存在的多边形");
                Require(mesh.face_hole.count == 0 || !mesh.face_hole[faceIndex], "不支持带孔多边形，请先三角化导出");
                const auto count = ufbx_triangulate_face(indices.data(), indices.size(), &mesh, mesh.faces[faceIndex]);
                Require(count && std::size_t(count) * 3 <= indices.size(), "多边形无法三角化");
                for (std::size_t i = 0; i < std::size_t(count) * 3; i += 3)
                    Triangle(context, *node, {indices[i], indices[i + 1], indices[i + 2]}, material, normals,
                             determinant);
            }
            const auto count = static_cast<std::uint32_t>(context.scene.triangles.size()) - first;
            if (count)
                context.scene.meshParts.push_back(
                    {Name(node->name, "节点") + "/" + Name(mesh.name, "网格") + "/" + std::to_string(part.index), first,
                     count, material});
        }
    }
    Require(!context.scene.triangles.empty(), "模型没有可导入的静态三角形表面");
}
}
