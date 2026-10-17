#include "editor/EditorGizmo.h"
#include "gameplay/World.h"
#include "render/SceneExtraction.h"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Near(float a, float b, float epsilon = 0.0002f) { return std::abs(a - b) < epsilon; }
bool Near(render::Vec3 a, render::Vec3 b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }
render::Vec3 Position(render::Float4 value) { return {value.x, value.y, value.z}; }

render::Scene TwoTriangles() {
    render::Scene scene;
    scene.materials.resize(1);
    // Facing -Z makes both opaque layers visible to the production camera looking along +Z.
    scene.triangles = {{{-1, -1, 3, 0}, {0, 1, 3, 0}, {1, -1, 3, 0}},
                       {{-1, -1, 6, 0}, {0, 1, 6, 0}, {1, -1, 6, 0}}};
    scene.meshParts = {{"Front", 0, 1, 0}, {"Back", 1, 1, 0}};
    return scene;
}

void PoseAgreesWithExtraction() {
    const auto source = TwoTriangles();
    gameplay::World world;
    world.CreateScene(source);
    for (auto [entity, transform, mesh] : world.Registry().view<gameplay::Transform, gameplay::MeshRenderer>().each()) {
        (void)entity;
        // Combined rotations and signed nonuniform scaling must agree with the engine, not toolkit Euler helpers.
        transform = {{2, -3, 4}, {32, 67, -123}, {-2, 0.75f, 3}};
        const auto pivot = render::editor::MeshPivot(source, mesh);
        const auto matrix = render::editor::GizmoPose(transform, pivot);
        const auto roundtrip = render::editor::GizmoTransform(matrix, pivot, transform);
        Check(roundtrip.has_value(), "finite signed pose decomposes");
        const auto before = render::ExtractRenderScene(source, world.ExtractRenderSnapshot());
        transform = *roundtrip;
        const auto after = render::ExtractRenderScene(source, world.ExtractRenderSnapshot());
        for (std::size_t i = 0; i < before.triangles.size(); ++i) {
            Check(Near(Position(before.triangles[i].a), Position(after.triangles[i].a)) &&
                  Near(Position(before.triangles[i].b), Position(after.triangles[i].b)) &&
                  Near(Position(before.triangles[i].c), Position(after.triangles[i].c)), "matrix roundtrip preserves extracted world geometry");
        }
        Check(Near(transform.scale, {-2, 0.75f, 3}), "signed scale is preserved");
    }
}

void PivotRemainsFixedDuringRotationAndScale() {
    const render::Vec3 pivot{4, 2, 7};
    gameplay::Transform original{{2, 3, -1}, {}, {1, 1, 1}};
    auto manipulated = render::editor::GizmoPose({{}, {35, 90, -23}, {2, 3, 4}});
    const auto center = pivot + original.position;
    manipulated[12] = center.x; manipulated[13] = center.y; manipulated[14] = center.z;
    // Holding the manipulated matrix translation must keep the visual center fixed while compensating ECS origin.
    const auto result = render::editor::GizmoTransform(manipulated, pivot, original);
    Check(result.has_value(), "gimbal-lock pose remains representable");
    const auto recomposed = render::editor::GizmoPose(*result, pivot);
    for (std::size_t i = 0; i < manipulated.size(); ++i)
        Check(Near(recomposed[i], manipulated[i], 0.0005f), "pivot compensation preserves manipulated affine transform at yaw ninety");
    auto singular = manipulated;
    singular[0] = singular[1] = singular[2] = 0;
    Check(!render::editor::GizmoTransform(singular, pivot, original), "singular scale rejected without mutating ECS");
    auto shear = render::editor::GizmoPose(original);
    shear[4] = 0.5f;
    Check(!render::editor::GizmoTransform(shear, pivot, original), "unsupported shear cannot silently deform ECS pose");
    manipulated[12] = std::numeric_limits<float>::infinity();
    Check(!render::editor::GizmoTransform(manipulated, pivot, original), "nonfinite toolkit output rejected");
}

void ProjectionAndLetterboxRayAgree() {
    const render::editor::ViewportRect image{130, 240, 800, 400};
    gameplay::Transform transform{{4, 2, -7}, {23, -34, 41}, {1, 1, 1}};
    gameplay::Camera camera;
    camera.verticalFovDegrees = 60;
    const auto view = render::editor::MakeGizmoView(transform, camera, image.width / image.height);
    const auto ray = render::editor::ViewportRay(image, 730, 340, transform, camera);
    Check(ray.has_value(), "point inside letterboxed image yields a ray");
    const auto point = ray->origin + ray->direction * 9;
    const std::array<float, 4> world{point.x, point.y, point.z, 1};
    std::array<float, 4> eye{}, clip{};
    // Project the picked ray through the actual matrix API rather than comparing duplicate basis formulas.
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column) eye[row] += view.view[column * 4 + row] * world[column];
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column) clip[row] += view.projection[column * 4 + row] * eye[column];
    Check(Near(image.x + (clip[0] / clip[3] + 1) * image.width * 0.5f, 730, 0.001f) &&
          Near(image.y + (1 - clip[1] / clip[3]) * image.height * 0.5f, 340, 0.001f), "rolled camera projection matches viewport pick coordinates");
    Check(!render::editor::ViewportRay(image, 129, 340, transform, camera) &&
          !render::editor::ViewportRay(image, 930, 340, transform, camera), "letterbox and exclusive far edge cannot pick");
    Check(ray->tMax > ray->tMin && Near(render::Length(ray->direction), 1), "ray distances remain finite clip distances in world meters");
}

void PickingUsesNearestVisibleEntityAndLightOwner() {
    auto source = TwoTriangles();
    gameplay::World world;
    world.CreateScene(source);
    gameplay::Entity front{entt::null}, back{entt::null};
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        (mesh.firstTriangle == 0 ? front : back) = entity;
    const render::Ray ray{{0, 0, 0}, {0, 0, 1}};
    Check(render::editor::PickEntity(source, world, ray) == front, "nearest triangle selects its owning entity");
    world.Registry().get<gameplay::MeshRenderer>(front).visible = false;
    Check(render::editor::PickEntity(source, world, ray) == back, "hidden front mesh does not mask visible entity");
    world.Registry().get<gameplay::Transform>(back).position.x = 4;
    Check(render::editor::PickEntity(source, world, ray) == entt::null, "pick follows current ECS transforms");
    auto& light = world.Registry().get<gameplay::Light>(world.LightEntity());
    light.type = gameplay::LightType::Rectangle;
    light.width = light.height = 2;
    auto& pose = world.Registry().get<gameplay::Transform>(world.LightEntity());
    pose.position = {0, 0, 2}; pose.rotationDegrees = {90, 0, 0};
    Check(render::editor::PickEntity(source, world, ray) == world.LightEntity(), "generated emitter triangles select the analytic light");

    source = render::BuildCornellScene();
    world.CreateCornell(source);
    // The ceiling proxy must manipulate the light pose rather than an ignored proxy transform.
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        if (mesh.lightOwned) Check(render::editor::GizmoTarget(world, entity) == world.LightEntity(), "source emitter proxy resolves to its owner");
    const auto removed = world.LightEntity();
    world.DestroyEntity(removed);
    Check(render::editor::GizmoTarget(world, removed) == entt::null, "destroyed or stale entity handle is ignored");
}

void PickingMatchesMaterialSurfaceAcceptance() {
    auto source = TwoTriangles();
    source.materials.resize(2);
    source.triangles[1].a.w = 1;
    source.meshParts[1].materialIndex = 1;
    gameplay::World world;
    world.CreateScene(source);
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    gameplay::Entity front{entt::null}, back{entt::null};
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        (mesh.firstTriangle == 0 ? front : back) = entity;
    const render::Ray ray{{0, 0, 0}, {0, 0, 1}};
    // A 180-degree Y rotation reverses the front surface, while translation keeps it at the same depth.
    auto& pose = world.Registry().get<gameplay::Transform>(front);
    pose.rotationDegrees.y = 180;
    pose.position.z = 6;
    Check(render::editor::PickEntity(source, world, ray) == back, "single-sided opaque backface must not hide a visible entity");
    auto& material = world.Materials()[0];
    material.flags.y = 1;
    Check(render::editor::PickEntity(source, world, ray) == front, "double-sided backface remains selectable");
    material.flags.y = 0;
    material.transmissionIor.x = 0.25f;
    Check(render::editor::PickEntity(source, world, ray) == front, "transmissive backface follows SceneQueries acceptance");
    pose = {};
    material.transmissionIor.x = 0;
    material.flags.z = 1;
    material.extras.w = 0.25f;
    Check(render::editor::PickEntity(source, world, ray) == back, "masked-out opacity must reveal the next accepted entity");
    material.extras.w = material.extras.z;
    Check(render::editor::PickEntity(source, world, ray) == front, "alpha equal to cutoff is accepted");
    material.flags.z = 0;
    material.extras.w = 0;
    Check(render::editor::PickEntity(source, world, ray) == front, "opaque material ignores its unused alpha factor");

    // A rejected nearest layer must not discard an accepted coplanar candidate by advancing the ray's minimum distance.
    material.flags.z = 1;
    world.Registry().get<gameplay::Transform>(back).position.z = -3;
    Check(render::editor::PickEntity(source, world, ray) == back, "alpha rejection still considers a coplanar accepted primitive");
}

void PickingMatchesTransformedFilteredTextureAlpha() {
    auto source = TwoTriangles();
    source.materials.resize(2);
    source.triangles[0].b.w = 1;
    source.triangles[1].a.w = 1;
    source.meshParts[1].materialIndex = 1;
    source.materials[0].flags.z = 1;
    source.materials[0].textures.x = 0;
    // At the center ray the barycentric UV is (.25, .5); the two-column texture has transparent left and opaque right texels.
    source.attributes.push_back({{0, 0, -1, 1}, {0, 0, -1, 0}, {0, 0, -1, 0},
        {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 0, 0, 1}});
    source.packedTexels = {0x00ffffffu, 0xffffffffu, 0x00ffffffu, 0xffffffffu};
    source.textureViews.push_back({{0, 2, 2, 0}});
    gameplay::World world;
    world.CreateScene(source);
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    gameplay::Entity front{entt::null}, back{entt::null};
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        (mesh.firstTriangle == 0 ? front : back) = entity;
    const render::Ray ray{{0, 0, 0}, {0, 0, 1}};
    auto& view = source.textureViews[0];
    auto& material = world.Materials()[0];
    auto expect = [&](bool covered, const char* message) {
        Check(render::editor::PickEntity(source, world, ray) == (covered ? front : back), message);
    };
    expect(false, "interpolated base-texture alpha hole reveals the rear entity");
    view.offsetScale.x = 0.25f;
    expect(true, "bilinear half coverage equal to alpha cutoff is accepted");
    material.extras.w = 0.5f;
    expect(false, "base texture alpha multiplies the material opacity factor");
    material.extras.w = 1;
    view.offsetScale.x = 1;
    expect(false, "repeat wrap maps an offset UV back into the transparent texel");
    view.image.w = 1;
    expect(true, "clamp wrap retains the opaque edge beyond one");
    view.image.w = 2;
    expect(true, "mirrored repeat selects the reflected opaque texel");
    view.offsetScale.x = -0.5f;
    view.image.w = 0;
    expect(true, "negative repeat UV wraps into the opaque texel");
    view.image.w = 2;
    expect(false, "negative mirrored UV reflects into the transparent texel");
    view.offsetScale = {0.25f, 0, 2, 1};
    view.image.w = 1;
    expect(true, "texture transform scales before applying its UV offset");
    view.offsetScale = {0.75f, 0, 1, 1};
    view.rotation = {1, 0, 0, 0};
    expect(false, "texture rotation mixes the interpolated V coordinate into alpha sampling");

    // Independent V wrapping and four-tap filtering must use image height and the high sampler-mode bits.
    source.packedTexels = {0x00ffffffu, 0x00ffffffu, 0xffffffffu, 0xffffffffu};
    view.rotation = {0, 1, 0, 0};
    view.offsetScale = {0, 0.75f, 1, 1};
    view.image.w = 0;
    expect(false, "repeat V wraps to the transparent image row");
    view.image.w = 1u << 2;
    expect(true, "clamp V retains the opaque bottom row");
    view.image.w = 2u << 2;
    expect(true, "mirrored V uses the reflected image row");
}

void PickingSharedResourceCloneUsesItsOwnOwnerAndMirroredUv() {
    auto source = TwoTriangles();
    source.triangles[0].b.w = 1;
    source.attributes.push_back({{0, 0, -1, 1}, {0, 0, -1, 0}, {0, 0, -1, 0},
        {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 0, 0, 1}});
    source.packedTexels = {0x00ffffffu, 0xffffffffu};
    render::GpuTextureView texture;
    texture.image = {0, 2, 1, 0};
    source.textureViews.push_back(texture);
    gameplay::World world;
    world.CreateScene(source);
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    gameplay::Entity original{entt::null};
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        if (mesh.firstTriangle == 0) original = entity;
    const auto mesh = world.Registry().get<gameplay::MeshRenderer>(original);
    const auto clone = world.CreateEntity("Visible source clone");
    world.Registry().emplace<gameplay::MeshRenderer>(clone, mesh);
    world.Registry().get<gameplay::Transform>(clone) = {{4, 0, 0}, {}, {-1, 1, 1}};
    const render::Ray ray{{4, 0, 0}, {0, 0, 1}};
    Check(render::editor::PickEntity(source, world, ray) == clone, "appended clone triangles select their precise entity instead of source owner");

    // Mirrored winding must carry UV corner changes into the clone's private attribute record for alpha picking.
    auto& material = world.Materials()[0];
    material.flags.z = 1;
    material.textures.x = 0;
    material.extras.z = 0.4f;
    material.extras.w = 1;
    Check(render::editor::PickEntity(source, world, ray) == entt::null,
        "transparent texel in a mirrored clone does not create an invisible selectable surface");
    source.textureViews[0].offsetScale.x = 0.5f;
    Check(render::editor::PickEntity(source, world, ray) == clone,
        "opaque transformed texel restores the clone's exact entity identity");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"pose matches scene extraction", PoseAgreesWithExtraction},
        {"center pivot and invalid poses", PivotRemainsFixedDuringRotationAndScale},
        {"projection and letterbox picking", ProjectionAndLetterboxRayAgree},
        {"nearest visible entity and emitter owner", PickingUsesNearestVisibleEntityAndLightOwner},
        {"material surface acceptance", PickingMatchesMaterialSurfaceAcceptance},
        {"transformed filtered texture alpha", PickingMatchesTransformedFilteredTextureAlpha},
        {"shared resource clone owner and mirrored alpha", PickingSharedResourceCloneUsesItsOwnOwnerAndMirroredUv},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
