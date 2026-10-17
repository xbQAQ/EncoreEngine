#include "gameplay/World.h"
#include "render/Bvh.h"
#include "render/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }

template <typename Function> void Rejects(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}

void EntityLifecycle() {
    gameplay::World world;
    Check(world.Revision() == 0, "new world starts with revision zero");
    const auto entity = world.CreateEntity("Mover");
    auto& registry = world.Registry();
    Check(registry.valid(entity), "created entity is valid");
    Check(registry.get<gameplay::Name>(entity).value == "Mover", "entity name component");
    Check(registry.all_of<gameplay::Transform>(entity), "entities have a transform");
    const auto revision = world.Revision();
    Check(revision > 0, "creation changes revision");
    Check(world.DestroyEntity(entity), "valid entity destroyed");
    Check(!registry.valid(entity), "destroyed handle is invalid");
    Check(world.Revision() > revision, "destruction changes revision");
    const auto afterDestroy = world.Revision();
    Check(!world.DestroyEntity(entity) && world.Revision() == afterDestroy, "stale destroy is a no-op");
    const auto replacement = world.CreateEntity("Replacement");
    Check(replacement != entity && !registry.valid(entity), "recycled slot uses a new entity version");
    const auto beforeEdit = world.Revision();
    registry.get<gameplay::Name>(replacement).value = "Edited";
    world.MarkDirty();
    Check(world.Revision() > beforeEdit, "explicit inspector edit changes revision");
}

void CornellEntities() {
    auto scene = render::BuildCornellScene();
    render::Bvh{}.Build(scene.triangles);
    gameplay::World world;
    world.CreateCornell(scene);
    const auto cameraEntity = world.CameraEntity();
    const auto lightEntity = world.LightEntity();
    Check(cameraEntity != entt::null && lightEntity != entt::null, "Cornell camera and light exist");
    const auto& camera = world.Registry().get<gameplay::Camera>(cameraEntity);
    const auto& cameraTransform = world.Registry().get<gameplay::Transform>(cameraEntity);
    Check(Near(camera.verticalFovDegrees, 43) && camera.nearPlane > 0 && camera.farPlane > camera.nearPlane, "camera defaults are valid");
    Check(Near(cameraTransform.position.y, 1.4f) && Near(cameraTransform.position.z, -4.6f), "camera starts outside open room");
    const auto snapshot = world.ExtractRenderSnapshot();
    Check(snapshot.revision == world.Revision(), "snapshot records world revision");
    Check(snapshot.cameras.size() == 1 && snapshot.cameras.front().entity == cameraEntity, "active camera extracted");
    Check(snapshot.lights.size() == 1 && snapshot.lights.front().entity == lightEntity, "enabled area light extracted");
    const auto& light = snapshot.lights.front();
    Check(light.light.type == gameplay::LightType::Rectangle && Near(light.light.width, 0.9f) && Near(light.light.height, 0.9f), "area light shape");
    Check(Near(light.transform.position.y, 2.74f) && Near(light.light.intensity, 15), "area light location and intensity");
    // Reordered geometry can split source objects, but every uploaded triangle must belong to one entity.
    std::vector<unsigned> coverage(scene.triangles.size());
    for (const auto& instance : snapshot.instances) {
        Check(instance.mesh.triangleCount > 0, "mesh range must be nonempty");
        Check(instance.transform.scale.x == 1 && instance.transform.position.x == 0, "imported baked geometry uses identity transform");
        const auto end = instance.mesh.firstTriangle + instance.mesh.triangleCount;
        Check(end <= scene.triangles.size(), "mesh range fits uploaded scene");
        for (auto triangle = instance.mesh.firstTriangle; triangle < end; ++triangle) {
            ++coverage[triangle];
            Check(static_cast<std::uint32_t>(scene.triangles[triangle].a.w) == instance.mesh.materialIndex, "mesh material matches every triangle in its range");
        }
    }
    Check(!coverage.empty() && std::all_of(coverage.begin(), coverage.end(), [](unsigned count) { return count == 1; }), "mesh entities partition reordered geometry exactly once");
    const auto entityView = world.Registry().view<const gameplay::Name, const gameplay::Transform>();
    std::size_t entities = 0;
    for (const auto entity : entityView) { (void)entity; ++entities; }
    Check(entities == snapshot.instances.size() + 2, "EnTT component queries see meshes camera and light");
}

void VelocityIntegration() {
    gameplay::World world;
    const auto moving = world.CreateEntity("Moving");
    const auto stationary = world.CreateEntity("Stationary");
    const auto velocityOnly = world.Registry().create();
    world.Registry().emplace<gameplay::Velocity>(velocityOnly, render::Vec3{100, 100, 100});
    world.Registry().emplace<gameplay::Velocity>(moving, render::Vec3{2, -1, 0.5f});
    const auto revision = world.Revision();
    world.Update(0.25f);
    const auto position = world.Registry().get<gameplay::Transform>(moving).position;
    Check(Near(position.x, 0.5f) && Near(position.y, -0.25f) && Near(position.z, 0.125f), "velocity integrates position in meters");
    Check(world.Registry().get<gameplay::Transform>(stationary).position.x == 0, "entities without velocity stay fixed");
    Check(!world.Registry().all_of<gameplay::Transform>(velocityOnly), "system only visits matching component combinations");
    Check(world.Revision() > revision && world.Registry().valid(moving), "motion updates revision while handle stays stable");
    const auto afterMotion = world.Revision();
    world.Update(0);
    Check(world.Revision() == afterMotion, "zero timestep does not invalidate history");
    world.Registry().get<gameplay::Velocity>(moving).linear = {};
    world.Update(1);
    Check(world.Revision() == afterMotion, "zero velocity does not invalidate history");
}

void InvalidUpdateIsAtomic() {
    gameplay::World world;
    const auto first = world.CreateEntity("Valid mover");
    const auto second = world.CreateEntity("Invalid mover");
    // Whichever EnTT iteration order is chosen, one bad component must roll back the entire step.
    world.Registry().emplace<gameplay::Velocity>(first, render::Vec3{1, 0, 0});
    world.Registry().emplace<gameplay::Velocity>(second, render::Vec3{0, 0, std::numeric_limits<float>::quiet_NaN()});
    const auto revision = world.Revision();
    Rejects([&] { world.Update(-1); }, "negative timestep rejected");
    Rejects([&] { world.Update(std::numeric_limits<float>::infinity()); }, "nonfinite timestep rejected");
    Rejects([&] { world.Update(1); }, "nonfinite velocity rejected");
    Check(world.Registry().get<gameplay::Transform>(first).position.x == 0, "bad component cannot partially update another entity");
    Check(world.Revision() == revision, "rejected update preserves revision");
    world.Registry().get<gameplay::Velocity>(second).linear = {std::numeric_limits<float>::max(), 0, 0};
    Rejects([&] { world.Update(4); }, "position overflow rejected");
    Check(world.Registry().get<gameplay::Transform>(first).position.x == 0 && world.Revision() == revision,
          "overflow cannot partially update world");
}

void SnapshotIsolationAndFiltering() {
    gameplay::World world;
    const auto meshEntity = world.CreateEntity("Mesh");
    world.Registry().emplace<gameplay::MeshRenderer>(meshEntity, 2u, 4u, 1u, true);
    const auto cameraEntity = world.CreateEntity("Camera");
    world.Registry().emplace<gameplay::Camera>(cameraEntity);
    const auto lightEntity = world.CreateEntity("Light");
    world.Registry().emplace<gameplay::Light>(lightEntity);
    const auto first = world.ExtractRenderSnapshot();
    Check(first.instances.size() == 1 && first.cameras.size() == 1 && first.lights.size() == 1, "matching components extracted");
    world.Registry().get<gameplay::Transform>(meshEntity).position.x = 5;
    world.Registry().get<gameplay::MeshRenderer>(meshEntity).visible = false;
    world.Registry().get<gameplay::Camera>(cameraEntity).active = false;
    world.Registry().get<gameplay::Light>(lightEntity).enabled = false;
    world.MarkDirty();
    const auto second = world.ExtractRenderSnapshot();
    Check(second.instances.empty() && second.cameras.empty() && second.lights.empty(), "disabled render components filtered");
    Check(first.instances.front().transform.position.x == 0 && first.instances.front().mesh.visible, "snapshot owns component copies");
    Check(first.revision < second.revision, "snapshot revisions distinguish edits");
    Check(world.DestroyEntity(meshEntity), "mesh entity destruction succeeds");
    Check(world.ExtractRenderSnapshot().instances.empty(), "destroyed entity excluded from future snapshots");
}

void ImportValidationAndPrimaryHandles() {
    auto scene = render::BuildCornellScene();
    gameplay::World world;
    world.CreateCornell(scene);
    const auto originalCamera = world.CameraEntity();
    const auto originalLight = world.LightEntity();
    const auto revision = world.Revision();
    scene.triangles.front().a.w = static_cast<float>(scene.materials.size());
    Rejects([&] { world.CreateCornell(scene); }, "invalid scene material index rejected");
    Check(world.CameraEntity() == originalCamera && world.Revision() == revision, "invalid import preserves existing world");
    scene = render::BuildCornellScene();
    scene.triangles.front().a.x = std::numeric_limits<float>::quiet_NaN();
    Rejects([&] { world.CreateCornell(scene); }, "nonfinite imported vertex rejected");
    Check(world.Registry().valid(originalCamera), "invalid geometry preserves prior handles");
    Check(world.DestroyEntity(originalCamera) && world.CameraEntity() == entt::null, "primary camera handle is checked after deletion");
    Check(world.DestroyEntity(originalLight) && world.LightEntity() == entt::null, "primary light handle is checked after deletion");
    world.CreateCornell(render::BuildCornellScene());
    Check(world.CameraEntity() != entt::null && !world.Registry().valid(originalCamera), "reimport creates fresh valid camera handle");
}

void SnapshotOrdering() {
    gameplay::World world;
    const auto first = world.CreateEntity("First");
    const auto second = world.CreateEntity("Second");
    const auto third = world.CreateEntity("Third");
    for (auto entity : {first, second, third}) world.Registry().emplace<gameplay::MeshRenderer>(entity, 0u, 1u, 0u, true);
    const auto snapshot = world.ExtractRenderSnapshot();
    Check(std::is_sorted(snapshot.instances.begin(), snapshot.instances.end(), [](const auto& a, const auto& b) {
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    }), "snapshot order is stable by entity identity");
    world.Update(0.1f);
    const auto repeated = world.ExtractRenderSnapshot();
    for (std::size_t index = 0; index < snapshot.instances.size(); ++index)
        Check(snapshot.instances[index].entity == repeated.instances[index].entity, "unchanged world keeps snapshot entity order");
}
void ImportedMeshPartsAndEditableMaterials() {
    render::Scene scene;
    scene.materials.resize(2);
    scene.materials[0].baseRoughness={0.2f,0.3f,0.4f,0.5f};
    scene.triangles={{{-1,0,0,0},{0,1,0,1},{0,0,0,0}},{{0,0,0,1},{0,1,0,2},{1,0,0,0}}};
    scene.meshParts={{"Object A",0,1,0},{"Object B",1,1,1}};
    gameplay::World world;world.CreateScene(scene);
    const auto snapshot=world.ExtractRenderSnapshot();
    Check(snapshot.instances.size()==2,"generic source primitive boundaries become separate mesh entities");
    for(const auto& instance:snapshot.instances) {
        const auto& part=scene.meshParts[instance.mesh.firstTriangle];
        Check(world.Registry().get<gameplay::Name>(instance.entity).value==part.name,"generic mesh part name remains inspectable");
        Check(instance.mesh.triangleCount==part.triangleCount&&instance.mesh.materialIndex==part.materialIndex,"generic mesh ranges preserve material ownership");
    }
    Check(snapshot.materials.size()==2&&Near(snapshot.materials[0].baseRoughness.x,0.2f),"snapshot owns source materials");
    const auto revision=world.Revision();world.Materials()[0].principled.z=0.7f;world.MarkDirty();
    const auto edited=world.ExtractRenderSnapshot();
    Check(edited.revision>revision&&Near(edited.materials[0].principled.z,0.7f)&&snapshot.materials[0].principled.z==0,"material edits update revision without mutating previous snapshots");
    Check(world.Registry().get<gameplay::Transform>(world.CameraEntity()).position.z<0,"generic scene camera is framed from source bounds");
}
void ImportedLightsPreserveAllFiveTypes() {
    auto scene=render::BuildCornellScene();scene.lights.clear();scene.lightNames.clear();
    for(unsigned type=0;type<5;++type) {
        render::GpuLight light;
        light.positionType={float(type),2,3,float(type)};light.directionRange={0,0,-1,7};light.colorIntensity={0.2f,0.4f,0.6f,9};
        light.tangentWidth={1,0,0,2};light.bitangentHeight={0,-1,0,3};light.parameters={0.15f,std::cos(0.6f),std::cos(0.3f),0};
        scene.lights.push_back(light);scene.lightNames.push_back("Light "+std::to_string(type));
    }
    gameplay::World world;world.CreateScene(scene);const auto snapshot=world.ExtractRenderSnapshot();
    Check(snapshot.lights.size()==5,"imported light list replaces default light instead of adding one");
    for(std::size_t i=0;i<snapshot.lights.size();++i) {
        const auto& light=snapshot.lights[i];
        Check(static_cast<unsigned>(light.light.type)==i,"all five authored light types survive ECS conversion");
        Check(Near(light.transform.position.x,float(i))&&Near(light.light.intensity,9)&&Near(light.light.width,2),"light position power and shape survive conversion");
        Check(Near(light.light.direction.z,-1)&&Near(light.light.bitangent.y,-1)&&Near(light.light.sourceRadius,0.15f),"authored emission frame and source radius survive conversion");
        Check(Near(light.light.outerAngleDegrees,0.6f*180/3.141592654f)&&Near(light.light.innerAngleDegrees,0.3f*180/3.141592654f),"spot cosine representation round-trips through ECS degrees");
    }
}
void GenericImportRejectsInvalidPartsAtomically() {
    const auto source=render::BuildCornellScene();gameplay::World world;world.CreateScene(source);
    const auto original=world.CameraEntity();const auto revision=world.Revision();
    auto malformed=source;malformed.meshParts={{"Outside",static_cast<std::uint32_t>(source.triangles.size()),1,0}};
    Rejects([&]{world.CreateScene(malformed);},"mesh part outside reference must reject before replacing world");
    Check(world.CameraEntity()==original&&world.Revision()==revision,"invalid mesh part preserves prior world and revision");
    malformed.meshParts={{"A",0,2,0},{"B",1,1,0}};
    Rejects([&]{world.CreateScene(malformed);},"overlapping mesh parts reject before creating ECS owners");
    Check(world.CameraEntity()==original,"overlap rejection preserves primary handles");
    malformed=source;malformed.triangles.clear();malformed.meshParts.clear();
    Rejects([&]{world.CreateScene(malformed);},"empty scenes must not produce an infinite camera pose");
}

void EmitterMeshOwnershipSurvivesMaterialEdits() {
    const auto source = render::BuildCornellScene();
    gameplay::World world;
    world.CreateCornell(source);
    unsigned ownedTriangles = 0;
    // The renderer's immutable source range, rather than the editable shared material, establishes ownership.
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each()) {
        (void)entity;
        const bool emitter = source.triangles[mesh.firstTriangle].a.w == 3;
        Check(mesh.lightOwned == emitter, "only original Cornell emitter ranges belong to the light");
        if (emitter) ownedTriangles += mesh.triangleCount;
        mesh.materialIndex = emitter ? 5u : 3u;
    }
    Check(ownedTriangles == 2, "ownership covers exactly the original ceiling quad");
    const auto edited = world.ExtractRenderSnapshot();
    for (const auto& instance : edited.instances)
        Check(instance.mesh.lightOwned == (source.triangles[instance.mesh.firstTriangle].a.w == 3),
              "snapshot preserves emitter ownership across ordinary and emitter material edits");
    gameplay::World generic;
    generic.CreateScene(source);
    for (const auto& instance : generic.ExtractRenderSnapshot().instances)
        Check(!instance.mesh.lightOwned, "generic material index three never creates implicit Cornell ownership");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"entity lifecycle", EntityLifecycle}, {"Cornell ECS import", CornellEntities},
        {"velocity integration", VelocityIntegration}, {"atomic invalid update", InvalidUpdateIsAtomic},
        {"snapshot filtering and isolation", SnapshotIsolationAndFiltering},
        {"import validation and handles", ImportValidationAndPrimaryHandles}, {"stable snapshot order", SnapshotOrdering},
        {"generic mesh parts and material edits", ImportedMeshPartsAndEditableMaterials},
        {"all imported light types", ImportedLightsPreserveAllFiveTypes},
        {"generic import atomic validation", GenericImportRejectsInvalidPartsAtomically},
        {"emitter ownership independent of material", EmitterMeshOwnershipSurvivesMaterialEdits},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failed) << '/' << tests.size() << " gameplay tests passed\n";
    return failed ? 1 : 0;
}
