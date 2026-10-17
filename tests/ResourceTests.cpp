#include "gameplay/World.h"
#include "render/SceneExtraction.h"
#ifdef _WIN32
#include "asset/GltfImporter.h"
#endif

#include <functional>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void ExplicitOwnershipAndStaleHandles() {
    static_assert(std::is_base_of_v<engine::Resource, render::TextureResource>);
    static_assert(std::is_base_of_v<engine::Resource, render::MaterialResource>);
    static_assert(std::is_base_of_v<engine::Resource, render::MeshResource>);
    engine::ResourceStore owner;
    const auto first = owner.Create<render::MaterialResource>("Paint");
    auto* material = owner.Resolve(first);
    Check(material != nullptr, "explicit store creation must return a resolvable typed handle");
    Check(material->Name() == "Paint" && material->TypeName() == "MaterialResource" && material->Id() == first.value,
          "resource metadata identifies the concrete asset");
    engine::ResourceStore other;
    Check(other.Resolve(first) == nullptr, "a handle cannot resolve in another owner");
    Check(owner.Resolve(engine::ResourceHandle<render::MeshResource>{first.value}) == nullptr,
          "forged resource type cannot resolve a material as geometry");
    Check(owner.Erase(first) && !owner.Erase(first) && owner.Resolve(first) == nullptr,
          "explicit erase invalidates outstanding handles");
    const auto second = owner.Create<render::MaterialResource>("Second");
    owner.Clear();
    const auto third = owner.Create<render::MaterialResource>("Third");
    Check(owner.Resolve(second) == nullptr && third != second && third != first,
          "clear and later allocations never revive stale handles");
}

render::Scene TexturedScene() {
    auto scene = render::BuildCornellScene();
    scene.materialNames = {"White", "Red", "Green", "Emitter", "Box", "Glass"};
    scene.sourcePath = "Authored/SharedImages.gltf";
    scene.packedTexels = {0xff112233u, 0xff445566u};
    render::GpuTextureView first;
    first.image = {0, 2, 1, 1};
    first.offsetScale = {0.1f, 0.2f, 2, 3};
    render::GpuTextureView second = first;
    second.image.w = 8;
    second.rotation = {1, 0, 0, 0};
    scene.textureViews = {first, second};
    scene.materials[0].textures.x = 0;
    scene.materials[1].textures.x = 1;
    return scene;
}

void WorldResourcesOwnCookedInputs() {
    auto scene = TexturedScene();
    gameplay::World world;
    world.CreateScene(scene);
    Check(world.Resources().Size() == scene.materials.size() + 2,
          "world owns materials, one shared image and one baked multisurface mesh");
    const auto* material = world.Resources().Resolve(world.MaterialHandle(0));
    Check(material && material->Name() == "White", "authored material names reach resource metadata");
    const auto* firstBinding = material->Texture(0);
    const auto* secondBinding = world.Resources().Resolve(world.MaterialHandle(1))->Texture(0);
    Check(firstBinding && secondBinding && firstBinding->texture == secondBinding->texture,
          "material slots refer to one image resource with independent sampling views");
    const auto* texture = world.Resources().Resolve(firstBinding->texture);
    Check(texture && texture->Width() == 2 && texture->Height() == 1 && texture->Texels()[1] == 0xff445566u,
          "texture resources own dimensions and decoded RGBA8 pixels");
    const auto snapshot = world.ExtractRenderSnapshot();
    for (const auto& instance : snapshot.instances) {
        const auto* mesh = world.Resources().Resolve(instance.mesh.meshResource);
        Check(mesh && mesh->Triangles().size() == scene.triangles.size() && !mesh->Surfaces().empty(),
              "ECS geometry ranges bind to a real resource-owned multisurface mesh");
    }
    world.Materials()[0].principled.z = 0.75f;
    world.MarkDirty();
    Check(material->Parameters().principled.z == 0.75f, "legacy material editing projects directly into the resource");
    const auto cooked = world.CookResourceScene();
    Check(cooked.triangles.size() == scene.triangles.size() && cooked.attributes.size() == scene.attributes.size(),
          "resource cooking preserves immutable primitive and attribute identities");
    Check(cooked.packedTexels == scene.packedTexels && cooked.textureViews.size() == 2,
          "two transformed uses retain one shared image payload");
    Check(cooked.textureViews[0].image.w == 1 && cooked.textureViews[1].image.w == 8 &&
          cooked.textureViews[0].offsetScale.z == 2 && cooked.textureViews[1].rotation.x == 1,
          "resource cooking preserves independent sampler and UV transforms");
    Check(cooked.materials[0].principled.z == 0.75f && cooked.sourcePath == scene.sourcePath,
          "cooked rendering inputs come from resource edits and retain source provenance");
    for (auto& parameters : world.Materials()) parameters.extras.w = 0.9f;
    Check(material->Parameters().extras.w == 0.9f, "range-for editing visits resource-owned material parameters");
    Check(!world.MaterialHandle(scene.materials.size()), "out-of-range material selection yields an invalid resource handle");
    // The source transport object may disappear; CPU resources still supply the renderer's reference scene.
    scene = {};
    const auto rendered = render::ExtractRenderScene(cooked, world.ExtractRenderSnapshot());
    Check(!rendered.triangles.empty() && rendered.materials[0].principled.z == 0.75f,
          "resource-cooked scene integrates with ordinary ECS render extraction");
}

void WorldMoveAndReplacementInvalidateHandles() {
    gameplay::World source;
    source.CreateScene(TexturedScene());
    const auto handle = source.MaterialHandle(0);
    const auto before = source.ExtractRenderSnapshot();
    gameplay::World destination;
    destination.CreateScene(render::BuildCornellScene());
    const auto replacedHandle = destination.MaterialHandle(0);
    destination = std::move(source);
    Check(destination.Resources().Resolve(handle) != nullptr, "world move preserves transferred resource handles");
    Check(destination.Resources().Resolve(replacedHandle) == nullptr, "world move releases the previous destination resource set");
    destination.Materials()[0].baseRoughness.x = 0.123f;
    destination.MarkDirty();
    Check(destination.Resources().Resolve(handle)->Parameters().baseRoughness.x == 0.123f,
          "material projection remains connected after world move assignment");
    Check(before.materials[0].baseRoughness.x != 0.123f, "old snapshots remain isolated from resource edits");
    destination.CreateScene(render::BuildCornellScene());
    Check(destination.Resources().Resolve(handle) == nullptr, "replacing the scene invalidates previous resource handles");
}

void CornellCookingFollowsExplicitWorldMode() {
    const auto source = render::BuildCornellScene();
    gameplay::World generic;
    generic.CreateScene(source);
    // The source transport marker cannot opt a generic World into special emitter ownership during cooking.
    Check(!generic.CookResourceScene().cornell, "generic world cooking must retain its explicit non-Cornell mode");
    gameplay::World cornell;
    cornell.CreateCornell(source);
    Check(cornell.CookResourceScene().cornell, "explicit Cornell world cooking must retain its emitter convention");
}

#ifdef _WIN32
void FreshImportBecomesResourceBackedRenderingData() {
    // Exercise the real cgltf/WIC importer instead of manufacturing an already-cooked transport record.
    const auto imported = render::ImportGltf(std::filesystem::path(__FILE__).parent_path() / "fixtures/textured_multilight.gltf");
    gameplay::World world;
    world.CreateScene(imported);
    Check(world.Resources().Size() == imported.materials.size() + 2,
          "real import shares its image between material uses and owns one baked multisurface mesh");
    const auto cooked = world.CookResourceScene();
    const auto rendered = render::ExtractRenderScene(cooked, world.ExtractRenderSnapshot());
    Check(cooked.triangles.size() == imported.triangles.size() && cooked.attributes.size() == imported.attributes.size() &&
          cooked.packedTexels == imported.packedTexels && cooked.meshParts.size() == imported.meshParts.size(),
          "fresh imported geometry, attributes, pixels and named surfaces survive resource cooking");
    Check(rendered.lights.size() == 3 && world.Resources().Resolve(world.MaterialHandle(0))->Name() == "Textured Ground",
          "resource-backed import preserves named materials and all authored analytic lights");
}
#endif
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"explicit ownership and stale handles", ExplicitOwnershipAndStaleHandles},
        {"resource-owned render inputs", WorldResourcesOwnCookedInputs},
        {"resource world move and replacement", WorldMoveAndReplacementInvalidateHandles},
        {"explicit world Cornell cooking mode", CornellCookingFollowsExplicitWorldMode},
#ifdef _WIN32
        {"fresh import through resources", FreshImportBecomesResourceBackedRenderingData},
#endif
    };
    unsigned failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
