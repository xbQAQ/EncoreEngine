#include "editor/EditorNavigation.h"
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

bool Near(float a, float b, float epsilon = 0.0003f) { return std::abs(a - b) < epsilon; }
bool Near(render::Vec3 a, render::Vec3 b, float epsilon = 0.0003f) {
    return Near(a.x, b.x, epsilon) && Near(a.y, b.y, epsilon) && Near(a.z, b.z, epsilon);
}
render::Vec3 Position(render::Float4 point) { return {point.x, point.y, point.z}; }

render::Scene OffCenterMesh() {
    render::Scene source;
    source.materials.resize(1);
    // Every source-box extremum is represented while the pivot is deliberately away from the origin.
    source.triangles = {{{2, -1, 1, 0}, {6, -1, 1, 0}, {6, 3, 5, 0}},
                        {{2, -1, 1, 0}, {6, 3, 5, 0}, {2, 3, 5, 0}}};
    source.meshParts = {{"Offset mesh", 0, 2, 0}};
    return source;
}

gameplay::Entity MeshEntity(gameplay::World& world) {
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each()) {
        (void)mesh;
        return entity;
    }
    return entt::null;
}

void FlyUsesCameraBasisAndWorldVertical() {
    render::editor::EditorNavigation navigation;
    gameplay::Camera camera;
    gameplay::Transform pose{{3, 4, 5}, {20, 35, 47}, {-2, 3, 4}};
    const auto basis = render::MakeCameraBasis(20, 35, 47);
    render::editor::NavigationInput input;
    input.mode = render::editor::NavigationMode::Fly;
    input.deltaSeconds = 0.1f;
    input.forward = 1;
    Check(navigation.Update(pose, camera, input), "RMB W moves the camera");
    Check(Near(pose.position, render::Vec3{3, 4, 5} + basis.forward * 0.2f), "fly follows +Z-forward rolled camera basis");
    const auto before = pose.position;
    input.forward = 0; input.up = 1;
    navigation.Update(pose, camera, input);
    Check(Near(pose.position, before + render::Vec3{0, 0.2f, 0}), "E moves along world up despite roll and pitch");
    input.up = 0; input.right = -1;
    const auto beforeLeft = pose.position;
    navigation.Update(pose, camera, input);
    Check(Near(pose.position, beforeLeft - basis.right * 0.2f), "A follows the camera right axis with a negative sign");
    Check(Near(pose.scale, {-2, 3, 4}) && Near(pose.rotationDegrees, {20, 35, 47}), "translation preserves authored roll and scale");
}

void FlyLookClampsAndSpeedPersists() {
    render::editor::EditorNavigation navigation;
    gameplay::Camera camera;
    gameplay::Transform pose{{}, {0, 0, 31}};
    render::editor::NavigationInput input;
    input.mode = render::editor::NavigationMode::Fly;
    input.mouseDeltaX = 40; input.mouseDeltaY = 20;
    Check(navigation.Update(pose, camera, input), "RMB drag looks around without requiring keyboard movement");
    Check(pose.rotationDegrees.x > 0 && pose.rotationDegrees.y > 0 && Near(pose.rotationDegrees.z, 31), "mouse look uses screen deltas and keeps roll");
    input.mouseDeltaX = 0; input.mouseDeltaY = 0; input.wheel = 4;
    const auto beforeSpeed = pose.position;
    Check(!navigation.Update(pose, camera, input), "speed-only wheel does not invalidate the rendered camera pose");
    Check(navigation.Speed() > 2 && Near(pose.position, beforeSpeed), "RMB wheel adjusts persistent speed without dolly");
    const float speed = navigation.Speed();
    input.wheel = 0; input.forward = 1; input.deltaSeconds = 0.05f;
    navigation.Update(pose, camera, input);
    Check(Near(render::Length(pose.position - beforeSpeed), speed * 0.05f), "later fly movement consumes the retained wheel speed");
    input.forward = 0; input.mouseDeltaY = 1e30f;
    navigation.Update(pose, camera, input);
    Check(pose.rotationDegrees.x < 90 && pose.rotationDegrees.x > 80, "pitch cannot cross the vertical pole");
    input.mouseDeltaY = 0; input.wheel = 1e30f;
    for (int repeat = 0; repeat < 100; ++repeat) navigation.Update(pose, camera, input);
    Check(std::isfinite(navigation.Speed()) && navigation.Speed() <= 500, "speed has a finite practical upper limit");
    input.wheel = -1e30f;
    for (int repeat = 0; repeat < 100; ++repeat) navigation.Update(pose, camera, input);
    Check(navigation.Speed() >= 0.05f, "speed has a nonzero lower limit");
}

void PanMatchesRolledImageAndFocusDepth() {
    render::editor::EditorNavigation navigation;
    gameplay::Camera camera;
    camera.verticalFovDegrees = 60;
    gameplay::Transform pose{{1, 2, 3}, {17, -28, 52}};
    const auto basis = render::MakeCameraBasis(17, -28, 52);
    render::editor::NavigationInput input;
    input.mode = render::editor::NavigationMode::Pan;
    input.viewportHeight = 500;
    input.mouseDeltaX = 25; input.mouseDeltaY = -15;
    const float worldPerPixel = 2 * navigation.FocusDistance() * std::tan(3.141592654f / 6) / input.viewportHeight;
    Check(navigation.Update(pose, camera, input), "MMB pan moves the camera");
    const auto expected = (-basis.right * 25 - basis.up * 15) * worldPerPixel;
    Check(Near(pose.position, render::Vec3{1, 2, 3} + expected), "pan follows rolled image axes with perspective depth scaling");
    Check(Near(navigation.Pivot(), render::Vec3{1, 2, 3} + basis.forward * 5 + expected), "pan translates retained pivot with the camera");
    Check(Near(pose.rotationDegrees, {17, -28, 52}), "pan cannot change camera orientation");
}

void FocusFramesTransformedMeshAtNarrowAspect() {
    auto source = OffCenterMesh();
    gameplay::World world;
    world.CreateScene(source);
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    const auto mesh = MeshEntity(world);
    world.Registry().get<gameplay::Transform>(mesh) = {{8, -3, 4}, {32, 67, -123}, {-2, 0.75f, 3}};
    gameplay::Transform pose{{0, 0, -6}, {11, -27, 37}};
    gameplay::Camera camera;
    camera.verticalFovDegrees = 52;
    camera.farPlane = 1000;
    constexpr float aspect = 0.45f;
    render::editor::EditorNavigation navigation;
    Check(navigation.Focus(pose, camera, source, world, mesh, aspect), "F frames a visible selected mesh");
    Check(Near(pose.rotationDegrees, {11, -27, 37}), "focus preserves current view direction and roll");
    const auto extracted = render::ExtractRenderScene(source, world.ExtractRenderSnapshot());
    const auto basis = render::MakeCameraBasis(11, -27, 37);
    const float tangent = std::tan(camera.verticalFovDegrees * 3.141592654f / 360);
    render::Aabb bounds;
    // Independent renderer extraction checks signed scaling and combined rotation, rather than duplicating navigation transforms.
    for (const auto& triangle : extracted.triangles)
        for (const auto& vertex : {triangle.a, triangle.b, triangle.c}) {
            const auto point = Position(vertex);
            bounds.Expand(point);
            const auto offset = point - pose.position;
            const float depth = render::Dot(offset, basis.forward);
            Check(depth > camera.nearPlane && std::abs(render::Dot(offset, basis.right)) < depth * tangent * aspect &&
                std::abs(render::Dot(offset, basis.up)) < depth * tangent, "every transformed vertex fits both perspective axes and near plane");
        }
    Check(Near(navigation.Pivot(), bounds.minimum * 0.5f + bounds.maximum * 0.5f), "focus pivot is transformed visible bounds center");
    Check(Near(render::Length(navigation.Pivot() - pose.position), navigation.FocusDistance()), "focus distance matches the retained world pivot");
}

void OrbitKeepsFocusedPivotAndRoll() {
    auto source = OffCenterMesh();
    gameplay::World world;
    world.CreateScene(source);
    const auto mesh = MeshEntity(world);
    gameplay::Camera camera;
    gameplay::Transform pose{{0, 0, -3}, {12, 25, 41}};
    render::editor::EditorNavigation navigation;
    Check(navigation.Focus(pose, camera, source, world, mesh, 1.7f), "orbit setup acquires selected target");
    const auto pivot = navigation.Pivot();
    const float distance = navigation.FocusDistance();
    render::editor::NavigationInput input;
    input.mode = render::editor::NavigationMode::Orbit;
    input.mouseDeltaX = 70; input.mouseDeltaY = -30;
    Check(navigation.Update(pose, camera, input), "Alt LMB rotates around focus");
    const auto basis = render::MakeCameraBasis(pose.rotationDegrees.x, pose.rotationDegrees.y, pose.rotationDegrees.z);
    Check(Near(navigation.Pivot(), pivot) && Near(render::Length(pivot - pose.position), distance), "orbit keeps target and camera distance fixed");
    Check(render::Dot(render::Normalize(pivot - pose.position), basis.forward) > 0.99999f && Near(pose.rotationDegrees.z, 41), "orbit aims at focus and preserves authored roll");
}

void DollyAndWheelStayOutsideFocus() {
    render::editor::EditorNavigation navigation;
    gameplay::Camera camera;
    gameplay::Transform pose;
    render::editor::NavigationInput input;
    input.wheel = 1;
    Check(navigation.Update(pose, camera, input), "plain wheel dollies forward");
    Check(pose.position.z > 0 && navigation.FocusDistance() < 5, "positive wheel approaches default focus");
    const auto pivot = navigation.Pivot();
    const auto beforeDrag = pose.position;
    input.wheel = 0; input.mode = render::editor::NavigationMode::Dolly; input.mouseDeltaX = 15;
    Check(navigation.Update(pose, camera, input) && pose.position.z > beforeDrag.z, "Alt RMB horizontal drag dollies toward focus");
    input.mouseDeltaX = 1e30f;
    for (int repeat = 0; repeat < 100; ++repeat) navigation.Update(pose, camera, input);
    Check(navigation.FocusDistance() >= camera.nearPlane * 2 && navigation.FocusDistance() < 1 &&
        Near(navigation.Pivot(), pivot), "dolly approaches a near-safe minimum without passing the retained pivot");
    input.mouseDeltaX = -1e30f;
    for (int repeat = 0; repeat < 100; ++repeat) navigation.Update(pose, camera, input);
    Check(render::IsFinite(pose.position) && navigation.FocusDistance() <= 1e6f, "extreme reverse dolly remains bounded and finite");
    Check(Near(camera.verticalFovDegrees, 43), "navigation never edits lens field of view");
}

void FocusUsesLightEmitterExtent() {
    gameplay::World world;
    const auto entity = world.CreateEntity("Emitter");
    auto& light = world.Registry().emplace<gameplay::Light>(entity);
    light.type = gameplay::LightType::Rectangle; light.width = 8; light.height = 2;
    light.range = 10000;
    auto& transform = world.Registry().get<gameplay::Transform>(entity);
    transform.position = {7, 3, 9}; transform.rotationDegrees = {90, 0, 0}; transform.scale = {3, 4, 5};
    gameplay::Transform pose;
    gameplay::Camera camera;
    render::editor::EditorNavigation navigation;
    Check(navigation.Focus(pose, camera, {}, world, entity, 1), "F can frame an analytic area light without source mesh");
    Check(Near(navigation.Pivot(), transform.position) && navigation.FocusDistance() > 9 && navigation.FocusDistance() < 20,
        "focus uses rendered emitter width and height, ignoring non-rendered transform scale and influence range");
    light.type = gameplay::LightType::Point; light.sourceRadius = 2;
    const float areaDistance = navigation.FocusDistance();
    Check(navigation.Focus(pose, camera, {}, world, entity, 1) && navigation.FocusDistance() < areaDistance,
        "point-light focus uses source radius rather than illumination range");
}

void InvalidInputAndHiddenSelectionDoNotMutate() {
    auto source = OffCenterMesh();
    gameplay::World world;
    world.CreateScene(source);
    const auto entity = MeshEntity(world);
    gameplay::Transform pose{{1, 2, 3}, {5, 6, 7}};
    gameplay::Camera camera;
    render::editor::EditorNavigation navigation;
    const auto original = pose;
    world.Registry().get<gameplay::MeshRenderer>(entity).visible = false;
    Check(!navigation.Focus(pose, camera, source, world, entity, 1), "hidden mesh has no frameable visible bounds");
    Check(!navigation.Focus(pose, camera, source, world, entt::null, 1), "stale or missing selection is ignored");
    render::editor::NavigationInput input;
    input.mode = render::editor::NavigationMode::Fly; input.forward = 1; input.deltaSeconds = 0.1f;
    input.mouseDeltaX = std::numeric_limits<float>::quiet_NaN();
    Check(!navigation.Update(pose, camera, input) && Near(pose.position, original.position) &&
        Near(pose.rotationDegrees, original.rotationDegrees), "nonfinite input is rejected atomically");
    input.mouseDeltaX = 0; input.viewportHeight = 0;
    Check(!navigation.Update(pose, camera, input), "zero-size image cannot drive navigation");
    world.Registry().get<gameplay::MeshRenderer>(entity).visible = true;
    camera.verticalFovDegrees = 180;
    Check(!navigation.Focus(pose, camera, source, world, entity, 1), "invalid projection is rejected without camera mutation");
    Check(Near(pose.position, original.position) && Near(pose.rotationDegrees, original.rotationDegrees), "invalid operations leave camera unchanged");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"fly basis and world vertical", FlyUsesCameraBasisAndWorldVertical},
        {"mouse look and persistent bounded speed", FlyLookClampsAndSpeedPersists},
        {"perspective and rolled-camera pan", PanMatchesRolledImageAndFocusDepth},
        {"transformed mesh focus in narrow viewport", FocusFramesTransformedMeshAtNarrowAspect},
        {"orbit target and roll", OrbitKeepsFocusedPivotAndRoll},
        {"dolly wheel and distance bounds", DollyAndWheelStayOutsideFocus},
        {"analytic light emitter focus", FocusUsesLightEmitterExtent},
        {"invalid and hidden input", InvalidInputAndHiddenSelectionDoNotMutate},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
