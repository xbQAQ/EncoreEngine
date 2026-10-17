#include "gameplay/World.h"
#include "render/Bvh.h"
#include "render/SceneExtraction.h"

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

bool Near(float left, float right, float tolerance = 0.0002f) { return std::abs(left - right) <= tolerance; }
render::Vec3 Position(const render::Float4& vertex) { return {vertex.x, vertex.y, vertex.z}; }
render::Float4 Vertex(const render::Vec3& point, std::uint32_t material) { return {point.x, point.y, point.z, static_cast<float>(material)}; }

template <typename Function> void Rejects(Function&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, message);
}

gameplay::RenderSnapshot SnapshotForSingleMesh(const render::Scene& scene, bool visible = true) {
    gameplay::RenderSnapshot snapshot;
    snapshot.revision = 1;
    gameplay::RenderInstance instance;
    instance.entity = entt::entity{1};
    instance.transform.scale = {1, 1, 1};
    instance.mesh = {0, static_cast<std::uint32_t>(scene.triangles.size()), 5, visible};
    snapshot.instances.push_back(instance);
    return snapshot;
}

void TransformAndMaterialOverride() {
    render::Scene reference;
    reference.materials.resize(6);
    reference.triangles = {{Vertex({1, 0, 0}, 0), Vertex({1, 1, 0}, 0), Vertex({1, 0, 1}, 0)}};
    gameplay::RenderSnapshot snapshot = SnapshotForSingleMesh(reference);
    snapshot.instances.front().transform = {{10, 20, 30}, {0, 0, 90}, {2, 3, 4}};
    const auto extracted = render::ExtractCornellRenderScene(reference, snapshot);
    Check(extracted.triangles.size() == reference.triangles.size(), "extraction preserves triangle allocation");
    Check(Near(extracted.triangles[0].a.x, 10) && Near(extracted.triangles[0].a.y, 22) && Near(extracted.triangles[0].a.z, 30), "scale then Euler Z then translation");
    Check(Near(extracted.triangles[0].b.x, 7) && Near(extracted.triangles[0].b.y, 22) && Near(extracted.triangles[0].b.z, 30), "Euler transform applies to second vertex");
    Check(Near(extracted.triangles[0].c.x, 10) && Near(extracted.triangles[0].c.y, 22) && Near(extracted.triangles[0].c.z, 34), "nonuniform scale is preserved");
    Check(extracted.triangles[0].a.w == 5 && extracted.triangles[0].b.w == 0, "mesh material overrides only GPU material lane a.w");

    reference.triangles.front().a = Vertex({1, 2, 3}, 0);
    snapshot.instances.front().transform.rotationDegrees = {90, 90, 90};
    const auto rotated = render::ExtractCornellRenderScene(reference, snapshot);
    Check(Near(rotated.triangles[0].a.x, 22) && Near(rotated.triangles[0].a.y, 26) && Near(rotated.triangles[0].a.z, 28),
          "Euler rotations apply X then Y then Z");
}

void VisibilityAndStableBvh() {
    auto reference = render::BuildCornellScene();
    render::Bvh{}.Build(reference.triangles);
    auto snapshot = SnapshotForSingleMesh(reference, false);
    auto hidden = render::ExtractCornellRenderScene(reference, snapshot);
    Check(hidden.triangles.size() == reference.triangles.size(), "hidden extraction preserves triangle count");
    for (std::size_t index = 0; index < hidden.triangles.size(); ++index) {
        const auto& triangle = hidden.triangles[index];
        Check(Near(triangle.a.x, 0) && Near(triangle.a.y, -100000) && Near(triangle.a.z, 0), "hidden triangle is moved outside ray domain");
        Check(Near(triangle.a.w, reference.triangles[index].a.w == 3 ? 3.0f : 5.0f), "hidden ranges retain material overrides and pinned emitter material");
        Check(render::Length(render::Cross(Position(triangle.b) - Position(triangle.a), Position(triangle.c) - Position(triangle.a))) == 0,
              "hidden triangles have exactly zero area");
    }
    const auto nodes = render::Bvh{}.Build(hidden.triangles);
    const auto hit = render::Bvh::Trace({{0, 1.4f, -4.6f}, {0, 0, 1}}, hidden.triangles, nodes);
    Check(!hit.Found(), "hidden triangles cannot be hit by the renderer");
}

void EmitterFollowsFirstRectangleLight() {
    auto reference = render::BuildCornellScene();
    render::Bvh{}.Build(reference.triangles);
    auto world = gameplay::World{};
    world.CreateCornell(reference);
    auto snapshot = world.ExtractRenderSnapshot();
    Check(snapshot.lights.size() == 1, "Cornell snapshot has one light");
    snapshot.lights.front().transform.position = {0.3f, 2.1f, -0.4f};
    snapshot.lights.front().transform.rotationDegrees = {0, 90, 0};
    snapshot.lights.front().light.width = 1.2f;
    snapshot.lights.front().light.height = 0.6f;
    snapshot.lights.front().light.color = {0.5f, 0.25f, 0.125f};
    snapshot.lights.front().light.intensity = 8;
    // Emitter pose and material belong to the light even when its mesh is edited independently.
    for (auto& instance : snapshot.instances) {
        if (instance.mesh.materialIndex != 3) continue;
        instance.transform = {{100, 100, 100}, {35, 45, 55}, {2, 3, 4}};
        instance.mesh.materialIndex = 5;
    }
    auto disabled = snapshot.lights.front();
    disabled.light.enabled = false;
    disabled.transform.position = {10, 10, 10};
    auto point = disabled;
    point.light.enabled = true;
    point.light.type = gameplay::LightType::Point;
    auto laterRectangle = snapshot.lights.front();
    laterRectangle.transform.position = {20, 20, 20};
    snapshot.lights.insert(snapshot.lights.begin(), point);
    snapshot.lights.insert(snapshot.lights.begin(), disabled);
    snapshot.lights.push_back(laterRectangle);
    const auto extracted = render::ExtractCornellRenderScene(reference, snapshot);
    std::size_t emitterTriangle = 0;
    while (emitterTriangle < extracted.triangles.size() && extracted.triangles[emitterTriangle].a.w != 3) ++emitterTriangle;
    Check(emitterTriangle < extracted.triangles.size(), "emitter material geometry remains present");
    const auto& emitter = extracted.triangles[emitterTriangle];
    Check(Near(emitter.a.w, 3), "emitter material remains material three");
    Check(Near(emitter.a.x, 0) && Near(emitter.a.y, 2.1f) && Near(emitter.a.z, 0.2f), "emitter uses light transform and normalized width/height");
    Check(Near(extracted.materials[3].emissionMetallic.x, 4) && Near(extracted.materials[3].emissionMetallic.y, 2) &&
              Near(extracted.materials[3].emissionMetallic.z, 1), "emitter radiance equals light color times intensity");

    for (auto& light : snapshot.lights) light.light.enabled = false;
    const auto unlit = render::ExtractCornellRenderScene(reference, snapshot);
    Check(Near(unlit.materials[3].emissionMetallic.x, 0) && Near(unlit.materials[3].emissionMetallic.y, 0) &&
              Near(unlit.materials[3].emissionMetallic.z, 0), "disabled rectangle removes emitter radiance");
}

void EmitterIdentityIsIndependentOfSharedMaterial() {
    const auto reference = render::BuildCornellScene();
    gameplay::World world;
    world.CreateCornell(reference);
    auto snapshot = world.ExtractRenderSnapshot();
    // Ordinary objects may share emissive material parameters without becoming part of the analytic light proposal.
    for (auto& instance : snapshot.instances) instance.mesh.materialIndex = 3;
    auto point = snapshot.lights.front();
    point.light.type = gameplay::LightType::Point;
    auto disabled = point;
    disabled.light.enabled = false;
    snapshot.lights.insert(snapshot.lights.begin(), point);
    snapshot.lights.insert(snapshot.lights.begin(), disabled);
    const auto extracted = render::ExtractCornellRenderScene(reference, snapshot);
    unsigned emitterTriangles = 0;
    for (std::size_t index = 0; index < reference.triangles.size(); ++index) {
        const bool emitter = reference.triangles[index].a.w == 3;
        Check(extracted.triangles[index].a.w == 3, "ordinary meshes can retain shared emissive material edits");
        Check(extracted.triangles[index].c.w == (emitter ? 2.0f : 0.0f),
              "only original Cornell emitter geometry owns the enabled analytic-light index");
        if (emitter) ++emitterTriangles;
    }
    // BVH permutations must carry identity in the triangle itself, independent of material or source ordering.
    auto reordered = extracted.triangles;
    render::Bvh{}.Build(reordered);
    unsigned tagged = 0;
    for (const auto& triangle : reordered) {
        if (triangle.c.w == 0) continue;
        Check(triangle.c.w == 2, "BVH sorting preserves the exact analytic-light tag");
        ++tagged;
    }
    Check(tagged == emitterTriangles && tagged == 2, "exactly the original ceiling quad carries light identity");
    snapshot.lights[1].light.enabled = false;
    const auto reindexed = render::ExtractCornellRenderScene(reference, snapshot);
    for (std::size_t index = 0; index < reference.triangles.size(); ++index)
        Check(reindexed.triangles[index].c.w == (reference.triangles[index].a.w == 3 ? 1.0f : 0.0f),
              "disabled lights do not leave stale indices in emitter primitives");
}

void InvalidSnapshotsRejectedWithoutSideEffects() {
    const auto reference = render::BuildCornellScene();
    auto overlapping = SnapshotForSingleMesh(reference);
    overlapping.instances.push_back(overlapping.instances.front());
    Rejects([&] { render::ExtractCornellRenderScene(reference, overlapping); }, "overlapping mesh ranges rejected");

    auto outOfRange = SnapshotForSingleMesh(reference);
    outOfRange.instances.front().mesh.firstTriangle = static_cast<std::uint32_t>(reference.triangles.size() - 1);
    outOfRange.instances.front().mesh.triangleCount = 2;
    Rejects([&] { render::ExtractCornellRenderScene(reference, outOfRange); }, "out of range mesh rejected");

    auto badMaterial = SnapshotForSingleMesh(reference);
    badMaterial.instances.front().mesh.materialIndex = 6;
    Rejects([&] { render::ExtractCornellRenderScene(reference, badMaterial); }, "mesh material outside scene rejected");

    auto badTransform = SnapshotForSingleMesh(reference);
    badTransform.instances.front().transform.position.x = std::numeric_limits<float>::quiet_NaN();
    Rejects([&] { render::ExtractCornellRenderScene(reference, badTransform); }, "nonfinite mesh transform rejected");

    auto overflow = SnapshotForSingleMesh(reference);
    overflow.instances.front().transform.scale = {std::numeric_limits<float>::max(), 1, 1};
    Rejects([&] { render::ExtractCornellRenderScene(reference, overflow); }, "finite components producing coordinate overflow rejected");

    auto badLight = SnapshotForSingleMesh(reference);
    gameplay::RenderLight light;
    light.entity = entt::entity{2};
    light.transform.position = {0, 2, 0};
    light.light.type = gameplay::LightType::Rectangle;
    light.light.width = -1;
    badLight.lights.push_back(light);
    Rejects([&] { render::ExtractCornellRenderScene(reference, badLight); }, "negative rectangle extent rejected");

    auto badCamera = SnapshotForSingleMesh(reference);
    gameplay::RenderCamera camera;
    camera.camera.nearPlane = std::numeric_limits<float>::quiet_NaN();
    badCamera.cameras.push_back(camera);
    Rejects([&] { render::ExtractCornellRenderScene(reference, badCamera); }, "nonfinite camera component rejected");

    auto badReference = reference;
    badReference.materials.resize(5);
    Rejects([&] { render::ExtractCornellRenderScene(badReference, SnapshotForSingleMesh(badReference)); }, "Cornell material layout rejected");
}

void UnrepresentedRangesStayStable() {
    render::Scene reference;
    reference.materials.resize(6);
    reference.triangles = {
        // Only a.w is a material lane; b.w zero requests the geometric attribute fallback.
        {Vertex({0, 0, 0}, 1), Vertex({1, 0, 0}, 0), Vertex({0, 1, 0}, 0)},
        {Vertex({0, 0, 1}, 2), Vertex({1, 0, 1}, 0), Vertex({0, 1, 1}, 0)},
    };
    const auto extracted = render::ExtractCornellRenderScene(reference, {});
    Check(extracted.triangles.size() == 2, "unrepresented geometry keeps stable count");
    for (std::size_t index = 0; index < extracted.triangles.size(); ++index) {
        Check(Near(extracted.triangles[index].a.x, 0) && Near(extracted.triangles[index].a.y, -100000), "unrepresented triangle is degenerate");
        Check(extracted.triangles[index].a.w == reference.triangles[index].a.w, "unrepresented material lane preserved");
    }
    auto partial = SnapshotForSingleMesh(reference);
    partial.instances.front().mesh.firstTriangle = 1;
    partial.instances.front().mesh.triangleCount = 1;
    partial.instances.front().transform.position = {2, 0, 0};
    const auto represented = render::ExtractCornellRenderScene(reference, partial);
    Check(Near(represented.triangles[0].a.y, -100000) && Near(represented.triangles[1].a.z, 1) &&
          Near(represented.triangles[1].a.x, 2), "partial mesh preserves original triangle slots");
}
render::Scene GenericReference() {
    render::Scene scene;scene.materials.resize(2);scene.materialNames={"Paint","Coat"};
    scene.materials[0].baseRoughness={0.1f,0.2f,0.3f,0.4f};
    scene.triangles={{{1,0,0,0},{0,1,0,1},{0,0,0,0}},{{1,0,1,1},{0,1,1,2},{0,0,1,0}}};
    const render::Float4 normal{0.70710678f,0.70710678f,0,1};
    const render::GpuTriangleAttributes attributes{normal,normal,normal,{0,0,1,0},{0,1,0,0},{0.70710678f,-0.70710678f,0,1}};
    scene.attributes={attributes,attributes};scene.meshParts={{"A",0,1,0},{"B",1,1,1}};
    scene.packedTexels={0xffffffff};render::GpuTextureView view;view.image={0,1,1,0};scene.textureViews={view};
    return scene;
}
gameplay::RenderSnapshot GenericSnapshot() {
    gameplay::RenderSnapshot snapshot;snapshot.instances.push_back({entt::entity{1},{},{0,2,0,true}});return snapshot;
}
void GenericAttributesAndMaterialEdits() {
    const auto reference=GenericReference();auto snapshot=GenericSnapshot();snapshot.instances[0].transform={{10,20,30},{0,0,90},{2,1,3}};
    snapshot.materials=reference.materials;snapshot.materials[0].principled.z=0.8f;snapshot.materials[0].textures.x=0;
    const auto extracted=render::ExtractRenderScene(reference,snapshot);
    Check(extracted.triangles.size()==2&&extracted.attributes.size()==2&&extracted.triangles[0].b.w==1&&extracted.triangles[1].b.w==2,"generic extraction preserves attribute and primitive identities");
    Check(Near(extracted.attributes[0].normal0.x,-0.894427f)&&Near(extracted.attributes[0].normal0.y,0.447214f),"ECS nonuniform edit uses inverse-transpose normals");
    Check(Near(extracted.attributes[0].tangent.x,0.447214f)&&Near(extracted.attributes[0].tangent.y,0.894427f),"ECS tangent follows forward transform");
    Check(extracted.meshParts[1].firstTriangle==1&&extracted.meshParts[1].name=="B"&&extracted.textureViews.size()==1&&extracted.packedTexels==reference.packedTexels,"source metadata and owned texture data survive extraction");
    Check(Near(extracted.materials[0].principled.z,0.8f)&&extracted.materials[0].textures.x==0&&reference.materials[0].principled.z==0,"snapshot material edits reach output without mutating reference");
    snapshot.instances[0].transform.scale={-1,1,1};snapshot.instances[0].transform.rotationDegrees={};snapshot.instances[0].transform.position={};
    const auto mirrored=render::ExtractRenderScene(reference,snapshot);
    const auto& triangle=mirrored.triangles[0];
    Check(render::Cross(Position(triangle.b)-Position(triangle.a),Position(triangle.c)-Position(triangle.a)).z>0,"mirrored ECS edits preserve authored front-face winding");
    Check(Near(mirrored.attributes[0].uv01.z,0)&&Near(mirrored.attributes[0].uv01.w,1)&&Near(mirrored.attributes[0].uv2.x,1),"mirrored winding swaps vertex normal and UV associations");
    Check(mirrored.triangles[0].b.w==1&&Near(mirrored.attributes[0].tangent.w,-1),"mirrored edits keep attribute index and update tangent handedness");
}

void SharedMeshInstancesHaveIndependentGeometryAttributesAndOwners() {
    const auto reference = GenericReference();
    gameplay::World world;
    world.CreateScene(reference);
    gameplay::Entity original{entt::null};
    for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
        if (mesh.firstTriangle == 0) original = entity;
    const auto mesh = world.Registry().get<gameplay::MeshRenderer>(original);
    world.Registry().get<gameplay::Transform>(original) = {{10, 20, 30}, {0, 0, 90}, {2, 1, 3}};
    const auto clone = world.CreateEntity("Shared mesh clone");
    world.Registry().emplace<gameplay::MeshRenderer>(clone, mesh);
    world.Registry().get<gameplay::MeshRenderer>(clone).materialIndex = 1;
    world.Registry().get<gameplay::Transform>(clone) = {{-4, 5, 6}, {}, {-1, 1, 1}};
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    std::vector<gameplay::Entity> owners;
    const auto snapshot = world.ExtractRenderSnapshot();
    const auto extracted = render::ExtractRenderScene(reference, snapshot, false, &owners);
    Check(extracted.triangles.size() == 3 && extracted.attributes.size() == 3, "shared resource clone appends one triangle and one private attribute");
    Check(Near(extracted.triangles[0].a.x, 10) && Near(extracted.triangles[0].a.y, 22) &&
        Near(extracted.triangles[2].a.x, -5) && Near(extracted.triangles[2].a.y, 5), "original source slot and appended clone each receive their own pose");
    Check(extracted.triangles[0].b.w == 1 && extracted.triangles[2].b.w == 3 &&
        extracted.triangles[0].a.w == 0 && extracted.triangles[2].a.w == 1, "clone attribute and material identities do not overwrite original instance");
    Check(Near(extracted.attributes[0].normal0.x, -0.894427f) && Near(extracted.attributes[0].normal0.y, 0.447214f) &&
        Near(extracted.attributes[2].normal0.x, -0.707107f) && Near(extracted.attributes[2].normal0.y, 0.707107f),
        "each instance applies its own inverse-transpose normal transform");
    Check(Near(extracted.attributes[2].uv01.z, 0) && Near(extracted.attributes[2].uv01.w, 1) &&
        Near(extracted.attributes[2].uv2.x, 1) && Near(extracted.attributes[2].tangent.w, -1),
        "mirrored clone keeps UV corner associations and changes tangent handedness privately");
    Check(owners.size() == 3 && owners[0] == original && owners[2] == clone,
        "unsorted output triangles report exact original and clone entity ownership");
    Check(reference.triangles.size() == 2 && reference.attributes.size() == 2 &&
        Near(reference.attributes[0].normal0.x, 0.707107f) && Near(reference.attributes[0].uv01.z, 1),
        "source triangle and attribute resources remain immutable");

    // Hidden first owners retain degenerate source slots while visible clones still append their independent geometry.
    auto hidden = snapshot;
    for (auto& item : hidden.instances) if (item.entity == original) item.mesh.visible = false;
    const auto visibleClone = render::ExtractRenderScene(reference, hidden, false, &owners);
    Check(Near(visibleClone.triangles[0].a.y, -100000) && Near(visibleClone.triangles[2].a.x, -5),
        "hiding original cannot hide a visible shared-resource clone");
    auto onlyClone = snapshot;
    onlyClone.instances.erase(std::remove_if(onlyClone.instances.begin(), onlyClone.instances.end(),
        [original](const auto& item) { return item.entity == original; }), onlyClone.instances.end());
    const auto promoted = render::ExtractRenderScene(reference, onlyClone, false, &owners);
    Check(promoted.triangles.size() == 2 && owners[0] == clone && Near(promoted.triangles[0].a.x, -5),
        "remaining clone can safely take the stable source slots when its original entity is removed");
}

void SharedMeshValidationKeepsPartialOverlapAndDuplicateEntitiesInvalid() {
    const auto reference = GenericReference();
    gameplay::World world;
    world.CreateScene(reference);
    auto snapshot = world.ExtractRenderSnapshot();
    snapshot.lights.clear();
    auto clone = snapshot.instances.front();
    clone.entity = entt::entity{100};
    snapshot.instances.push_back(clone);
    Check(render::ExtractRenderScene(reference, snapshot).triangles.size() == 3,
        "distinct entities with an equal nonzero resource handle may share an exact source range");
    auto invalid = snapshot;
    invalid.instances.back().entity = invalid.instances.front().entity;
    Rejects([&] { render::ExtractRenderScene(reference, invalid); }, "same entity cannot appear twice in one snapshot");
    invalid = snapshot;
    invalid.instances.back().mesh.triangleCount = 2;
    Rejects([&] { render::ExtractRenderScene(reference, invalid); }, "partial overlaps cannot masquerade as a shared source instance");
    invalid = snapshot;
    invalid.instances.back().mesh.meshResource = {};
    Rejects([&] { render::ExtractRenderScene(reference, invalid); }, "overlap without shared resource identity remains invalid");
    invalid = snapshot;
    ++invalid.instances.back().mesh.meshResource.value;
    Rejects([&] { render::ExtractRenderScene(reference, invalid); }, "different resource identities cannot claim the same source slots");
    invalid = snapshot;
    invalid.instances.back().entity = entt::null;
    Rejects([&] { render::ExtractRenderScene(reference, invalid); }, "null entity cannot establish independent instance ownership");

    // Failure must not publish a partially overwritten ownership map to selection code.
    std::vector<gameplay::Entity> owners{entt::entity{999}};
    Rejects([&] { render::ExtractRenderScene(reference, invalid, false, &owners); }, "invalid snapshot is rejected before ownership publication");
    Check(owners.size() == 1 && owners.front() == entt::entity{999}, "failed extraction leaves caller ownership output untouched");
}
void AllLightTypesGenerateMatchingEmitterGeometry() {
    auto reference=GenericReference();reference.lightNames={"Stale source name"};auto snapshot=GenericSnapshot();
    for(unsigned type=0;type<5;++type) {
        gameplay::RenderLight item;item.entity=entt::entity{type+10};item.transform.position={float(type)*4,2,0};item.transform.rotationDegrees={0,0,90};
        item.light.type=static_cast<gameplay::LightType>(type);item.light.width=2;item.light.height=4;item.light.color={0.2f,0.4f,0.6f};item.light.intensity=5;
        snapshot.lights.push_back(item);
    }
    const auto scene=render::ExtractRenderScene(reference,snapshot);
    Check(scene.lights.size()==5&&scene.lightNames.size()==5,"extracted light names remain aligned after replacing source lights");
    Check(scene.triangles.size()==2+2+64&&scene.materials.size()==4,"only rectangle and disk add visible emitter geometry and materials");
    double rectangleArea=0,diskArea=0;
    for(std::size_t i=2;i<scene.triangles.size();++i) {
        const auto& triangle=scene.triangles[i];const auto cross=render::Cross(Position(triangle.b)-Position(triangle.a),Position(triangle.c)-Position(triangle.a));
        Check(cross.x>0&&triangle.b.w==0,"generated emitter winding follows rotated emission direction and selects geometric attributes");
        Check(triangle.c.w == (triangle.a.w == 2 ? 3.0f : 4.0f), "generated rectangle and disk encode their own analytic light index");
        (triangle.a.w==2?rectangleArea:diskArea)+=0.5*render::Length(cross);
    }
    Check(std::abs(rectangleArea-8)<0.001&&std::abs(diskArea-64*std::sin(2*3.141592653589793/64))<0.001,"generated rectangle and 64-sector ellipse match analytic sampling area");
    for(unsigned i=0;i<5;++i) {
        const auto& light=scene.lights[i];
        Check(Near(light.positionType.w,float(i))&&Near(light.directionRange.x,1)&&Near(light.tangentWidth.y,1)&&Near(light.bitangentHeight.z,1),"all light types share the same rotated authored frame");
    }
    Check(scene.lights[2].shadow.z==2&&scene.lights[3].shadow.z==3&&Near(scene.materials[3].emissionMetallic.z,3),"area-light material identity agrees with visible emitted radiance");
    auto triangles=scene.triangles;const auto nodes=render::Bvh{}.Build(triangles);
    const auto hit=render::Bvh::Trace({{7,2,0},{1,0,0}},triangles,nodes);
    Check(hit.Found()&&hit.materialIndex==2&&Near(hit.distance,1),"generated rotated emitter participates in the same BVH as source geometry");
}
void GenericExtractionRejectsInvalidGpuPayloads() {
    auto reference=GenericReference();const auto snapshot=GenericSnapshot();
    reference.materials[0].principled.y=std::numeric_limits<float>::quiet_NaN();
    Rejects([&]{render::ExtractRenderScene(reference,snapshot);},"new reference material lanes must reject nonfinite values");
    reference=GenericReference();auto edited=snapshot;edited.materials=reference.materials;edited.materials[0].extras.x=std::numeric_limits<float>::infinity();
    Rejects([&]{render::ExtractRenderScene(reference,edited);},"edited material lanes must reject nonfinite values");
    edited.materials=reference.materials;edited.materials[0].textures.x=2;
    Rejects([&]{render::ExtractRenderScene(reference,edited);},"edited texture-view bindings must remain in range");
    reference.triangles[0].b.w=0.5f;
    Rejects([&]{render::ExtractRenderScene(reference,snapshot);},"fractional attribute references must reject before indexing");
    reference=GenericReference();reference.attributes[0].uv2.y=std::numeric_limits<float>::quiet_NaN();
    Rejects([&]{render::ExtractRenderScene(reference,snapshot);},"nonfinite UV payload must reject before GPU upload");
    reference=GenericReference();auto lightSnapshot=snapshot;gameplay::RenderLight light;light.light.tangent=light.light.direction;lightSnapshot.lights.push_back(light);
    Rejects([&]{render::ExtractRenderScene(reference,lightSnapshot);},"parallel sampling axes must not create a degenerate area emitter");
}
void SpotBoundaryAndLightLimit() {
    const auto reference=GenericReference();auto snapshot=GenericSnapshot();gameplay::RenderLight spot;spot.light.type=gameplay::LightType::Spot;spot.light.outerAngleDegrees=90;spot.light.innerAngleDegrees=0;snapshot.lights.push_back(spot);
    const auto extracted=render::ExtractRenderScene(reference,snapshot);
    Check(extracted.lights.size()==1&&std::abs(extracted.lights[0].parameters.y)<0.0001f,"glTF's valid 90-degree outer half-cone survives extraction");
    snapshot.lights.assign(17,spot);
    Rejects([&]{render::ExtractRenderScene(reference,snapshot);},"seventeen enabled lights reject instead of truncating");
    snapshot.lights.back().light.enabled=false;
    Check(render::ExtractRenderScene(reference,snapshot).lights.size()==16,"disabled lights do not consume enabled-light budget");
}

// 比较契约覆盖全部非相机字段，避免新增快路径吞掉材质、资源或解析灯的真实修改。
void CameraIndependentSceneInputs() {
    gameplay::World world;
    world.CreateCornell(render::BuildCornellScene());
    const auto baseline = world.ExtractRenderSnapshot();
    auto cameraOnly = baseline;
    ++cameraOnly.revision;
    cameraOnly.cameras.front().transform = {{1, 2, 3}, {10, 20, 30}, {2, 3, 4}};
    cameraOnly.cameras.front().camera = {70, .1f, 200, true};
    Check(render::SameRenderSceneInputs(baseline, cameraOnly), "Camera poses, projection and revisions do not change scene inputs");
    cameraOnly.cameras.clear();
    Check(render::SameRenderSceneInputs(baseline, cameraOnly), "Fallback camera selection does not change scene inputs");
    const auto changesScene = [&](const auto& edit) {
        auto changed = baseline;
        edit(changed);
        Check(!render::SameRenderSceneInputs(baseline, changed) && !render::SameRenderSceneInputs(changed, baseline),
            "Every changed scene input must invalidate in either comparison direction");
    };
    changesScene([](auto& s) { s.instances.pop_back(); });
    changesScene([](auto& s) { s.materials.pop_back(); });
    changesScene([](auto& s) { s.lights.clear(); });
    changesScene([](auto& s) { s.instances.front().entity = entt::null; });
    changesScene([](auto& s) { ++s.instances.front().mesh.firstTriangle; });
    changesScene([](auto& s) { ++s.instances.front().mesh.triangleCount; });
    changesScene([](auto& s) { ++s.instances.front().mesh.materialIndex; });
    changesScene([](auto& s) { s.instances.front().mesh.visible = !s.instances.front().mesh.visible; });
    changesScene([](auto& s) { s.instances.front().mesh.lightOwned = !s.instances.front().mesh.lightOwned; });
    changesScene([](auto& s) { ++s.instances.front().mesh.meshResource.value; });
    changesScene([](auto& s) { s.instances.front().mesh.useSurfaceMaterials = !s.instances.front().mesh.useSurfaceMaterials; });

    // 网格和灯光都消费完整变换，相机兼任其中之一时仍须失效场景。
    for (const auto member : {&gameplay::Transform::position, &gameplay::Transform::rotationDegrees, &gameplay::Transform::scale})
        for (const auto axis : {&render::Vec3::x, &render::Vec3::y, &render::Vec3::z}) {
            changesScene([&](auto& s) { (s.instances.front().transform.*member).*axis += 1; });
            changesScene([&](auto& s) { (s.lights.front().transform.*member).*axis += 1; });
        }
    changesScene([](auto& s) { s.lights.front().entity = entt::null; });
    changesScene([](auto& s) { s.lights.front().light.type = gameplay::LightType::Point; });
    changesScene([](auto& s) { s.lights.front().light.enabled = !s.lights.front().light.enabled; });
    changesScene([](auto& s) { ++s.lights.front().light.emitterMaterial; });
    for (const auto member : {&gameplay::Light::color, &gameplay::Light::direction, &gameplay::Light::tangent, &gameplay::Light::bitangent})
        for (const auto axis : {&render::Vec3::x, &render::Vec3::y, &render::Vec3::z})
            changesScene([&](auto& s) { (s.lights.front().light.*member).*axis += 1; });
    for (const auto member : {&gameplay::Light::intensity, &gameplay::Light::width, &gameplay::Light::height,
                             &gameplay::Light::range, &gameplay::Light::sourceRadius,
                             &gameplay::Light::outerAngleDegrees, &gameplay::Light::innerAngleDegrees})
        changesScene([&](auto& s) { s.lights.front().light.*member += 1; });

    // 材质浮点通道、贴图索引和标志都影响渲染或透明拾取，必须完整参与比较。
    for (const auto member : {&render::GpuMaterial::baseRoughness, &render::GpuMaterial::emissionMetallic,
                             &render::GpuMaterial::transmissionIor, &render::GpuMaterial::principled,
                             &render::GpuMaterial::sheen, &render::GpuMaterial::medium,
                             &render::GpuMaterial::extras, &render::GpuMaterial::tangentFrame})
        for (const auto lane : {&render::Float4::x, &render::Float4::y, &render::Float4::z, &render::Float4::w})
            changesScene([&](auto& s) { (s.materials.front().*member).*lane += 1; });
    for (const auto member : {&render::GpuMaterial::flags, &render::GpuMaterial::textures, &render::GpuMaterial::textures2})
        for (const auto lane : {&render::Uint4::x, &render::Uint4::y, &render::Uint4::z, &render::Uint4::w})
            changesScene([&](auto& s) { ++((s.materials.front().*member).*lane); });
    Check(render::SameRenderSceneInputs({}, {}), "Empty scene inputs compare without dereferencing storage");
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"transform and material override", TransformAndMaterialOverride},
        {"visibility and stable BVH", VisibilityAndStableBvh},
        {"emitter follows rectangle light", EmitterFollowsFirstRectangleLight},
        {"emitter identity independent of shared material", EmitterIdentityIsIndependentOfSharedMaterial},
        {"invalid snapshots", InvalidSnapshotsRejectedWithoutSideEffects},
        {"unrepresented ranges", UnrepresentedRangesStayStable},
        {"generic attributes and material edits", GenericAttributesAndMaterialEdits},
        {"shared mesh instance geometry attributes and owners", SharedMeshInstancesHaveIndependentGeometryAttributesAndOwners},
        {"shared mesh validation boundaries", SharedMeshValidationKeepsPartialOverlapAndDuplicateEntitiesInvalid},
        {"all light types and generated emitter geometry", AllLightTypesGenerateMatchingEmitterGeometry},
        {"invalid generic GPU payloads", GenericExtractionRejectsInvalidGpuPayloads},
        {"spot boundary and enabled light limit", SpotBoundaryAndLightLimit},
        {"camera independent scene input classification", CameraIndependentSceneInputs},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failed) << '/' << tests.size() << " scene extraction tests passed\n";
    return failed ? 1 : 0;
}
