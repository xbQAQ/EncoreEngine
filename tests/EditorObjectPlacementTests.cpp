#include "editor/EditorObjectPalette.h"
#include "editor/EditorUi.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include "render/SceneExtraction.h"
#include "dx12/DescriptorArena.h"
#include "dx12/Dx12Device.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

render::WorldDocument Presets() {
    return render::LoadWorldDocument(std::filesystem::path(RENDER_SOURCE_DIRECTORY) / "Content/Objects/basic.textproto");
}

render::Scene Wall() {
    render::Scene scene;
    scene.materials.resize(2);
    scene.materialNames = {"Wall", "Behind wall"};
    // 前方墙面的绕序朝向相机，后层用于验证透明和背面拒绝后的继续拾取。
    scene.triangles = {{{-10, -10, 4, 0}, {0, 10, 4, 0}, {10, -10, 4, 0}},
        {{-10, -10, 6, 1}, {0, 10, 6, 0}, {10, -10, 6, 0}}};
    scene.meshParts = {{"Front", 0, 1, 0}, {"Back", 1, 1, 1}};
    return scene;
}

void PickingCache() {
    auto source = Wall();
    gameplay::World world;
    world.CreateScene(source);
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    render::editor::SurfacePickCache cache;
    const render::Ray ray{{0, 0, 0}, {0, 0, 1}, .1f, 100};
    const auto first = cache.Pick(source, world, ray);
    Check(first && first->position.z == 4, "Cached picking starts at the nearest visible material");
    // 导航只改变查询射线和视图，不能失效已经接受的世界空间拾取几何。
    const auto builds = cache.SceneBuildCount();
    world.Registry().get<gameplay::Transform>(world.CameraEntity()).position.x += 1;
    world.MarkDirty();
    Check(cache.Pick(source, world, ray)->entity == first->entity && cache.SceneBuildCount() == builds,
        "Camera navigation reuses the extracted picking scene");
    world.Registry().get<gameplay::Camera>(world.CameraEntity()).verticalFovDegrees = 70;
    world.MarkDirty();
    Check(cache.Pick(source, world, ray)->position.z == 4 && cache.SceneBuildCount() == builds,
        "Camera projection changes preserve picking geometry and unchanged-ray hits");
    const auto nearPlane = world.Registry().get<gameplay::Camera>(world.CameraEntity()).nearPlane;
    world.Registry().get<gameplay::Camera>(world.CameraEntity()).nearPlane = -1;
    world.MarkDirty();
    bool rejected = false;
    try { cache.Pick(source, world, ray); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected && cache.SceneBuildCount() == builds, "Picking camera fast path retains projection validation");
    world.Registry().get<gameplay::Camera>(world.CameraEntity()).nearPlane = nearPlane;
    Check(cache.Pick(source, world, ray)->position.z == 4 && cache.SceneBuildCount() == builds,
        "A rejected picking revision can be corrected without rebuilding geometry");
    Check(!cache.Pick(source, world, {{0, 0, 5}, {0, 0, -1}, .1f, 4}) && cache.Pick(source, world, ray)->position.z == 4,
        "Backface rejection for another ray never mutates the cached immutable geometry");
    world.Materials()[0].flags.z = 1;
    world.Materials()[0].extras.w = 0;
    world.Materials()[0].extras.z = .5f;
    world.MarkDirty();
    const auto material = cache.Pick(source, world, ray);
    Check(material && material->position.z == 6, "Material edits invalidate cached visibility and unchanged-ray hits");
    world.Materials()[0].flags.z = 0;
    world.MarkDirty();
    const auto front = first->entity;
    world.Registry().get<gameplay::Transform>(front).position.z = -1;
    world.MarkDirty();
    const auto transformed = cache.Pick(source, world, ray);
    Check(transformed && transformed->position.z == 3, "Transform edits invalidate the cached extracted geometry");
    const auto added = world.CreateEntity("Additional foreground instance");
    world.Registry().get<gameplay::Transform>(added).position.z = -2;
    world.Registry().emplace<gameplay::MeshRenderer>(added, world.Registry().get<gameplay::MeshRenderer>(front));
    const auto created = cache.Pick(source, world, ray);
    Check(created && created->entity == added && created->position.z == 2,
        "Entity creation invalidates the cached source ownership and surface result");
    auto replacement = source;
    for (auto& triangle : replacement.triangles) { triangle.a.z += 10; triangle.b.z += 10; triangle.c.z += 10; }
    const auto replaced = cache.Pick(replacement, world, ray);
    Check(replaced && replaced->position.z == 12, "A different immutable source identity invalidates cached geometry");
    cache.Reset();
    const auto reset = cache.Pick(source, world, ray);
    Check(reset && reset->position.z == 2, "Scene reset clears cached extraction and ray results");
    Check(cache.Pick(source, world, {{0, 0, 0}, {0, 0, 1}, 2.5f, 100})->position.z == 3 &&
        !cache.Pick(source, world, {{0, 0, 0}, {0, 0, 1}, .1f, 1.5f}),
        "Near and far clipping changes invalidate an otherwise identical cached ray");
    Check(!cache.Pick(source, world, {{100, 0, 0}, {0, 0, 1}, .1f, 100}),
        "A changed mouse ray cannot reuse an older accepted surface");

    auto stack = Wall();
    stack.triangles.clear();
    // 多层透明遮罩必须逐个拒绝后接受后层，筛选过程不重复扫描整个三角形数组。
    for (int layer = 0; layer < 256; ++layer) {
        const float z = 1 + static_cast<float>(layer) * .01f;
        stack.triangles.push_back({{-10, -10, z, 0}, {0, 10, z, 0}, {10, -10, z, 0}});
    }
    stack.triangles.push_back({{-10, -10, 6, 1}, {0, 10, 6, 0}, {10, -10, 6, 0}});
    stack.meshParts = {{"Transparent stack", 0, 256, 0}, {"Opaque back", 256, 1, 1}};
    stack.materials[0].flags.z = 1;
    stack.materials[0].extras.w = 0;
    stack.materials[0].extras.z = .5f;
    gameplay::World layered;
    layered.CreateScene(stack);
    layered.Registry().get<gameplay::Light>(layered.LightEntity()).enabled = false;
    const auto accepted = cache.Pick(stack, layered, ray);
    Check(accepted && accepted->position.z == 6,
        "A long transparent stack still resolves the nearest accepted opaque surface");
    Check(cache.Pick(stack, layered, ray)->position.z == 6,
        "An unchanged world and ray preserve the accepted cached surface");
}

void PlacementMath() {
    gameplay::World world;
    world.CreateScene(Wall());
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    world.InstallObjectPresets(Presets());
    const auto source = world.CookResourceScene();
    const auto& presets = world.ObjectPresets();
    Check(presets.size() >= 7, "Default palette includes two primitives and all five light types");
    gameplay::Transform camera;
    const render::Ray ray{{0, 0, 0}, {0, 0, 1}, .1f, 100};
    const auto placement = render::editor::ResolveObjectPlacement(source, world, presets[0], ray, camera);
    Check(placement && placement->anchor == render::editor::PlacementAnchor::Surface &&
        std::abs(placement->contact.z - 4) < 1e-5f, "Placement accepts the nearest visible surface");
    Check(std::abs(placement->transform.position.z - 3.5f) < 1e-5f,
        "Bottom-centered cube remains fully outside a vertical wall");
    // 同一表面查询必须和既有材质拖放、点击选择接受完全相同的可见交点。
    world.Materials()[0].flags.z = 1;
    world.Materials()[0].extras.w = 0;
    world.Materials()[0].extras.z = .5f;
    const auto transparent = render::editor::ResolveObjectPlacement(source, world, presets[0], ray, camera);
    Check(transparent && std::abs(transparent->contact.z - 6) < 1e-5f,
        "Alpha-rejected foreground does not hide an accepted placement surface");
    world.Materials()[0].flags.z = 0;
    const auto clipped = render::editor::ResolveObjectPlacement(source, world, presets[0],
        {{0, 0, 0}, {0, 0, 1}, 5, 7}, camera);
    Check(clipped && std::abs(clipped->contact.z - 6) < 1e-5f,
        "Placement excludes scene geometry before the near clipping plane");
    const auto backface = render::editor::PickSurface(source, world, {{0, 0, 5}, {0, 0, -1}, .1f, 4});
    Check(!backface, "Single-sided wall backfaces do not become placement targets");
    world.Materials()[0].flags.y = 1;
    const auto doubleSided = render::editor::PickSurface(source, world, {{0, 0, 5}, {0, 0, -1}, .1f, 4});
    Check(doubleSided && doubleSided->normal.z > .9f,
        "Double-sided placement normal faces the incoming ray");
    auto configuredLibrary = Presets();
    auto* defaults = configuredLibrary.mutable_levels(0)->mutable_scenes(0)->mutable_objects(0)->mutable_transform();
    defaults->mutable_rotation_degrees()->set_y(90);
    defaults->mutable_scale()->set_x(2);
    defaults->mutable_position()->set_x(.25f);
    defaults->mutable_position()->set_y(.3f);
    gameplay::World configured;
    configured.CreateScene(Wall());
    configured.InstallObjectPresets(configuredLibrary);
    const auto rotated = render::editor::ResolveObjectPlacement(configured.CookResourceScene(), configured,
        configured.ObjectPresets()[0], ray, camera);
    Check(rotated && std::abs(rotated->transform.position.z - 3) < 1e-5f && rotated->transform.rotationDegrees.y == 90 &&
        rotated->transform.scale.x == 2 && rotated->transform.position.x == .25f && rotated->transform.position.y == .3f,
        "Configured world-axis offset, rotation and scale are included once in wall support");
    const auto configuredCube = configured.CreatePresetObject(0, rotated->transform);
    const auto& configuredPose = configured.Registry().get<gameplay::Transform>(configuredCube);
    Check(configuredPose.rotationDegrees.y == 90 && configuredPose.scale.x == 2 &&
        render::Length(configuredPose.position - rotated->transform.position) < 1e-5f,
        "Created transform exactly matches the configured placement preview");

    gameplay::World empty;
    empty.CreateScene(Wall());
    // 导入入口需要有效源几何；关闭所有实例可得到没有可命中表面的真实空白视口。
    for (auto [entity, mesh] : empty.Registry().view<gameplay::MeshRenderer>().each()) { (void)entity; mesh.visible = false; }
    empty.Registry().get<gameplay::Light>(empty.LightEntity()).enabled = false;
    empty.InstallObjectPresets(Presets());
    const auto blank = empty.CookResourceScene();
    const auto& cube = empty.ObjectPresets()[0];
    const auto ground = render::editor::ResolveObjectPlacement(blank, empty, cube,
        {{0, 2, 0}, render::Normalize({.23f, -1, 1}), .1f, 100}, camera, .5f);
    Check(ground && ground->anchor == render::editor::PlacementAnchor::Ground &&
        ground->transform.position.y == 0 && std::abs(ground->transform.position.x - .5f) < 1e-5f,
        "Blank-space ground fallback preserves bottom-centered placement and translation snapping");
    const auto groundPlane = render::editor::ResolveObjectPlacement(blank, empty, empty.ObjectPresets()[1],
        {{0, 2, 0}, render::Normalize({0, -1, 1}), .1f, 100}, camera);
    Check(groundPlane && std::abs(groundPlane->transform.position.y - .001f) < 1e-5f &&
        render::Length(groundPlane->transform.rotationDegrees) == 0,
        "Plane keeps its configured XZ orientation and avoids coplanar depth flicker");
    const auto forward = render::editor::ResolveObjectPlacement(blank, empty, cube, ray, camera);
    Check(forward && forward->anchor == render::editor::PlacementAnchor::Camera &&
        std::abs(forward->transform.position.z - 3) < 1e-5f,
        "Parallel ground ray falls back to the camera-forward plane at three meters");
    Check(!render::editor::ResolveObjectPlacement(blank, empty, cube,
        {{0, 0, 0}, {0, 0, 1}, .1f, 2}, camera),
        "Fallback never crosses the camera far clipping plane");
    const auto light = render::editor::ResolveObjectPlacement(source, world, presets[2], ray, camera);
    Check(light && std::abs(light->transform.position.z - 3.85f) < 1e-5f,
        "Light placement lifts the source fifteen centimeters off the accepted wall");
}

class HiddenWindow {
public:
    HiddenWindow() {
        // 使用真实 Win32 后端窗口，鼠标事件仅注入独立 ImGui 上下文。
        handle_ = CreateWindowExW(0, L"STATIC", L"Object placement", WS_OVERLAPPEDWINDOW,
            0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(handle_ != nullptr, "Create hidden object placement window");
    }
    ~HiddenWindow() { if (handle_) DestroyWindow(handle_); }
    HWND Handle() const { return handle_; }
private:
    HWND handle_{}; // 编辑器及其后端销毁以后释放的隐藏窗口。
};

render::editor::ViewportRect ImageRect(ImTextureID texture) {
    const auto* viewport = ImGui::FindWindowByName("###Viewport");
    Check(viewport != nullptr, "Production viewport exists");
    float left = std::numeric_limits<float>::infinity(), top = left;
    float right = -left, bottom = -left;
    // 直接读取生产视口的图像顶点，测试不绑定工具栏高度和停靠比例。
    for (const auto& command : viewport->DrawList->CmdBuffer) {
        if (command.GetTexID() != texture) continue;
        for (unsigned index = 0; index < command.ElemCount; ++index) {
            const auto point = viewport->DrawList->VtxBuffer[command.VtxOffset + viewport->DrawList->IdxBuffer[command.IdxOffset + index]].pos;
            left = std::min(left, point.x); top = std::min(top, point.y);
            right = std::max(right, point.x); bottom = std::max(bottom, point.y);
        }
    }
    Check(std::isfinite(left) && right > left && bottom > top, "Production scene image has usable bounds");
    return {left, top, right - left, bottom - top};
}

void NativeInteraction() {
    HiddenWindow window;
    render::dx12::Dx12Device device(true);
    render::dx12::DescriptorArena descriptors(device);
    render::editor::EditorUi editor(window.Handle(), device, descriptors, false);
    device.BeginCommands();
    editor.CreateGpuObjects();
    device.SubmitAndWait();
    gameplay::World world;
    world.CreateScene(Wall());
    world.Registry().get<gameplay::Transform>(world.CameraEntity()) = {};
    world.Registry().get<gameplay::Camera>(world.CameraEntity()).verticalFovDegrees = 60;
    world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
    world.InstallObjectPresets(Presets());
    const auto source = world.CookResourceScene();
    render::RenderSettings settings;
    auto& io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = {1280, 900};
    const auto frame = [&](ImVec2 mouse, bool pressed, bool right = false) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
        io.DeltaTime = 1.0f / 60;
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        editor.Build(settings, world, source, static_cast<unsigned>(io.DisplaySize.x), static_cast<unsigned>(io.DisplaySize.y),
            512, 512, 0, 1.0f / 60, {});
        editor.EndFrame();
    };
    const auto shortcut = [&](ImGuiKey key) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(key, true);
        frame({-100, -100}, false);
        io.AddKeyEvent(key, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame({-100, -100}, false);
    };
    frame({-100, -100}, false); frame({-100, -100}, false); frame({-100, -100}, false);
    auto* palette = ImGui::FindWindowByName("###ObjectPalette");
    Check(palette && palette->DockNode && !palette->Hidden && !palette->SkipItems,
        "Common objects panel is a separate visible docked window");
    const auto image = ImageRect(static_cast<ImTextureID>(descriptors.Gpu(1).ptr));
    const ImVec2 target{image.x + image.width * .5f, image.y + image.height * .5f};
    const auto row = [&](std::uint32_t index) {
        // 从真实悬停 ID 找到稳定预设行，随后完整经过原生按压、越过阈值和释放流程。
        const auto id = ImHashStr("##PresetRow", 0, ImHashData(&index, sizeof(index), palette->ID));
        ImGui::SetScrollY(palette, 0);
        frame({-100, -100}, false); frame({-100, -100}, false);
        for (int page = 0; page < 8; ++page) {
            for (float y = palette->Pos.y + 35; y < palette->Pos.y + palette->Size.y - 5; y += 4) {
                const ImVec2 point{palette->Pos.x + palette->Size.x * .5f, y};
                frame(point, false);
                if (ImGui::GetHoveredID() == id) return point;
            }
            if (palette->Scroll.y >= palette->ScrollMax.y) break;
            // 短窗口通过真实滚轮继续查找，不能假定七个预设同时显示。
            io.AddMouseWheelEvent(0, -4);
            frame({palette->Pos.x + palette->Size.x * .5f, palette->Pos.y + palette->Size.y * .5f}, false);
        }
        throw std::runtime_error("Native preset row is visible and mouse-addressable");
    };
    const auto meshCount = world.Registry().view<gameplay::MeshRenderer>().size();
    const auto lightCount = world.Registry().view<gameplay::Light>().size();
    const auto cameraBefore = world.Registry().get<gameplay::Transform>(world.CameraEntity());
    const auto drag = [&](std::uint32_t index, ImVec2 destination, bool cancel = false) {
        const auto start = row(index);
        const auto entityCount = world.Registry().view<gameplay::Name>().size();
        frame(start, true);
        frame({start.x + 12, start.y}, true);
        frame(destination, true, true);
        frame(destination, true);
        Check(!ImGuizmo::IsUsingAny(), "Preset drag never starts a transform manipulation");
        Check(world.Registry().view<gameplay::Name>().size() == entityCount,
            "Hovering a dragged preset only previews and never creates an entity");
        const auto currentImage = ImageRect(static_cast<ImTextureID>(descriptors.Gpu(1).ptr));
        if (currentImage.Contains(destination.x, destination.y)) {
            const auto* viewport = ImGui::FindWindowByName("###Viewport");
            Check(std::any_of(viewport->DrawList->VtxBuffer.begin(), viewport->DrawList->VtxBuffer.end(),
                [](const auto& vertex) { return vertex.col == IM_COL32(100, 230, 165, 255); }),
                "Native preset drag renders a nonempty placement preview in the viewport");
        }
        if (cancel) { io.AddKeyEvent(ImGuiKey_Escape, true); frame(destination, true); }
        frame(destination, false);
        if (cancel) io.AddKeyEvent(ImGuiKey_Escape, false);
        frame(destination, false); frame(destination, false);
    };
    drag(0, target, true);
    drag(0, {-30, -30});
    Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount,
        "Escape and dropping outside the viewport create no object");
    drag(0, target);
    Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 1,
        "Real palette row drag creates exactly one cube on release");
    Check(render::Length(world.Registry().get<gameplay::Transform>(world.CameraEntity()).position - cameraBefore.position) == 0 &&
        render::Length(world.Registry().get<gameplay::Transform>(world.CameraEntity()).rotationDegrees - cameraBefore.rotationDegrees) == 0,
        "Object drag blocks simultaneous camera navigation");
    const auto extracted = render::ExtractRenderScene(source, world.ExtractRenderSnapshot());
    const auto visibleTriangles = std::count_if(extracted.triangles.begin(), extracted.triangles.end(), [](const auto& triangle) {
        const render::Vec3 a{triangle.a.x, triangle.a.y, triangle.a.z};
        const render::Vec3 b{triangle.b.x, triangle.b.y, triangle.b.z}, c{triangle.c.x, triangle.c.y, triangle.c.z};
        return render::Length(render::Cross(b - a, c - a)) > .00001f;
    });
    Check(visibleTriangles == 14, "Dropped cube activates its reserved geometry in production render extraction");
    shortcut(ImGuiKey_Z);
    Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount,
        "One undo removes the entire creation transaction");
    shortcut(ImGuiKey_Y);
    Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 1,
        "Redo restores the created cube and its entity identity");
    gameplay::Entity cube{entt::null};
    for (const auto entity : world.Registry().view<gameplay::MeshRenderer>())
        if (world.Registry().get<gameplay::MeshRenderer>(entity).firstTriangle == world.ObjectPresets()[0].firstTriangle) cube = entity;
    Check(world.Registry().valid(cube), "Created cube retains its immutable preset geometry range");
    const auto cubeDefault = world.Registry().get<gameplay::Transform>(cube);
    world.Registry().get<gameplay::Transform>(cube).position.x += 7;
    world.MarkDirty();
    auto* details = ImGui::FindWindowByName("###Details");
    Check(details && details->DockNode && details->DockNode->TabBar, "Created object has a native Details panel");
    const auto activatePanel = [&](const ImGuiWindow* panel) {
        Check(panel && panel->DockNode && panel->DockNode->TabBar, "Panel owns a native dock tab");
        for (const auto& tab : panel->DockNode->TabBar->Tabs) if (tab.ID == panel->TabId) {
            const auto* bar = panel->DockNode->TabBar;
            const ImVec2 tabCenter{bar->BarRect.Min.x + tab.Offset + tab.Width * .5f, bar->BarRect.GetCenter().y};
            frame(tabCenter, false); frame(tabCenter, true); frame(tabCenter, false); frame(tabCenter, false);
            return;
        }
        throw std::runtime_error("Native panel tab is discoverable");
    };
    activatePanel(details);
    // 重做必须恢复选择，详情重置则证明默认值捕获的是放置结果而非原点或后来改动。
    ImGui::ActivateItemByID(details->GetID("###Reset Object"));
    frame({-100, -100}, false); frame({-100, -100}, false);
    Check(render::Length(world.Registry().get<gameplay::Transform>(cube).position - cubeDefault.position) < 1e-5f,
        "Redo restores selection and Details reset uses the creation-time placement baseline");
    shortcut(ImGuiKey_Z);
    Check(std::abs(world.Registry().get<gameplay::Transform>(cube).position.x - cubeDefault.position.x - 7) < 1e-5f,
        "Resetting a created object remains a separate undo transaction");
    shortcut(ImGuiKey_Y);
    drag(1, {target.x + 25, target.y + 15});
    for (std::uint32_t index = 2; index < world.ObjectPresets().size(); ++index) drag(index, target);
    Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 2 &&
        world.Registry().view<gameplay::Light>().size() == lightCount + 5,
        "Native drops create plane plus all five supported light types");
    const auto exported = world.ExportDocument();
    gameplay::World restored;
    restored.CreateDocument(exported);
    Check(restored.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 2 &&
        restored.Registry().view<gameplay::Light>().size() == lightCount + 5,
        "World document round trip preserves every palette-created object");
    while (world.ExtractRenderSnapshot().lights.size() < 16) world.CreatePresetObject(3, {});
    frame({-100, -100}, false);
    const auto countAtLimit = world.Registry().view<gameplay::Name>().size();
    drag(3, target);
    Check(world.Registry().view<gameplay::Name>().size() == countAtLimit && world.ExtractRenderSnapshot().lights.size() == 16,
        "Dropping a seventeenth enabled light leaves no partial entity");
    const auto errorColor = ImGui::ColorConvertFloat4ToU32({1, .42f, .36f, 1});
    Check(std::any_of(palette->DrawList->VtxBuffer.begin(), palette->DrawList->VtxBuffer.end(),
        [errorColor](const auto& vertex) { return vertex.col == errorColor; }),
        "Backend creation failure remains visibly rendered in the palette");

    // 使用正式布局菜单重建窄窗口默认分区，并验证被滚动隐藏的末尾光源仍可拖动。
    io.DisplaySize = {900, 720};
    // 按正式编辑器的稳定标识定位面板，确保改名后交互回归仍覆盖真实窗口。
    auto* sidebar = ImGui::FindWindowByName("###EncoreEngine");
    activatePanel(sidebar);
    ImGui::ActivateItemByID(ImHashStr("###Layout", 0, ImHashStr("##MenuBar", 0, sidebar->ID)));
    frame({-100, -100}, false);
    const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
    Check(!popups.empty() && popups.back().Window, "Narrow layout reset uses the production menu");
    ImGui::ActivateItemByID(popups.back().Window->GetID("###Reset Layout"));
    frame({-100, -100}, false); frame({-100, -100}, false); frame({-100, -100}, false);
    const auto narrowImage = ImageRect(static_cast<ImTextureID>(descriptors.Gpu(1).ptr));
    Check(palette->DockNode && !palette->Hidden && !palette->SkipItems && palette->DockId != sidebar->DockId &&
        details->DockId == sidebar->DockId && narrowImage.width > 100 && narrowImage.height > 100,
        "Narrow layout preserves a visible palette, a usable viewport and shared Details dock");
    Check(palette->Pos.x >= 0 && palette->Pos.y >= 0 && palette->Pos.x + palette->Size.x <= io.DisplaySize.x &&
        palette->Pos.y + palette->Size.y <= io.DisplaySize.y,
        "Narrow object palette stays inside the client area");
    drag(6, {narrowImage.x + narrowImage.width * .5f, narrowImage.y + narrowImage.height * .5f});
    Check(world.ExtractRenderSnapshot().lights.size() == 16,
        "The final light row remains draggable after scrolling in a narrow palette");
}
}

int main() {
    try {
        PlacementMath();
        PickingCache();
        NativeInteraction();
        std::cout << "PASS object placement: accepted surfaces, clipped fallbacks, native drag/cancel, presets, history and world export\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL object placement: " << error.what() << '\n';
        return 1;
    }
}
