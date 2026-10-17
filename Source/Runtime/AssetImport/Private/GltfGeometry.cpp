#include "GltfInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace render::gltf {
namespace {
struct Matrix {
    std::array<float, 16> values{}; // Column-major node-to-world affine transform supplied by cgltf.
    double determinant{};           // Linear determinant controls normal inversion and mirrored winding.
    explicit Matrix(const cgltf_node &node) {
        // Validate the complete inherited affine transform before normals or world positions can use it.
        cgltf_node_transform_world(&node, values.data());
        for (float value : values)
            Require(std::isfinite(value), "node world transform contains a nonfinite value");
        const auto &m = values;
        determinant = double(m[0]) * (double(m[5]) * m[10] - double(m[9]) * m[6]) -
                      double(m[4]) * (double(m[1]) * m[10] - double(m[9]) * m[2]) +
                      double(m[8]) * (double(m[1]) * m[6] - double(m[5]) * m[2]);
        Require(std::isfinite(determinant) && std::abs(determinant) > 1e-20, "node transform has a singular scale");
    }
    Vec3 Point(const Vec3 &v) const {
        const auto &m = values;
        return {Finite(double(m[0]) * v.x + double(m[4]) * v.y + double(m[8]) * v.z + m[12], "world position"),
                Finite(double(m[1]) * v.x + double(m[5]) * v.y + double(m[9]) * v.z + m[13], "world position"),
                Finite(double(m[2]) * v.x + double(m[6]) * v.y + double(m[10]) * v.z + m[14], "world position")};
    }
    Vec3 Direction(const Vec3 &v) const {
        const auto &m = values;
        return {Finite(double(m[0]) * v.x + double(m[4]) * v.y + double(m[8]) * v.z, "world direction"),
                Finite(double(m[1]) * v.x + double(m[5]) * v.y + double(m[9]) * v.z, "world direction"),
                Finite(double(m[2]) * v.x + double(m[6]) * v.y + double(m[10]) * v.z, "world direction")};
    }
    Vec3 Normal(const Vec3 &v) const {
        const auto &m = values;
        // Cofactors form inverse-transpose without assuming orthogonal or uniformly scaled nodes.
        const double x = (double(m[5]) * m[10] - double(m[9]) * m[6]) * v.x +
                         (double(m[9]) * m[2] - double(m[1]) * m[10]) * v.y +
                         (double(m[1]) * m[6] - double(m[5]) * m[2]) * v.z;
        const double y = (double(m[8]) * m[6] - double(m[4]) * m[10]) * v.x +
                         (double(m[0]) * m[10] - double(m[8]) * m[2]) * v.y +
                         (double(m[4]) * m[2] - double(m[0]) * m[6]) * v.z;
        const double z = (double(m[4]) * m[9] - double(m[8]) * m[5]) * v.x +
                         (double(m[8]) * m[1] - double(m[0]) * m[9]) * v.y +
                         (double(m[0]) * m[5] - double(m[4]) * m[1]) * v.z;
        return Normalize({Finite(x / determinant, "world normal"), Finite(y / determinant, "world normal"),
                          Finite(z / determinant, "world normal")});
    }
};
std::vector<float> Floats(const cgltf_accessor *accessor, cgltf_type type, const char *description) {
    if (!accessor)
        return {};
    Require(accessor->type == type, std::string(description) + " accessor has the wrong vector size");
    const auto components = cgltf_num_components(type);
    Require(accessor->count <= std::numeric_limits<std::size_t>::max() / components,
            std::string(description) + " accessor is too large");
    // cgltf applies sparse patches and normalized integer conversion through the same unpack operation.
    std::vector<float> result(accessor->count * components);
    Require(cgltf_accessor_unpack_floats(accessor, result.data(), result.size()) == result.size(),
            std::string(description) + " accessor cannot be unpacked");
    for (float value : result)
        Require(std::isfinite(value), std::string(description) + " accessor contains a nonfinite value");
    return result;
}
Vec3 Vector(const std::vector<float> &values, std::size_t index, std::size_t stride = 3) {
    return {values[index * stride], values[index * stride + 1], values[index * stride + 2]};
}
Float4 Lane(const Vec3 &value, float w = 0) {
    return {value.x, value.y, value.z, w};
}
bool HasTextures(const GpuMaterial &material) {
    return material.textures.x != ~0u || material.textures.y != ~0u || material.textures.z != ~0u ||
           material.textures.w != ~0u || material.textures2.x != ~0u || material.textures2.y != ~0u ||
           material.textures2.z != ~0u || material.textures2.w != ~0u;
}
void Primitive(ImportContext &context, const cgltf_node &node, const cgltf_primitive &primitive,
               std::size_t primitiveIndex) {
    // Resolve semantic streams once, rejecting features that would otherwise disappear during static conversion.
    Require(primitive.type == cgltf_primitive_type_triangles, "only triangle-list mesh primitives are supported");
    const cgltf_accessor *position = nullptr;
    const cgltf_accessor *normal = nullptr;
    const cgltf_accessor *uv = nullptr;
    const cgltf_accessor *tangent = nullptr;
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const auto &attribute = primitive.attributes[i];
        switch (attribute.type) {
        case cgltf_attribute_type_position:
            position = attribute.data;
            break;
        case cgltf_attribute_type_normal:
            normal = attribute.data;
            break;
        case cgltf_attribute_type_tangent:
            tangent = attribute.data;
            break;
        case cgltf_attribute_type_texcoord:
            if (attribute.index == 0)
                uv = attribute.data;
            break;
        case cgltf_attribute_type_color:
            throw std::runtime_error(
                "glTF import: vertex color attributes are unsupported; bake vertex colors into a base-color texture");
        case cgltf_attribute_type_joints:
        case cgltf_attribute_type_weights:
            throw std::runtime_error("skinned vertex attributes are unsupported");
        default:
            break;
        }
    }
    // All streams must address the same vertices; textured primitives cannot silently fall back to zero UVs.
    Require(position != nullptr && position->count > 0, "mesh primitive has no POSITION vertices");
    for (const auto *optional : {normal, uv, tangent})
        Require(!optional || optional->count == position->count, "mesh attribute counts do not match POSITION");
    const auto positions = Floats(position, cgltf_type_vec3, "POSITION");
    const auto normals = Floats(normal, cgltf_type_vec3, "NORMAL");
    const auto coordinates = Floats(uv, cgltf_type_vec2, "TEXCOORD_0");
    const auto tangents = Floats(tangent, cgltf_type_vec4, "TANGENT");
    const auto material = primitive.material ? static_cast<std::uint32_t>(primitive.material - context.data.materials)
                                             : static_cast<std::uint32_t>(context.data.materials_count);
    Require(material < context.scene.materials.size(), "mesh material index is outside the material array");
    Require(uv || !HasTextures(context.scene.materials[material]), "textured primitive is missing TEXCOORD_0");
    // Dense validated indices share one path with synthetic indices for nonindexed triangle lists.
    const std::size_t indexCount = primitive.indices ? primitive.indices->count : position->count;
    Require(indexCount > 0 && indexCount % 3 == 0, "triangle-list index or vertex count is not divisible by three");
    std::vector<std::uint32_t> indices(indexCount);
    if (primitive.indices) {
        Require(primitive.indices->type == cgltf_type_scalar, "index accessor must be SCALAR");
        Require(!primitive.indices->is_sparse,
                "sparse index accessors are unsupported; export a dense triangle index buffer");
        Require(cgltf_accessor_unpack_indices(primitive.indices, indices.data(), sizeof(std::uint32_t),
                                              indices.size()) == indices.size(),
                "index accessor cannot be unpacked");
    } else {
        for (std::size_t i = 0; i < indices.size(); ++i) {
            Require(i <= std::numeric_limits<std::uint32_t>::max(), "vertex count exceeds 32-bit indices");
            indices[i] = static_cast<std::uint32_t>(i);
        }
    }
    for (const auto index : indices)
        Require(index < position->count, "mesh index is outside POSITION accessor");
    Require(context.scene.triangles.size() + indexCount / 3 <= 0x00ffffffu,
            "triangle attribute indices exceed exact float storage capacity");
    // Source triangle order remains stable; the renderer builds reordered BVH copies after ECS extraction.
    const auto first = static_cast<std::uint32_t>(context.scene.triangles.size());
    const Matrix matrix(node);
    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
        std::array<std::uint32_t, 3> ids{indices[triangle], indices[triangle + 1], indices[triangle + 2]};
        // Mirrored nodes reverse primitive winding while inverse-transpose normals retain authored fronts.
        if (matrix.determinant < 0)
            std::swap(ids[1], ids[2]);
        std::array<Vec3, 3> p;
        std::array<Vec3, 3> n;
        std::array<std::array<float, 2>, 3> tex{};
        for (std::size_t v = 0; v < 3; ++v) {
            p[v] = matrix.Point(Vector(positions, ids[v]));
            if (uv)
                tex[v] = {coordinates[ids[v] * 2], coordinates[ids[v] * 2 + 1]};
        }
        const Vec3 geometric = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
        for (std::size_t v = 0; v < 3; ++v) {
            n[v] = normal ? matrix.Normal(Vector(normals, ids[v])) : geometric;
            Require(Length(n[v]) > 0, "mesh contains a zero-length normal or degenerate unnormaled triangle");
        }
        // The GPU stores one tangent frame per triangle, projected against the average smooth normal.
        Vec3 averageNormal = Normalize(n[0] + n[1] + n[2]);
        if (Length(averageNormal) == 0)
            averageNormal = Length(geometric) > 0 ? geometric : n[0];
        Vec3 t{};
        float handedness = 1;
        if (tangent) {
            for (std::size_t v = 0; v < 3; ++v)
                t += matrix.Direction(Vector(tangents, ids[v], 4));
            handedness = tangents[ids[0] * 4 + 3] * (matrix.determinant < 0 ? -1.0f : 1.0f);
            Require(std::abs(std::abs(handedness) - 1) < 0.001f, "tangent handedness must be -1 or +1");
        } else {
            const float du1 = tex[1][0] - tex[0][0], dv1 = tex[1][1] - tex[0][1], du2 = tex[2][0] - tex[0][0],
                        dv2 = tex[2][1] - tex[0][1];
            const double determinant = double(du1) * dv2 - double(du2) * dv1;
            if (std::abs(determinant) > 1e-12) {
                t = (p[1] - p[0]) * static_cast<float>(dv2 / determinant) -
                    (p[2] - p[0]) * static_cast<float>(dv1 / determinant);
                const Vec3 bitangent = (p[2] - p[0]) * static_cast<float>(du1 / determinant) -
                                       (p[1] - p[0]) * static_cast<float>(du2 / determinant);
                handedness = Dot(Cross(averageNormal, t), bitangent) < 0 ? -1.0f : 1.0f;
            }
        }
        t = Normalize(t - averageNormal * Dot(t, averageNormal));
        if (Length(t) == 0)
            t = Normalize(Cross(std::abs(averageNormal.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, averageNormal));
        // Indirection keeps attributes attached to their source triangle across later BVH permutations.
        GpuTriangleAttributes attributes{
            Lane(n[0], handedness),       Lane(n[1]),         Lane(n[2]), {tex[0][0], tex[0][1], tex[1][0], tex[1][1]},
            {tex[2][0], tex[2][1], 0, 0}, Lane(t, handedness)};
        const auto attributeIndex = context.scene.attributes.size();
        context.scene.attributes.push_back(attributes);
        context.scene.triangles.push_back(
            {Lane(p[0], static_cast<float>(material)), Lane(p[1], static_cast<float>(attributeIndex + 1)), Lane(p[2])});
    }
    const auto partName =
        Name(node.name, "Node") + "/" + Name(node.mesh->name, "Mesh") + "/" + std::to_string(primitiveIndex);
    context.scene.meshParts.push_back({partName, first, static_cast<std::uint32_t>(indexCount / 3), material});
}
template <class Function> void SelectedNodes(ImportContext &context, Function function) {
    // Honor the selected scene and source traversal order while detecting repeated or cyclic node ownership.
    const auto *scene = context.data.scene          ? context.data.scene
                        : context.data.scenes_count ? context.data.scenes
                                                    : nullptr;
    std::vector<const cgltf_node *> pending;
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            pending.push_back(scene->nodes[scene->nodes_count - 1 - i]);
    } else {
        for (cgltf_size i = context.data.nodes_count; i > 0; --i)
            if (!context.data.nodes[i - 1].parent)
                pending.push_back(context.data.nodes + i - 1);
    }
    std::set<const cgltf_node *> visited;
    while (!pending.empty()) {
        const auto *node = pending.back();
        pending.pop_back();
        Require(visited.insert(node).second, "selected scene contains a node more than once");
        function(*node);
        for (cgltf_size i = node->children_count; i > 0; --i)
            pending.push_back(node->children[i - 1]);
    }
}
} // namespace
void ImportGeometry(ImportContext &context) {
    // Each mesh instance is baked independently while primitive names retain node identity for the ECS outliner.
    SelectedNodes(context, [&](const cgltf_node &node) {
        if (node.mesh)
            for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
                Primitive(context, node, node.mesh->primitives[i], i);
    });
    Require(!context.scene.triangles.empty(), "selected glTF scene contains no supported mesh triangles");
}
void ImportLights(ImportContext &context) {
    // Punctual emitters inherit position and direction, but node scale must not scale radiometric intensity.
    SelectedNodes(context, [&](const cgltf_node &node) {
        if (!node.light)
            return;
        Require(context.scene.lights.size() < 16, "selected scene exceeds the limit of 16 analytic lights");
        const Matrix matrix(node);
        const auto &light = *node.light;
        GpuLight result;
        const Vec3 position = matrix.Point({});
        const Vec3 direction = Normalize(matrix.Direction({0, 0, -1}));
        // Ancestor scale followed by child rotation can shear axes; punctual-light frames must remain orthonormal.
        const Vec3 authoredTangent = matrix.Direction({1, 0, 0});
        const Vec3 tangent = Normalize(authoredTangent - direction * Dot(authoredTangent, direction));
        const Vec3 bitangent = Normalize(Cross(direction, tangent));
        unsigned type{};
        switch (light.type) {
        case cgltf_light_type_directional:
            type = 0;
            break;
        case cgltf_light_type_point:
            type = 1;
            break;
        case cgltf_light_type_spot:
            type = 4;
            break;
        default:
            throw std::runtime_error("unsupported punctual light type");
        }
        Require(light.intensity >= 0 && light.range >= 0, "punctual light intensity and range must be nonnegative");
        result.positionType = Lane(position, static_cast<float>(type));
        result.directionRange = Lane(direction, Finite(light.range, "light range"));
        result.colorIntensity = {Finite(light.color[0], "light color"), Finite(light.color[1], "light color"),
                                 Finite(light.color[2], "light color"), Finite(light.intensity, "light intensity")};
        Require(light.color[0] >= 0 && light.color[1] >= 0 && light.color[2] >= 0,
                "punctual light color must be nonnegative");
        result.tangentWidth = Lane(tangent);
        result.bitangentHeight = Lane(bitangent);
        if (type == 4)
            Require(light.spot_inner_cone_angle >= 0 && light.spot_outer_cone_angle > 0 &&
                        light.spot_outer_cone_angle <= 1.5707963268f &&
                        light.spot_inner_cone_angle <= light.spot_outer_cone_angle,
                    "spot cone angles are invalid");
        result.parameters = {0, Finite(std::cos(light.spot_outer_cone_angle), "spot outer angle"),
                             Finite(std::cos(light.spot_inner_cone_angle), "spot inner angle"), 0};
        context.scene.lights.push_back(result);
        context.scene.lightNames.push_back(Name(node.name, "Node") + "/" + Name(light.name, "Light"));
    });
}
} // namespace render::gltf
