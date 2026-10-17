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
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class HiddenWindow {
public:
    HiddenWindow() {
        // 内建窗口类提供真实后端 HWND，不显示窗口，也不移动系统鼠标。
        handle_ = CreateWindowExW(0, L"STATIC", L"Gizmo interaction", WS_OVERLAPPEDWINDOW,
            0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(handle_ != nullptr, "Create hidden gizmo test HWND");
    }
    ~HiddenWindow() { if (handle_) DestroyWindow(handle_); }
    HWND Handle() const { return handle_; }

private:
    HWND handle_{}; // ImGui 后端销毁后才释放的隐藏父窗口。
};

render::Scene Fixture() {
    render::Scene scene;
    scene.materials.resize(2);
    scene.materialNames = {"Shared material", "Alternate material"};
    // 绕序朝向 +Z 视线相机，交互目标在生产单面剔除规则下同样可见。
    scene.triangles = {{{-1.1f, -0.5f, 3, 0}, {-0.7f, 0.5f, 3, 0}, {-0.3f, -0.5f, 3, 0}},
                       {{0.3f, -0.5f, 3, 0}, {0.7f, 0.5f, 3, 0}, {1.1f, -0.5f, 3, 0}}};
    scene.meshParts = {{"Left mesh", 0, 1, 0}, {"Right mesh", 1, 1, 0}};
    return scene;
}

render::editor::ViewportRect ImageRect(ImTextureID texture) {
    auto* viewport = ImGui::FindWindowByName("###Viewport");
    Check(viewport != nullptr, "Production viewport exists");
    float left = std::numeric_limits<float>::infinity(), top = left;
    float right = -left, bottom = -left;
    // 从真实图像绘制命令读取范围，不硬编码工具栏高度或留黑边尺寸。
    for (const auto& command : viewport->DrawList->CmdBuffer) {
        if (command.GetTexID() != texture) continue;
        for (unsigned index = 0; index < command.ElemCount; ++index) {
            const auto vertex = viewport->DrawList->VtxBuffer[command.VtxOffset + viewport->DrawList->IdxBuffer[command.IdxOffset + index]].pos;
            left = std::min(left, vertex.x); top = std::min(top, vertex.y);
            right = std::max(right, vertex.x); bottom = std::max(bottom, vertex.y);
        }
    }
    Check(std::isfinite(left) && right > left && bottom > top, "Viewport contains a nonempty scene image command");
    return {left, top, right - left, bottom - top};
}

ImVec2 Project(const render::editor::ViewportRect& image, render::Vec3 point) {
    const float tangent = std::tan(3.14159265358979323846f / 6);
    return {image.x + (1 + point.x / (point.z * tangent)) * image.width * 0.5f,
        image.y + (1 - point.y / (point.z * tangent)) * image.height * 0.5f};
}
}

int main() {
    try {
        HiddenWindow window;
        render::dx12::Dx12Device device(true);
        render::dx12::DescriptorArena descriptors(device);
        // 自动交互不读取或覆盖用户的持久化布局。
        render::editor::EditorUi editor(window.Handle(), device, descriptors, false);
        device.BeginCommands();
        editor.CreateGpuObjects();
        device.SubmitAndWait();
        // 验证图集实际包含字形，避免 UTF-8 标签静默显示为缺字符方框。
        for (const auto character : {L'编', L'辑', L'器', L'法', L'线', L'糙', L'蓄', L'已', L'重', L'命', L'名'})
            Check(ImGui::GetIO().FontDefault->FindGlyphNoFallback(static_cast<ImWchar>(character)) != nullptr,
                "Chinese editor atlas contains actual UI and authored-name glyphs");
        const auto source = Fixture();
        gameplay::World world;
        world.CreateScene(source);
        world.Registry().get<gameplay::Transform>(world.CameraEntity()) = {};
        world.Registry().get<gameplay::Camera>(world.CameraEntity()).verticalFovDegrees = 60;
        world.Registry().get<gameplay::Light>(world.LightEntity()).enabled = false;
        gameplay::Entity left{entt::null}, right{entt::null};
        for (auto [entity, mesh] : world.Registry().view<gameplay::MeshRenderer>().each())
            (mesh.firstTriangle == 0 ? left : right) = entity;
        const auto moving = world.CreateEntity("Independent simulation");
        world.Registry().emplace<gameplay::Velocity>(moving, gameplay::Velocity{{1, 0, 0}});
        render::RenderSettings settings;
        auto& io = ImGui::GetIO();
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize = {1000, 700};
        const auto imageTexture = static_cast<ImTextureID>(descriptors.Gpu(1).ptr);
        std::uint32_t materialPayload = ~0u;
        auto frame = [&](ImVec2 mouse, bool pressed, bool rightPressed = false, bool middlePressed = false) {
            // 合成事件只绕过系统轮询，生产 Build 和真实 ImGuizmo 状态机保持不变。
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, rightPressed);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, middlePressed);
            io.DeltaTime = 1.0f / 60;
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            ImGuizmo::BeginFrame();
            // 外部拖放源跟随正式浏览器的协议名，验证品牌迁移后仍走生产拾取与历史逻辑。
            if (materialPayload != ~0u && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
                ImGui::SetDragDropPayload("ENCORE_MATERIAL", &materialPayload, sizeof(materialPayload));
                ImGui::TextUnformatted("材质");
                ImGui::EndDragDropSource();
            }
            editor.Build(settings, world, source, static_cast<unsigned>(io.DisplaySize.x), static_cast<unsigned>(io.DisplaySize.y),
                512, 512, 0, 1.0f / 60, {});
            editor.EndFrame();
        };
        frame({-100, -100}, false);
        frame({-100, -100}, false);
        frame({-100, -100}, false);
        const auto image = ImageRect(imageTexture);
        const auto center = Project(image, {0.7f, 0, 3});
        frame(center, false);
        frame(center, true);
        frame(center, false);
        frame(center, false);

        // 拖动所选右网格的中心平面手柄，左网格必须保持不变。
        const auto before = world.Registry().get<gameplay::Transform>(right);
        const auto cameraBefore = world.Registry().get<gameplay::Transform>(world.CameraEntity());
        const auto revision = world.Revision();
        settings.reset = false;
        frame(center, true);
        Check(ImGuizmo::IsUsingAny(), "Mouse press captures the selected entity's gizmo");
        const ImVec2 dragged(center.x + 45, center.y - 20);
        frame(dragged, true, true);
        const auto after = world.Registry().get<gameplay::Transform>(right);
        Check(render::Length(after.position - before.position) > 0.05f, "Real gizmo drag changes the viewport-picked mesh");
        Check(render::Length(world.Registry().get<gameplay::Transform>(left).position) == 0, "Viewport selection does not manipulate another entity");
        const auto cameraAfter = world.Registry().get<gameplay::Transform>(world.CameraEntity());
        Check(render::Length(cameraAfter.position - cameraBefore.position) == 0 &&
            render::Length(cameraAfter.rotationDegrees - cameraBefore.rotationDegrees) == 0, "Gizmo capture blocks simultaneous right-button camera input");
        Check(world.Revision() > revision && settings.reset, "Manipulation invalidates render snapshot and accumulated history");
        world.Update(0.1f);
        const auto simulated = world.Registry().get<gameplay::Transform>(moving).position;
        frame(dragged, false);
        Check(!ImGuizmo::IsUsingAny(), "Mouse release ends transform capture");

        // 场景撤销必须消费真实快捷键，并将完整鼠标拖动合为一笔事务。
        const auto shortcut = [&](ImGuiKey key, bool shift = false) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiMod_Shift, shift);
            io.AddKeyEvent(key, true);
            frame(dragged, false);
            io.AddKeyEvent(key, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddKeyEvent(ImGuiMod_Shift, false);
            frame(dragged, false);
        };
        settings.reset = false;
        const auto undoRevision = world.Revision();
        shortcut(ImGuiKey_Z);
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - before.position) < 1e-5f,
            "Ctrl+Z restores the complete gizmo drag in one step");
        Check(world.Revision() > undoRevision && settings.reset, "Undo invalidates the world and GI history");
        Check(render::Length(world.Registry().get<gameplay::Transform>(moving).position - simulated) < 1e-5f,
            "Gizmo undo must not rewind another entity's simulation between mouse frames");
        shortcut(ImGuiKey_Y);
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - after.position) < 1e-5f,
            "Ctrl+Y replays the gizmo transaction");
        shortcut(ImGuiKey_Z);
        shortcut(ImGuiKey_Z, true);
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - after.position) < 1e-5f,
            "Ctrl+Shift+Z also replays a scene transaction");

        // Q 隐藏手柄；右键导航期间 W/E/Q 用于移动，不切换变换工具。
        const auto tap = [&](ImGuiKey key) {
            io.AddKeyEvent(key, true);
            frame(dragged, false);
            io.AddKeyEvent(key, false);
            frame(dragged, false);
        };
        tap(ImGuiKey_Q);
        const auto movedCenter = Project(image, render::Vec3{0.7f, 0, 3} + after.position);
        frame(movedCenter, false);
        frame(movedCenter, true);
        Check(!ImGuizmo::IsUsingAny(), "UE Q selection mode hides transform handles");
        frame(movedCenter, false);
        tap(ImGuiKey_W);
        frame(dragged, false, true);
        io.AddKeyEvent(ImGuiKey_W, true);
        io.AddKeyEvent(ImGuiKey_E, true);
        frame({dragged.x + 8, dragged.y + 3}, false, true);
        io.AddKeyEvent(ImGuiKey_W, false);
        io.AddKeyEvent(ImGuiKey_E, false);
        frame(dragged, false);
        const auto navigated = world.Registry().get<gameplay::Transform>(world.CameraEntity());
        Check(render::Length(navigated.position - cameraBefore.position) > 0.02f && navigated.position.y > cameraBefore.position.y,
            "RMB W/E fly forward and upward in world space");
        shortcut(ImGuiKey_Z);
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - before.position) < 1e-5f &&
            render::Length(world.Registry().get<gameplay::Transform>(world.CameraEntity()).position - navigated.position) < 1e-5f,
            "Camera navigation creates no scene undo entry and survives undoing the preceding object drag");
        shortcut(ImGuiKey_Y);
        world.Registry().get<gameplay::Transform>(world.CameraEntity()) = cameraBefore;
        world.MarkDirty();

        // F 和 Alt 导航只移动相机，不修改所选对象的作者位姿。
        tap(ImGuiKey_F);
        const auto focusedPose = world.Registry().get<gameplay::Transform>(world.CameraEntity());
        Check(render::Length(focusedPose.position - cameraBefore.position) > 0.05f, "UE F focuses selected geometry");
        io.AddKeyEvent(ImGuiMod_Alt, true);
        frame(dragged, true);
        frame({dragged.x + 30, dragged.y + 10}, true);
        Check(render::Length(world.Registry().get<gameplay::Transform>(world.CameraEntity()).position - focusedPose.position) > 0.01f,
            "Alt LMB navigates even when initiated over the selected object's handles");
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - after.position) < 1e-5f,
            "Alt orbit does not start a transform drag or modify the selected entity");
        frame(dragged, false);
        io.AddKeyEvent(ImGuiMod_Alt, false);
        frame(dragged, false);
        world.Registry().get<gameplay::Transform>(world.CameraEntity()) = cameraBefore;
        world.MarkDirty();

        // 复制与删除共用场景历史，撤销恢复精确的带代际选择。
        const auto meshCount = world.Registry().view<gameplay::MeshRenderer>().size();
        world.Registry().get<gameplay::Camera>(world.CameraEntity()).active = false;
        shortcut(ImGuiKey_D);
        Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 1, "Ctrl+D duplicates the selected mesh resource instance");
        const auto duplicatedScene = render::ExtractRenderScene(source, world.ExtractRenderSnapshot());
        Check(duplicatedScene.triangles.size() > source.triangles.size(), "A duplicate reaches the real render extraction without overlapping source ownership");
        tap(ImGuiKey_Delete);
        Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount, "Delete removes the selected duplicate");
        shortcut(ImGuiKey_Z);
        Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount + 1, "Ctrl+Z restores a deleted entity");
        shortcut(ImGuiKey_Z);
        Check(world.Registry().view<gameplay::MeshRenderer>().size() == meshCount, "Next Ctrl+Z reverses the original duplicate operation");
        shortcut(ImGuiKey_Y);
        shortcut(ImGuiKey_Y);
        world.Registry().get<gameplay::Camera>(world.CameraEntity()).active = true;

        // 独立窗口每帧分别捕获，详情中的多帧文本编辑仍只能形成一笔事务。
        frame(movedCenter, false);
        frame(movedCenter, true);
        frame(movedCenter, false);
        auto* sidebar = ImGui::FindWindowByName("###EncoreEngine");
        Check(sidebar != nullptr, "Localized editor sidebar is discoverable by its stable ID");
        auto* worldWindow = ImGui::FindWindowByName("###World");
        const auto dockTabCenter = [&](const ImGuiWindow* panel) {
            Check(panel && panel->DockNode && panel->DockNode->TabBar, "Independent editor window has a native dock tab");
            const auto* tabs = panel->DockNode->TabBar;
            for (const auto& tab : tabs->Tabs) if (tab.ID == panel->TabId)
                return ImVec2{tabs->BarRect.Min.x + tab.Offset + tab.Width * .5f, tabs->BarRect.GetCenter().y};
            throw std::runtime_error("Native editor dock tab is discoverable");
        };
        const auto activatePanel = [&](const ImGuiWindow* panel) {
            // DisplaySize 或分隔比例刚变化时，先提交一帧布局，再读取移动后的真实标签坐标。
            frame({-100, -100}, false);
            const auto center = dockTabCenter(panel);
            frame(center, false); frame(center, true); frame(center, false); frame(center, false);
            Check(panel->DockNode->SelectedTabId == panel->TabId && !panel->SkipItems,
                "Clicking the current native dock tab activates the requested panel");
        };
        Check(worldWindow && worldWindow->DockId == sidebar->DockId,
            "World is an independent dockable window sharing the default rendering dock group");
        const auto worldTab = dockTabCenter(worldWindow);
        activatePanel(worldWindow);
        auto* details = ImGui::FindWindowByName("###Details");
        Check(details != nullptr, "Persistent Details panel exists independently of the World tab");
        const auto nameId = details->GetID("###Name");
        const auto originalName = world.Registry().get<gameplay::Name>(right).value;
        ImGui::ActivateItemByID(nameId);
        frame(worldTab, false);
        Check(ImGui::GetActiveID() == nameId, "Production Name input receives native keyboard focus");
        shortcut(ImGuiKey_A);
        io.AddInputCharactersUTF8("已重命名");
        frame(worldTab, false);
        tap(ImGuiKey_Home);
        io.AddInputCharactersUTF8("X");
        frame(worldTab, false);
        Check(world.Registry().get<gameplay::Name>(right).value == "X已重命名", "Native UTF-8 text input changes the selected name");
        shortcut(ImGuiKey_Z);
        Check(world.Registry().get<gameplay::Name>(right).value == "已重命名", "Ctrl+Z within Name undoes only its last text insertion");
        tap(ImGuiKey_Enter);
        frame(worldTab, false);
        shortcut(ImGuiKey_Z);
        Check(world.Registry().get<gameplay::Name>(right).value == originalName, "Scene Ctrl+Z undoes the accepted rename as one interaction");
        Check(render::Length(world.Registry().get<gameplay::Transform>(right).position - after.position) < 1e-5f,
            "Local text undo and scene rename undo preserve the preceding object transform");
        shortcut(ImGuiKey_Y);
        Check(world.Registry().get<gameplay::Name>(right).value == "已重命名", "Accepted UTF-8 rename redoes through the scene history");

        // 导航激活在本帧仍持有按钮，下一帧立即撤销必须先消费其释放状态。
        ImGui::ActivateItemByID(details->GetID("###Reset Object"));
        frame(worldTab, false);
        Check(world.Registry().get<gameplay::Name>(right).value == originalName &&
            render::Length(world.Registry().get<gameplay::Transform>(right).position) < 1e-5f,
            "Details reset restores the captured object baseline");
        Check(ImGui::GetActiveID() == details->GetID("###Reset Object"),
            "Keyboard-activated reset retains its active ID until the next widget submission");
        shortcut(ImGuiKey_Z);
        Check(world.Registry().get<gameplay::Name>(right).value == "已重命名" &&
            render::Length(world.Registry().get<gameplay::Transform>(right).position - after.position) < 1e-5f,
            "Object reset undoes independently of the preceding text interaction");

        // 材质恢复操作只恢复作者参数，不改变对象分配；它同样拥有独立撤销事务。
        const auto rightMaterial = world.Registry().get<gameplay::MeshRenderer>(right).materialIndex;
        const float baselineRoughness = world.Materials()[rightMaterial].baseRoughness.w;
        world.Materials()[rightMaterial].baseRoughness.w = 0.123f;
        world.MarkDirty();
        frame(worldTab, false);
        const auto materialResetId = ImHashStr("###Reset Material", 0, ImHashStr("SharedMaterial", 0, details->ID));
        ImGui::ActivateItemByID(materialResetId);
        frame(worldTab, false);
        Check(world.Materials()[rightMaterial].baseRoughness.w == baselineRoughness,
            "Details material reset restores the imported material baseline");
        shortcut(ImGuiKey_Z);
        Check(world.Materials()[rightMaterial].baseRoughness.w == 0.123f,
            "Material reset is a separate undoable property transaction");

        // 向视口拖放只替换鼠标下对象的材质，并能通过统一历史恢复原分配。
        const auto otherCenter = Project(image, {-0.7f, 0, 3});
        tap(ImGuiKey_Q);
        materialPayload = 1;
        frame(otherCenter, true);
        frame(otherCenter, false);
        materialPayload = ~0u;
        frame(otherCenter, false);
        Check(world.Registry().get<gameplay::MeshRenderer>(left).materialIndex == 1 &&
            world.Registry().get<gameplay::MeshRenderer>(right).materialIndex == rightMaterial,
            "Viewport material drop changes only the visible object under the pointer");
        shortcut(ImGuiKey_Z);
        Check(world.Registry().get<gameplay::MeshRenderer>(left).materialIndex == 0,
            "Viewport material assignment participates in scene undo");
        tap(ImGuiKey_W);
        // 点击另一网格会更新后续变换所用的共享选择。
        frame(otherCenter, false);
        frame(otherCenter, true);
        frame(otherCenter, false);
        frame(otherCenter, false);
        frame(otherCenter, true);
        Check(ImGuizmo::IsUsingAny(), "Picking another visible triangle moves the gizmo to its owner");
        frame({otherCenter.x - 35, otherCenter.y}, true);
        Check(render::Length(world.Registry().get<gameplay::Transform>(left).position) > 0.05f, "Second picked entity receives its own drag");
        editor.ClearSelection();
        Check(!ImGuizmo::IsUsingAny(), "Scene replacement clearing cancels an active toolkit drag");
        frame(otherCenter, false);

        // 常驻场景按钮通过真实 ImGui 激活，并在帧结束前保持当前世界不变。
        activatePanel(sidebar);
        const auto sceneRevision = world.Revision();
        ImGui::ActivateItemByID(sidebar->GetID("###Choose Scene"));
        frame(otherCenter, false);
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        Check(!popups.empty() && popups.back().Window != nullptr,
            "Persistent scene selector opens a real project scene popup");
        auto* scenePopup = popups.back().Window;
        ImGui::ActivateItemByID(scenePopup->GetID("material-lab.textproto"));
        frame(otherCenter, false);
        const auto sceneRequest = editor.ConsumeWorldRequest();
        Check(sceneRequest && sceneRequest->kind == render::editor::WorldUiRequestKind::Open &&
            std::filesystem::path(sceneRequest->path).filename() == L"material-lab.textproto" &&
            world.Revision() == sceneRevision && !editor.ConsumeWorldRequest(),
            "Choosing a project scene queues one world open without changing the traversed registry");
        gameplay::World selectedWorld;
        selectedWorld.CreateDocument(render::LoadWorldDocument(sceneRequest->path));
        Check(selectedWorld.ActiveSceneId() == "material-lab",
            "The scene selector request resolves to the shipped material laboratory world");

        // 关闭选择器必须保留当前世界，也不能重放上一次已消费的打开请求。
        ImGui::ActivateItemByID(sidebar->GetID("###Choose Scene"));
        frame(otherCenter, false);
        tap(ImGuiKey_Escape);
        // 合成键盘激活不移动导航焦点；在菜单外点击，结束弹窗的鼠标捕获。
        frame({995, 695}, false); frame({995, 695}, true); frame({995, 695}, false);
        Check(ImGui::GetCurrentContext()->OpenPopupStack.empty(), "Clicking outside the scene selector dismisses its popup");
        Check(!editor.ConsumeWorldRequest() && world.Revision() == sceneRevision,
            "Dismissing the scene selector never loads or replays a scene");

        // 从真实停靠节点提取分隔位置，验证鼠标拖动会调整邻接面板并跨帧保留。
        auto* contentWindow = ImGui::FindWindowByName("###ContentBrowser");
        auto* viewportWindow = ImGui::FindWindowByName("###Viewport");
        auto* profileWindow = ImGui::FindWindowByName("###Profile");
        auto* resourcesWindow = ImGui::FindWindowByName("###ResourceBrowser");
        auto* materialWindow = ImGui::FindWindowByName("###MaterialScript");
        auto* gameplayWindow = ImGui::FindWindowByName("###GameplayScript");
        auto* dockspace = ImGui::DockBuilderGetNode(ImHashStr("EncoreEditorDockSpace"));
        Check(io.IniFilename == nullptr && !(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) &&
            !editor.UpdatePlatformWindows() && dockspace && dockspace->SplitAxis == ImGuiAxis_Y,
            "Production dockspace uses an isolated in-memory layout during tests");
        for (const auto* panel : {sidebar, worldWindow, details, contentWindow, viewportWindow, profileWindow,
            resourcesWindow, materialWindow, gameplayWindow})
            Check(panel && panel->DockNode && !(panel->Flags & ImGuiWindowFlags_NoMove) &&
                !(panel->DockNode->MergedFlags & (ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoUndocking)),
                "Every main editor panel permits docking, movement and resizing");
        const auto drag = [&](ImVec2 start, ImVec2 end) {
            frame(start, false); frame(start, false); frame(start, true);
            frame(end, true); frame(end, true); frame(end, false); frame(end, false);
        };
        auto* upper = dockspace->ChildNodes[0];
        auto* lower = dockspace->ChildNodes[1];
        const float originalContentHeight = contentWindow->Size.y;
        const ImVec2 horizontalSplitter{dockspace->Pos.x + dockspace->Size.x * .5f,
            (upper->Pos.y + upper->Size.y + lower->Pos.y) * .5f};
        drag(horizontalSplitter, {horizontalSplitter.x, horizontalSplitter.y - 90});
        Check(contentWindow->Size.y > originalContentHeight + 70,
            "Dragging the horizontal dock splitter expands the content browser");
        const float resizedContentHeight = contentWindow->Size.y;
        frame({-100, -100}, false); frame({-100, -100}, false);
        Check(std::abs(contentWindow->Size.y - resizedContentHeight) < 1,
            "Production Build preserves the user's content height on later frames");
        Check(upper->SplitAxis == ImGuiAxis_X, "Wide default layout separates tools from viewport and details");
        const float originalSidebarWidth = sidebar->Size.x;
        const ImVec2 verticalSplitter{(upper->ChildNodes[0]->Pos.x + upper->ChildNodes[0]->Size.x + upper->ChildNodes[1]->Pos.x) * .5f,
            upper->Pos.y + upper->Size.y * .5f};
        drag(verticalSplitter, {verticalSplitter.x + 40, verticalSplitter.y});
        Check(sidebar->Size.x > originalSidebarWidth + 25,
            "Dragging a vertical dock splitter adjusts the neighboring editor panels");

        // 实际点击项目文件停靠标签，列表必须随底部区域增长，不再内嵌第二层页签。
        activatePanel(contentWindow);
        ImGuiTable* filesTable = nullptr;
        for (auto& table : ImGui::GetCurrentContext()->Tables.Buf)
            if (table.OuterWindow == contentWindow && table.ColumnsCount == 3 && (table.Flags & ImGuiTableFlags_ScrollY))
                filesTable = &table;
        Check(filesTable && filesTable->OuterRect.GetHeight() >= ImGui::GetTextLineHeightWithSpacing() * 5 && contentWindow->ScrollMax.y == 0,
            "The docked project file list displays multiple rows without a second outer scrollbar");

        // 标签拖出停靠树后，浮动窗口仍可通过真实角点拖动改变大小。
        const auto contentTitle = dockTabCenter(contentWindow);
        io.AddKeyEvent(ImGuiMod_Shift, true);
        drag(contentTitle, {350, 180});
        io.AddKeyEvent(ImGuiMod_Shift, false);
        frame({-100, -100}, false);
        Check(!contentWindow->DockNode, "Dragging a tab out of its dock creates a floating editor panel");
        ImGui::SetWindowPos(contentWindow, {200, 100});
        ImGui::SetWindowSize(contentWindow, {400, 260});
        frame({-100, -100}, false); frame({-100, -100}, false);
        const ImVec2 resizeCorner{contentWindow->Pos.x + contentWindow->Size.x - 3, contentWindow->Pos.y + contentWindow->Size.y - 3};
        drag(resizeCorner, {resizeCorner.x + 75, resizeCorner.y + 55});
        Check(contentWindow->Size.x > 450 && contentWindow->Size.y > 295,
            "Floating panels retain mouse-driven width and height changes");
        const auto floatingSize = contentWindow->Size;
        const std::string savedLayout(ImGui::SaveIniSettingsToMemory());

        // 恢复默认布局使用正式菜单；再次载入序列化布局可恢复浮动尺寸和分隔关系。
        const auto resetLayout = [&] {
            activatePanel(sidebar);
            const auto menuBarId = ImHashStr("##MenuBar", 0, sidebar->ID);
            ImGui::ActivateItemByID(ImHashStr("###Layout", 0, menuBarId));
            frame({-100, -100}, false);
            const auto& menus = ImGui::GetCurrentContext()->OpenPopupStack;
            Check(!menus.empty() && menus.back().Window, "Layout menu opens through the production menu bar");
            ImGui::ActivateItemByID(menus.back().Window->GetID("###Reset Layout"));
            frame({-100, -100}, false); frame({-100, -100}, false); frame({-100, -100}, false);
        };
        resetLayout();
        Check(contentWindow->DockNode && contentWindow->Size.x > 900 && std::abs(contentWindow->Size.y - originalContentHeight) < 2,
            "Reset layout restores the default docked proportions");
        ImGui::LoadIniSettingsFromMemory(savedLayout.c_str(), savedLayout.size());
        frame({-100, -100}, false); frame({-100, -100}, false); frame({-100, -100}, false);
        Check(!contentWindow->DockNode && std::abs(contentWindow->Size.x - floatingSize.x) < 1 && std::abs(contentWindow->Size.y - floatingSize.y) < 1,
            "Saved ImGui settings restore floating panels without Build overwriting their geometry");
        resetLayout();
        io.DisplaySize = {820, 620};
        resetLayout();
        Check(sidebar->DockId == details->DockId && sidebar->DockNode && sidebar->DockNode->TabBar->Tabs.Size == 5,
            "Narrow default layout keeps tools and details accessible in one resizable dock group");
        const auto narrowImage = ImageRect(imageTexture);
        Check(narrowImage.width > 100 && narrowImage.height > 100,
            "Narrow layout retains a nonempty viewport after restoring defaults");
        // 每种工具都执行真实标签拖出，避免只有最外层容器可移动而工具仍被内嵌锁住。
        io.DisplaySize = {1000, 700};
        resetLayout();
        for (auto* panel : {worldWindow, profileWindow, resourcesWindow, materialWindow, gameplayWindow}) {
            activatePanel(panel);
            const auto title = dockTabCenter(panel);
            io.AddKeyEvent(ImGuiMod_Shift, true);
            drag(title, {450, 220});
            io.AddKeyEvent(ImGuiMod_Shift, false);
            frame({-100, -100}, false);
            Check(!panel->DockNode, "Every independent tool tab can leave its dock group");
            resetLayout();
        }
        // 实际激活采样滑条，区分纯光栅、光栅间接光与光追模式的控件可见性。
        activatePanel(sidebar);
        const auto activatesControl = [&](const char* label) {
            const auto id = sidebar->GetID(label);
            ImGui::ClearActiveID();
            ImGui::ActivateItemByID(id);
            frame({-100, -100}, false);
            return ImGui::GetActiveID() == id;
        };
        const auto sampleBudget = settings.samples;
        settings.pipeline = 0;
        settings.giIntensity = 0;
        Check(!activatesControl("###SPP") && !activatesControl("###Indirect samples"),
            "Pure raster direct lighting exposes no global SPP or indirect sample slider");
        settings.giIntensity = 1;
        Check(!activatesControl("###SPP") && activatesControl("###Indirect samples"),
            "Raster SSGI exposes its dedicated indirect sample budget without global SPP");
        settings.pipeline = 1;
        for (const auto gi : {0u, 2u}) {
            settings.gi = gi;
            Check(activatesControl("###SPP") && !activatesControl("###Indirect samples"),
                "Path tracing and ReSTIR preserve their existing per-pixel sample slider");
        }
        Check(settings.samples == sampleBudget, "Changing visible sampling controls never changes the shared sample defaults");
        std::cout << "PASS production editor: gizmo transactions, Ctrl+Z/Y/Shift+Z, UE Q/W/F and RMB/Alt navigation, duplicate/delete, history reset and cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL editor gizmo interaction: " << error.what() << '\n';
        return 1;
    }
}
