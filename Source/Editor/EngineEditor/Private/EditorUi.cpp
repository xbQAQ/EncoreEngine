#include "editor/EditorUi.h"
#include "EditorLocalization.h"

#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include "dx12/Dx12Device.h"
#include "dx12/DescriptorArena.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <ImGuizmo.h>
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace render::editor {
namespace {
constexpr unsigned maximumLights = 16;
constexpr const char* lightTypes = "方向光\0点光源\0矩形光\0圆盘光\0聚光灯\0";
constexpr std::array<const char*, 5> lightNames{"方向光", "点光源", "矩形光", "圆盘光", "聚光灯"};

const char* FitNumericItem(const char* label, float minimumWidth) {
    const auto* marker = std::strstr(label, "###");
    const float textWidth = ImGui::CalcTextSize(label, marker).x;
    const float inlineWidth = ImGui::GetContentRegionAvail().x - textWidth - ImGui::GetStyle().ItemInnerSpacing.x;
    // 长中文标签移到上一行，隐藏标签仍使用原 ### 标识，保留输入和撤销状态。
    if (marker && inlineWidth < minimumWidth) {
        ImGui::TextUnformatted(label, marker);
        ImGui::SetNextItemWidth(-1);
        return marker;
    }
    ImGui::SetNextItemWidth(std::max(1.0f, std::min(ImGui::CalcItemWidth(), inlineWidth)));
    return label;
}

bool EditScalar(const char* label, float& value, float speed, float minimum, float maximum,
    const char* format = "%.3f") {
    float candidate = value;
    // 输入和拖动都先校验，再修改共享场景参数。
    if (!ImGui::DragFloat(FitNumericItem(label, 70), &candidate, speed, minimum, maximum, format, ImGuiSliderFlags_AlwaysClamp) ||
        !std::isfinite(candidate)) return false;
    value = candidate;
    return true;
}

bool EditColor(const char* label, float& red, float& green, float& blue, bool hdr = false) {
    float candidate[]{red, green, blue};
    const auto flags = ImGuiColorEditFlags_Float | (hdr ? ImGuiColorEditFlags_HDR : 0);
    if (!ImGui::ColorEdit3(FitNumericItem(label, 120), candidate, flags)) return false;
    // HDR 自发光保持非负；反射率和衰减颜色限制在单位区间。
    for (auto& value : candidate) {
        if (!std::isfinite(value)) return false;
        value = std::clamp(value, 0.0f, hdr ? 100000.0f : 1.0f);
    }
    red = candidate[0]; green = candidate[1]; blue = candidate[2];
    return true;
}

bool EditFlag(const char* label, std::uint32_t& value) {
    bool enabled = value != 0;
    if (!ImGui::Checkbox(label, &enabled)) return false;
    value = enabled ? 1u : 0u;
    return true;
}

bool EditVector(const char* label, gameplay::Vec3& value, float speed, float minimum, float maximum) {
    float edited[]{value.x, value.y, value.z};
    if (!ImGui::DragFloat3(FitNumericItem(label, 140), edited, speed, minimum, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp)) return false;
    const gameplay::Vec3 candidate{edited[0], edited[1], edited[2]};
    // 组件进入模拟或场景提取前拒绝粘贴的 NaN 与无穷值。
    if (!IsFinite(candidate)) return false;
    value = candidate;
    return true;
}

bool MaterialInspector(GpuMaterial& material) {
    bool changed = false;
    // 基础反射与自发光共同编辑所选网格引用的材质实例。
    ImGui::SeparatorText("表面");
    changed |= EditColor("基础颜色###Base color", material.baseRoughness.x, material.baseRoughness.y, material.baseRoughness.z);
    changed |= EditScalar("粗糙度###Roughness", material.baseRoughness.w, 0.005f, 0, 1);
    changed |= EditScalar("金属度###Metallic", material.emissionMetallic.w, 0.005f, 0, 1);
    changed |= EditColor("自发光###Emission", material.emissionMetallic.x, material.emissionMetallic.y, material.emissionMetallic.z, true);
    changed |= EditFlag("双面###Double sided", material.flags.y);
    int alphaMode = static_cast<int>(material.flags.z);
    if (ImGui::Combo("透明模式###Alpha mode", &alphaMode, "不透明\0遮罩\0")) {
        material.flags.z = static_cast<unsigned>(alphaMode);
        changed = true;
    }
    changed |= EditScalar("不透明度###Opacity", material.extras.w, 0.005f, 0, 1);
    if (material.flags.z == 1) changed |= EditScalar("遮罩阈值###Alpha cutoff", material.extras.z, 0.005f, 0, 1);
    changed |= EditScalar("法线强度###Normal strength", material.extras.y, 0.01f, 0, 4);

    // Disney 波瓣均为作者参数；纹理索引和逐交点内部标志不在此编辑。
    if (ImGui::TreeNodeEx("Disney 原则材质###Principled", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= EditScalar("镜面反射###Specular", material.principled.x, 0.005f, 0, 1);
        changed |= EditScalar("镜面染色###Specular tint", material.principled.y, 0.005f, 0, 1);
        changed |= EditScalar("各向异性###Anisotropy", material.principled.z, 0.005f, -0.99f, 0.99f);
        changed |= EditScalar("各向异性角###Anisotropy angle", material.extras.x, 0.01f, -3.141592654f, 3.141592654f);
        changed |= EditScalar("次表面散射###Subsurface", material.principled.w, 0.005f, 0, 1);
        changed |= EditScalar("清漆层###Clearcoat", material.transmissionIor.z, 0.005f, 0, 1);
        changed |= EditScalar("清漆光泽###Coat gloss", material.transmissionIor.w, 0.005f, 0, 1);
        changed |= EditScalar("绒毛光泽###Sheen", material.sheen.x, 0.005f, 0, 1);
        changed |= EditScalar("绒毛染色###Sheen tint", material.sheen.y, 0.005f, 0, 1);
        ImGui::TreePop();
    }
    // 薄表面透射不会创建实体参与介质。
    if (ImGui::TreeNodeEx("透射###Transmission", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= EditFlag("薄表面###Thin surface", material.flags.x);
        changed |= EditScalar("透射强度###Transmission", material.transmissionIor.x, 0.005f, 0, 1);
        changed |= EditScalar("折射率###IOR", material.transmissionIor.y, 0.005f, 1.001f, 4);
        changed |= EditScalar("薄表面平坦度###Thin flatness", material.sheen.z, 0.005f, 0, 1);
        changed |= EditScalar("漫透射###Diffuse transmission", material.sheen.w, 0.005f, 0, 1);
        changed |= EditColor("衰减颜色###Attenuation", material.medium.x, material.medium.y, material.medium.z);
        changed |= EditScalar("衰减距离###Attenuation distance", material.medium.w, 0.01f, 0, 10000);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("自定义切线###Authored tangent")) {
        gameplay::Vec3 tangent{material.tangentFrame.x, material.tangentFrame.y, material.tangentFrame.z};
        if (EditVector("切线###Tangent", tangent, 0.01f, -1, 1)) {
            material.tangentFrame.x = tangent.x; material.tangentFrame.y = tangent.y; material.tangentFrame.z = tangent.z;
            changed = true;
        }
        changed |= EditScalar("切线手性###Handedness", material.tangentFrame.w, 0.05f, -1, 1);
        ImGui::TreePop();
    }
    return changed;
}

std::array<unsigned, 2> LightCounts(const entt::registry& registry) {
    std::array<unsigned, 2> counts{};
    for (auto [entity, light] : registry.view<const gameplay::Light>().each()) {
        (void)entity;
        ++counts[0];
        if (light.enabled) ++counts[1];
    }
    return counts;
}

gameplay::Entity EmitterOwner(const entt::registry& registry, const gameplay::MeshRenderer* mesh) {
    if (!mesh || !mesh->lightOwned) return entt::null;
    // 不可变的源所有权在材质编辑后仍成立，不锁定仅共享自发光数据的其他网格。
    for (auto [entity, light] : registry.view<const gameplay::Light>().each())
        if (light.emitterMaterial != ~0u) return entity;
    return entt::null;
}

gameplay::Entity ActiveCamera(gameplay::World& world) {
    auto selected = gameplay::Entity{entt::null};
    // 与 World 不可变渲染快照中的确定性相机排序保持一致。
    for (auto [entity, transform, camera] : world.Registry().view<gameplay::Transform, gameplay::Camera>().each()) {
        (void)transform;
        if (camera.active && (selected == entt::null || entt::to_integral(entity) < entt::to_integral(selected))) selected = entity;
    }
    return selected;
}

bool Rename(gameplay::Name& name) {
    std::vector<char> buffer(std::max<std::size_t>(256, name.value.size() + 128), '\0');
    std::memcpy(buffer.data(), name.value.data(), name.value.size());
    if (!ImGui::InputText("名称###Name", buffer.data(), buffer.size())) return false;
    name.value = buffer.data();
    return true;
}

void MarkEdited(RenderSettings& settings, gameplay::World& world, bool changed) {
    if (!changed) return;
    // 直接修改注册表后，用一次版本变更同时失效渲染快照与累积历史。
    world.MarkDirty();
    settings.reset = true;
}
}

EditorUi::EditorUi(HWND window, dx12::Dx12Device& device, dx12::DescriptorArena& descriptors, bool persistLayout)
    : window_(window), device_(device), descriptors_(descriptors) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    if (!context_) throw std::runtime_error("ImGui context creation failed");
    ImGui::SetCurrentContext(context_);
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // 交互编辑器允许工具跨原生窗口拖出；离屏测试不创建额外系统窗口。
    if (persistLayout) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr;
    // 新品牌使用独立用户目录，避免新版停靠标识读取旧布局；隐藏测试不覆盖个人布局。
    if (persistLayout) {
        std::array<wchar_t, 32768> localDirectory{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localDirectory.data(), static_cast<DWORD>(localDirectory.size()));
        if (length && length < localDirectory.size()) {
            const auto directory = std::filesystem::path(localDirectory.data()) / L"EncoreEngine";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (!error) {
                // 常用物体新增常驻分区时启用新版布局，保留旧版布局文件供回退。
                const auto bytes = (directory / L"editor-layout-v3.ini").u8string();
                layoutFilename_.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                io.IniFilename = layoutFilename_.c_str();
            }
        }
    }
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    // 独立原生窗口使用不透明背景，保持停靠和拖出后的面板外观一致。
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) style.Colors[ImGuiCol_WindowBg].w = 1;
    style.FrameRounding = 2;
    style.WindowMinSize = ImVec2(1, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.18f, 0.8f, 0.5f, 1);

    // 后端初始化只分配 CPU 状态，字体创建显式延迟到 RDG。
    bool platformInitialized = false;
    try {
        // 本地化标签保留稳定的 ### 标识，字体字节和字形在声明的上传阶段前准备完毕。
        ConfigureEditorLanguage();
        if (!ImGui_ImplWin32_Init(window)) throw std::runtime_error("ImGui Win32 initialization failed");
        platformInitialized = true;
        ImGui_ImplDX12_InitInfo init{};
        init.Device = device_.Native();
        init.CommandQueue = device_.Queue();
        init.NumFramesInFlight = 2;
        init.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init.UserData = &descriptors_;
        init.SrvDescriptorHeap = descriptors_.ShaderHeap();
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto& arena = *static_cast<dx12::DescriptorArena*>(info->UserData);
            *cpu = arena.VisibleCpu(0);
            *gpu = arena.Gpu(0);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
        if (!ImGui_ImplDX12_Init(&init)) throw std::runtime_error("ImGui DX12 initialization failed");
    } catch (...) {
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
        if (platformInitialized) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        throw;
    }
}

EditorUi::~EditorUi() {
    if (!context_) return;
    ImGui::SetCurrentContext(context_);
    // 所属运行时必须先等待已提交绘制完成，再销毁编辑器。
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context_);
}

void EditorUi::RequestImport() {
    std::vector<wchar_t> path(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"模型 (*.fbx;*.gltf;*.glb)\0*.fbx;*.gltf;*.glb\0FBX 模型 (*.fbx)\0*.fbx\0glTF 2.0 模型 (*.gltf;*.glb)\0*.gltf;*.glb\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"导入模型";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    cameraDragging_ = false;
    // 对话框只排队路径，宿主在当前界面帧结束后替换场景。
    if (GetOpenFileNameW(&dialog)) {
        pendingImport_ = path.data();
        importError_.clear();
    } else if (const auto error = CommDlgExtendedError(); error != 0) {
        importError_ = "打开文件对话框失败（错误码 " + std::to_string(error) + "）";
    }
}

std::wstring EditorUi::ConsumeImportRequest() {
    // 原生选择器和内容浏览器共用帧结束后的场景替换边界。
    if (!pendingImport_.empty()) return std::exchange(pendingImport_, std::wstring{});
    return content_.ConsumeImportRequest();
}

void EditorUi::SetImportError(std::string error) {
    importError_ = std::move(error);
}

void EditorUi::ClearSelection() {
    // 导入世界可能复用实体位值，宿主替换场景时须显式清除语义选择。
    selection_ = entt::null;
    cameraDragging_ = false;
    navigationMode_ = NavigationMode::None;
    navigationButton_ = -1;
    navigation_ = EditorNavigation{};
    // 资源替换属于文档边界，不能将旧实体标识或材质槽历史回放到新文档。
    history_.Clear();
    defaults_.Clear();
    defaultsReady_ = false;
    scripts_.Reset();
    // 新文档即使不含脚本，也须清除上一文档已发布的材质程序。
    scripts_.RequestMaterialCompile();
    content_.Reset();
    objectPalette_.Reset();
    // 源场景和世界可能复用地址与版本，跨文档的查询数据必须显式释放。
    surfacePicker_.Reset();
    worldUi_.Reset();
    propertyBefore_.reset();
    gizmoBefore_.reset();
    propertyActiveId_ = 0;
    propertyOwner_ = propertyFrameOwner_ = 0;
    propertyRecorded_ = false;
    CancelGizmo();
}

std::optional<WorldUiRequest> EditorUi::ConsumeWorldRequest() {
    // 菜单与内容浏览器共用宿主的加载边界，所有世界替换都发生在 ImGui 帧结束后。
    if (auto request = worldUi_.ConsumeRequest()) return request;
    auto path = content_.ConsumeWorldOpenRequest();
    if (!path.empty()) return WorldUiRequest{WorldUiRequestKind::Open, std::move(path), {}};
    return std::nullopt;
}

void EditorUi::CreateGpuObjects() {
    ImGui::SetCurrentContext(context_);
    if (gpuObjectsCreated_) return;
    if (!ImGui_ImplDX12_CreateDeviceObjects()) throw std::runtime_error("ImGui GPU object creation failed");
    gpuObjectsCreated_ = true;
}

void EditorUi::BeginFrame() {
    ImGui::SetCurrentContext(context_);
    // 防止 DX12 NewFrame 在调用方声明的图阶段外延迟上传字体。
    if (!gpuObjectsCreated_) throw std::logic_error("Editor.UploadFonts must execute before EditorUi::BeginFrame");
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // 工具包共用 ImGui 帧寿命，并记录到视口的普通叠加绘制列表。
    ImGuizmo::SetImGuiContext(context_);
    ImGuizmo::BeginFrame();
}

void EditorUi::BuildDockspace(unsigned clientWidth, unsigned clientHeight) {
    const auto dockspace = ImHashStr("EncoreEditorDockSpace");
    const auto* viewport = ImGui::GetMainViewport();
    // 只有首次启动或主动恢复布局时建立默认分区，之后大小与停靠完全交给 ImGui。
    if (std::exchange(resetLayout_, false) || !ImGui::DockBuilderGetNode(dockspace)) {
        const float width = static_cast<float>(std::max(clientWidth, 1u));
        const float height = static_cast<float>(std::max(clientHeight, 1u));
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(dockspace, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
        ImGuiID work = dockspace;
        const auto content = ImGui::DockBuilderSplitNode(work, ImGuiDir_Down,
            std::clamp(std::max(220.0f, height * 0.34f) / height, 0.25f, 0.48f), nullptr, &work);
        const float sidebarWidth = width < 1000 ? std::max(260.0f, width * 0.38f) : std::clamp(width * 0.22f, 240.0f, 300.0f);
        const auto sidebar = ImGui::DockBuilderSplitNode(work, ImGuiDir_Left,
            std::min(sidebarWidth / width, 0.48f), nullptr, &work);
        ImGuiID sidebarTools = sidebar;
        // 常用物体保持独立可见；下方保留渲染与世界页签，所有区域仍可自由停靠。
        const auto palette = ImGui::DockBuilderSplitNode(sidebarTools, ImGuiDir_Up, .58f, nullptr, &sidebarTools);
        ImGuiID details = sidebar;
        // 窄窗口将工具与详情放在同一停靠组，避免三个细栏挤压视口和中文控件。
        if (width >= 1000) details = ImGui::DockBuilderSplitNode(work, ImGuiDir_Right,
            std::clamp(width * 0.24f, 260.0f, 340.0f) / std::max(1.0f, width - sidebarWidth), nullptr, &work);
        if (width < 1000) details = sidebarTools;
        ImGui::DockBuilderDockWindow("常用物体###ObjectPalette", palette);
        ImGui::DockBuilderDockWindow("渲染###EncoreEngine", sidebarTools);
        ImGui::DockBuilderDockWindow("世界###World", sidebarTools);
        ImGui::DockBuilderDockWindow("视口###Viewport", work);
        ImGui::DockBuilderDockWindow("详情###Details", details);
        ImGui::DockBuilderDockWindow("HLSL 材质###MaterialScript", details);
        ImGui::DockBuilderDockWindow("Lua 逻辑###GameplayScript", details);
        // 文件与时间线默认共享宽底栏，所有标签都是可独立拖出的真正窗口。
        ImGui::DockBuilderDockWindow("项目文件###ContentBrowser", content);
        ImGui::DockBuilderDockWindow("场景资源###ResourceBrowser", content);
        ImGui::DockBuilderDockWindow("性能时间线###Profile", content);
        ImGui::DockBuilderFinish(dockspace);
    }
    // 宿主覆盖客户区，分隔条、标签拖放与浮动窗口使用上游停靠交互。
    ImGui::DockSpaceOverViewport(dockspace, viewport);
}

void EditorUi::Build(RenderSettings& settings, gameplay::World& world, const Scene& source, unsigned clientWidth, unsigned clientHeight,
    unsigned renderWidth, unsigned renderHeight, unsigned accumulated, float delta,
    const std::vector<std::string>& passes) {
    ImGui::SetCurrentContext(context_);
    // 实体代际拒绝删除后的旧句柄，宿主显式清除则处理整个 World 的替换。
    selection_ = GizmoTarget(world, selection_);
    if (!defaultsReady_) { defaults_.Capture(world); defaultsReady_ = true; }
    defaults_.Observe(world);
    BuildDockspace(clientWidth, clientHeight);
    const auto windowFlags = ImGuiWindowFlags_NoCollapse;

    // 面板不再逐帧覆盖位置和尺寸，浮动、停靠及持久化的分隔比例均可保留。
    ImGui::SetNextWindowSizeConstraints(ImVec2(220, 140), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("渲染###EncoreEngine", nullptr, windowFlags | ImGuiWindowFlags_MenuBar)) {
        EditMenu(settings, world);
        BeginPropertyFrame(settings, world);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        // 场景入口留在渲染窗口，文档操作仍延迟到界面帧结束。
        worldUi_.DrawSceneSelector();
        if (ImGui::Button("导入模型...###Import glTF...", ImVec2(-1, 0))) RequestImport();
        if (!importError_.empty()) ImGui::TextWrapped("导入失败：%s", importError_.c_str());
        SettingsPanel(settings, accumulated, delta, passes);
        ImGui::PopItemWidth();
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    // 世界层级使用独立的停靠标签，不再嵌在不可拖出的页签容器中。
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 160), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("世界###World", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        WorldPanel(settings, world);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    // 源面板先于视口提交，使本帧刚开始的拖动立即占有放置输入。
    objectPalette_.Draw(world, !gizmo_.dragging && !cameraDragging_);
    // 选择和视口导航先于详情执行，使相机数值在当前界面帧即可见。
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 160), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("视口###Viewport", nullptr, windowFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        Viewport(settings, world, source, renderWidth, renderHeight, delta);
    ImGui::End();
    defaults_.Observe(world);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 140), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("详情###Details", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        DetailsPanel(settings, world);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 180), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("项目文件###ContentBrowser", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        content_.DrawFilesPanel(world, selection_, settings);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 180), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("场景资源###ResourceBrowser", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        content_.DrawResourcesPanel(world, selection_, settings);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    // 脚本编辑器与详情共用作者事务机制，但各自拥有独立窗口和原生滚动区。
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("HLSL 材质###MaterialScript", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        scripts_.DrawMaterial(world, selection_);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Lua 逻辑###GameplayScript", nullptr, windowFlags)) {
        BeginPropertyFrame(settings, world);
        scripts_.DrawGameplay(world, selection_, settings);
        EndPropertyFrame(settings, world);
    }
    ImGui::End();
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 180), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("性能时间线###Profile", nullptr, windowFlags)) profiler_.Draw();
    ImGui::End();
    // 隐藏的停靠标签也继续观察源码，保证撤销、文件载入和预览延迟始终生效。
    scripts_.Observe(world);
    // 控件先更新释放状态并提交属性事务，避免上一帧按钮的 ActiveId 吞掉紧接的撤销。
    EditorShortcuts(settings, world);
}

void EditorUi::SettingsPanel(RenderSettings& settings, unsigned accumulated, float delta,
    const std::vector<std::string>& passes) {
    ImGui::TextWrapped("%s", device_.AdapterName().c_str());
    ImGui::Separator();
    // 管线选择独立于 GI；纯光栅排列不包含场景光线查询。
    int pipeline = settings.pipeline == 0 ? 0 : settings.backend == 1 ? 2 : 1;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##Pipeline", &pipeline, device_.DxrSupported() ?
        "光栅化\0软件光线追踪\0DXR 硬件光线追踪\0" : "光栅化\0软件光线追踪\0")) {
        settings.pipeline = pipeline == 0 ? 0u : 1u;
        settings.backend = pipeline == 2 ? 1u : 0u;
        settings.gi = pipeline == 0 ? 1u : 0u;
        if (pipeline == 0 && settings.shadowMode == 0) settings.shadowMode = 2;
        settings.debugView = 0;
        settings.reset = true;
    }
    ImGui::SetNextItemWidth(-1);
    int gi = static_cast<int>(settings.gi);
    if (settings.pipeline == 0) {
        int rasterGi = settings.giIntensity > 0 ? 1 : 0;
        if (ImGui::Combo("##GI", &rasterGi, "仅直接光照\0屏幕空间全局光照（SSGI）\0")) {
            settings.giIntensity = rasterGi ? 1.0f : 0.0f;
            settings.reset = true;
        }
        // 光栅直接光照使用确定性面光积分；只有 SSGI 的蒙特卡洛间接光需要采样预算。
        if (settings.giIntensity > 0) {
            int indirectSamples = static_cast<int>(settings.samples);
            if (ImGui::SliderInt("间接光采样###Indirect samples", &indirectSamples, 1, 16)) {
                settings.samples = static_cast<unsigned>(indirectSamples);
                settings.reset = true;
            }
        }
    } else if (ImGui::Combo("##GI", &gi, "路径追踪\0屏幕空间全局光照（SSGI）\0ReSTIR 全局光照\0")) {
        settings.gi = static_cast<unsigned>(gi);
        settings.reset = true;
    }
    // 路径追踪与 ReSTIR 保留每像素采样，不能将其含义套到纯光栅直接光照上。
    if (settings.pipeline != 0) {
        int samples = static_cast<int>(settings.samples);
        if (ImGui::SliderInt("每像素采样###SPP", &samples, 1, 16)) { settings.samples = static_cast<unsigned>(samples); settings.reset = true; }
    }
    if (settings.pipeline != 0 && ImGui::SliderInt("路径顶点数###Path vertices", &settings.pathVertices, 1, 10)) settings.reset = true;
    if (settings.pipeline != 0 && settings.gi == 2) {
        if (ImGui::SliderInt("候选样本数###Candidates", &settings.candidates, 1, 8)) settings.reset = true;
        if (ImGui::Checkbox("时间复用###Temporal reuse", &settings.temporalReuse)) settings.reset = true;
        if (ImGui::Checkbox("空间复用###Spatial reuse", &settings.spatialReuse)) settings.reset = true;
        if (ImGui::SliderInt("历史样本上限###History M", &settings.maxHistory, 1, 128)) settings.reset = true;
        if (ImGui::SliderInt("复用半径###Reuse radius", &settings.spatialRadius, 1, 48)) settings.reset = true;
        if (ImGui::Combo("显示视图###View", &settings.debugView, "最终画面\0蓄水池样本数 M（调试）\0蓄水池权重 W（调试）\0法线（调试）\0")) settings.reset = true;
    }
    // 仅显示控制保留传输历史，影响光传输的修改请求历史重置。
    ImGui::SeparatorText("光照与后处理");
    if (ImGui::Checkbox("阴影###Shadows", &settings.shadows)) settings.reset = true;
    ImGui::BeginDisabled(!settings.shadows);
    int shadowMode = settings.pipeline == 0 ? static_cast<int>(std::max(1u, settings.shadowMode)) - 1 : static_cast<int>(settings.shadowMode);
    if (ImGui::Combo("阴影方式###Shadow filter", &shadowMode, settings.pipeline == 0 ?
        "PCF 滤波\0PCSS 软阴影\0VSM 方差阴影\0" : "光线追踪\0PCF 滤波\0PCSS 软阴影\0VSM 方差阴影\0")) {
        settings.shadowMode = static_cast<unsigned>(shadowMode) + (settings.pipeline == 0 ? 1u : 0u);
        settings.reset = true;
    }
    if (settings.shadowMode != 0) {
        if (EditScalar("深度偏移（米）###Depth bias (m)", settings.shadowBias, 0.0001f, 0, 0.2f, "%.4f")) settings.reset = true;
        if (EditScalar("法线偏移（米）###Normal bias (m)", settings.shadowNormalBias, 0.0001f, 0, 0.2f, "%.4f")) settings.reset = true;
        if (settings.shadowMode <= 2 && EditScalar("滤波半径（像素）###Filter radius (px)", settings.shadowRadius, 0.05f, 0, 32)) settings.reset = true;
        if (settings.shadowMode == 3) {
            if (EditScalar("最小方差###Minimum variance", settings.vsmMinVariance, 0.000001f, 0.00000001f, 0.1f, "%.8f")) settings.reset = true;
            if (EditScalar("漏光抑制###Bleed reduction", settings.vsmBleedReduction, 0.005f, 0, 0.99f)) settings.reset = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::SliderFloat("间接光强度###Indirect", &settings.giIntensity, 0, 3)) settings.reset = true;
    // 屏幕效果独立启用，参数变动统一使依赖历史的光照结果失效。
    if (ImGui::CollapsingHeader("屏幕空间效果###Screen Effects")) {
        if (ImGui::Checkbox("环境遮蔽 SSAO###SSAO", &settings.ssao)) settings.reset = true;
        ImGui::BeginDisabled(!settings.ssao);
        if (EditScalar("遮蔽强度###SSAO Intensity", settings.ssaoIntensity, 0.01f, 0, 4)) settings.reset = true;
        if (EditScalar("遮蔽半径###SSAO Radius", settings.ssaoRadius, 0.01f, 0.01f, 4)) settings.reset = true;
        ImGui::EndDisabled();
        if (ImGui::Checkbox("屏幕反射 SSR###SSR", &settings.ssr)) settings.reset = true;
        ImGui::BeginDisabled(!settings.ssr);
        if (EditScalar("反射强度###SSR Intensity", settings.ssrIntensity, 0.01f, 0, 4)) settings.reset = true;
        if (EditScalar("反射距离###SSR Distance", settings.ssrDistance, 0.05f, 0.1f, 50)) settings.reset = true;
        ImGui::EndDisabled();
        if (ImGui::Checkbox("屏幕间接光 SSIL###SSIL", &settings.ssil)) settings.reset = true;
        ImGui::BeginDisabled(!settings.ssil);
        if (EditScalar("屏幕间接光强度###SSIL Intensity", settings.ssilIntensity, 0.01f, 0, 4)) settings.reset = true;
        ImGui::EndDisabled();
        if (ImGui::Checkbox("次表面散射 SSS###SSS", &settings.sss)) settings.reset = true;
        ImGui::BeginDisabled(!settings.sss);
        if (EditScalar("散射强度###SSS Intensity", settings.sssIntensity, 0.01f, 0, 4)) settings.reset = true;
        if (EditScalar("散射半径###SSS Radius", settings.sssRadius, 0.005f, 0.001f, 1)) settings.reset = true;
        ImGui::EndDisabled();
    }
    if (ImGui::Checkbox("天空###Sky", &settings.sky)) settings.reset = true;
    ImGui::BeginDisabled(!settings.sky);
    if (EditScalar("天空强度###Sky Intensity", settings.skyIntensity, 0.01f, 0, 8)) settings.reset = true;
    ImGui::EndDisabled();
    ImGui::SliderFloat("曝光补偿 EV###Exposure EV", &settings.exposure, -4, 4);
    ImGui::Checkbox("自动曝光###Auto exposure", &settings.autoExposure);
    ImGui::SliderFloat("泛光###Bloom", &settings.bloom, 0, 0.4f);
    if (ImGui::SliderFloat("介质密度###Medium density", &settings.mediumDensity, 0, 0.5f)) settings.reset = true;
    ImGui::SliderFloat("相函数 g###Phase g", &settings.phaseG, -0.8f, 0.8f);
    if (ImGui::Button("重置累积###Reset accumulation", ImVec2(-1, 0))) settings.reset = true;
    // 蓄水池 M 是光追复用统计；光栅面板不显示全局 SPP 或每帧候选统计。
    if (settings.pipeline != 0) {
        ImGui::Text(settings.gi == 1 ? "累计采样预算：%u" : "累计每像素采样：%u", accumulated);
        if (settings.gi == 2) ImGui::Text("每帧新候选：%u", settings.samples * static_cast<unsigned>(settings.candidates));
    }
    ImGui::Text("CPU 帧耗时：%.1f ms", delta * 1000);
    if (ImGui::CollapsingHeader("渲染图###Render Graph")) {
        // 未知扩展保留诊断标签，已知阶段使用编辑器专属中文名称。
        for (const auto& pass : passes) {
            const auto* translated = EditorPassName(pass);
            ImGui::BulletText("%s", translated ? translated : pass.c_str());
        }
    }
}

void EditorUi::WorldPanel(RenderSettings& settings, gameplay::World& world) {
    // 文档模块只发出请求，宿主在当前 UI 与 GPU 帧完成后执行加载或切换。
    worldUi_.Draw(world);
    if (ImGui::Button(settings.simulate ? "暂停###Pause" : "运行###Play", ImVec2(-1, 0))) settings.simulate = !settings.simulate;
    auto& registry = world.Registry();
    const auto counts = LightCounts(registry);
    ImGui::Text("启用光源：%u / %u（共 %u 个）", counts[1], maximumLights, counts[0]);
    ImGui::Combo("新光源类型###New light", &newLightType_, lightTypes);
    ImGui::BeginDisabled(counts[1] >= maximumLights);
    if (ImGui::Button("创建光源###Create Light", ImVec2(-1, 0))) {
        // 新光源在活动相机附近创建，并使用有效的发射坐标系。
        selection_ = world.CreateEntity(lightNames[static_cast<std::size_t>(newLightType_)]);
        auto& light = registry.emplace<gameplay::Light>(selection_);
        light.type = static_cast<gameplay::LightType>(newLightType_);
        const auto camera = ActiveCamera(world);
        if (camera != entt::null) {
            const auto& pose = registry.get<gameplay::Transform>(camera);
            const auto basis = MakeCameraBasis(pose.rotationDegrees.x, pose.rotationDegrees.y, pose.rotationDegrees.z);
            registry.get<gameplay::Transform>(selection_).position = pose.position + basis.forward * 3.0f;
        }
        MarkEdited(settings, world, true);
    }
    ImGui::EndDisabled();
    std::vector<gameplay::Entity> entities;
    for (const auto entity : registry.view<gameplay::Name>()) entities.push_back(entity);
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) { return entt::to_integral(a) < entt::to_integral(b); });
    if (!registry.valid(selection_)) selection_ = entt::null;

    // 实体列表填满可缩放的世界窗口，只有长列表需要内部滚动。
    ImGui::SeparatorText("实体");
    const float listHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("EntityList", ImVec2(0, listHeight), ImGuiChildFlags_Borders)) {
        for (const auto entity : entities) {
            ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
            const auto& name = registry.get<gameplay::Name>(entity).value;
            // 选择发光体代理时，详情和视口工具均重定向到其解析光源拥有者。
            if (ImGui::Selectable(name.empty() ? "（未命名）" : name.c_str(), selection_ == entity)) selection_ = GizmoTarget(world, entity);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void EditorUi::DetailsPanel(RenderSettings& settings, gameplay::World& world) {
    // 详情始终位于视口更新之后，相机导航和对象变换可在本帧显示最新数值。
    if (world.Registry().valid(selection_)) {
        ImGui::PushItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.62f));
        ImGui::BeginDisabled(!defaults_.EntityDefaults(selection_));
        if (ImGui::Button("恢复对象默认值###Reset Object", ImVec2(-1, 0)) && defaults_.ResetEntity(world, selection_)) {
            settings.reset = true;
            CancelGizmo();
        }
        ImGui::EndDisabled();
        InspectEntity(settings, world);
        ImGui::PopItemWidth();
    } else ImGui::TextDisabled("未选择对象");
    // 无选择时仍比较权威源码，保证撤销、重做和文档替换会更新材质预览。
}

void EditorUi::InspectEntity(RenderSettings& settings, gameplay::World& world) {
    auto& registry = world.Registry();
    bool changed = false;
    if (auto* name = registry.try_get<gameplay::Name>(selection_)) changed |= Rename(*name);
    const auto* mesh = registry.try_get<gameplay::MeshRenderer>(selection_);
    const auto emitter = EmitterOwner(registry, mesh);
    const bool emitterMesh = emitter != entt::null;

    if (auto* transform = registry.try_get<gameplay::Transform>(selection_)) {
        if (ImGui::CollapsingHeader("变换###Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            // 显式关联的光源拥有发光几何；光源实体本身仍提供完整位姿编辑。
            ImGui::BeginDisabled(emitterMesh);
            changed |= EditVector("位置###Position", transform->position, 0.01f, -1000, 1000);
            changed |= EditVector("旋转###Rotation", transform->rotationDegrees, 0.25f, -360, 360);
            // 光源尺寸保存在 Light 组件中，提取阶段有意忽略 Transform 缩放。
            ImGui::BeginDisabled(registry.all_of<gameplay::Light>(selection_));
            changed |= EditVector("缩放###Scale", transform->scale, 0.01f, 0.01f, 100);
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (emitterMesh) {
                const auto* owner = registry.try_get<gameplay::Name>(emitter);
                ImGui::TextWrapped("发光体所属光源：%s", owner ? owner->value.c_str() : "光源");
            }
        }
    }
    if (auto* renderer = registry.try_get<gameplay::MeshRenderer>(selection_)) {
        if (ImGui::CollapsingHeader("网格渲染器###Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            // 光源所属几何必须可见，保证光源采样与 BSDF 命中的 MIS 一致。
            ImGui::BeginDisabled(emitterMesh);
            changed |= ImGui::Checkbox("可见###Visible", &renderer->visible);
            ImGui::EndDisabled();
            ImGui::Text("三角形：起始 %u，数量 %u", renderer->firstTriangle, renderer->triangleCount);
            auto& materials = world.Materials();
            ImGui::BeginDisabled(emitterMesh || materials.empty());
            const auto materialLabel = [&](std::size_t index) {
                const auto* material = world.Material(index);
                return (material && !material->Name().empty() ? material->Name() : std::string("未命名材质")) + " [" + std::to_string(index) + "]";
            };
            // 从当前模型的不可变源范围列出材质槽，编辑其中一个不会替换其他表面。
            std::vector<std::uint32_t> surfaceSlots;
            if (const auto* resource = world.Resources().Resolve(renderer->meshResource)) {
                const auto triangles = resource->Triangles();
                for (std::size_t i = renderer->firstTriangle; i < std::size_t(renderer->firstTriangle) + renderer->triangleCount; ++i) {
                    const auto index = static_cast<std::uint32_t>(triangles[i].a.w);
                    if (std::find(surfaceSlots.begin(), surfaceSlots.end(), index) == surfaceSlots.end()) surfaceSlots.push_back(index);
                }
            }
            if (!surfaceSlots.empty() && ImGui::Checkbox("使用模型表面材质###Use Surface Materials", &renderer->useSurfaceMaterials)) {
                if (renderer->useSurfaceMaterials && std::find(surfaceSlots.begin(), surfaceSlots.end(), renderer->materialIndex) == surfaceSlots.end())
                    renderer->materialIndex = surfaceSlots.front();
                changed = true;
            }
            if (ImGui::BeginCombo("材质###Material index", materialLabel(renderer->materialIndex).c_str())) {
                for (std::size_t index = 0; index < materials.size(); ++index) {
                    if (renderer->useSurfaceMaterials && std::find(surfaceSlots.begin(), surfaceSlots.end(), index) == surfaceSlots.end()) continue;
                    const auto label = materialLabel(index);
                    if (ImGui::Selectable(label.c_str(), renderer->materialIndex == index)) {
                        if (renderer->useSurfaceMaterials) {
                            renderer->materialIndex = static_cast<std::uint32_t>(index);
                            changed = true;
                        } else (void)ApplyMaterialAsset(world, selection_, static_cast<unsigned>(index), settings);
                    }
                    if (renderer->materialIndex == index) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            // 负载仅含材质槽，不把资源指针保存到下一帧。
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("ENCORE_MATERIAL");
                    payload && payload->DataSize == sizeof(std::uint32_t)) {
                    std::uint32_t index{};
                    std::memcpy(&index, payload->Data, sizeof(index));
                    (void)ApplyMaterialAsset(world, selection_, index, settings);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::EndDisabled();
        }
        if (renderer->materialIndex < world.Materials().size() &&
            ImGui::CollapsingHeader("材质###Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("SharedMaterial");
            // 关联发光体的辐亮度在提取阶段由 Light 组件派生。
            ImGui::BeginDisabled(emitterMesh);
            if (ImGui::Button("恢复材质默认值###Reset Material", ImVec2(-1, 0)) && defaults_.ResetMaterial(world, renderer->materialIndex)) {
                settings.reset = true;
                scripts_.RequestMaterialCompile();
            }
            changed |= MaterialInspector(world.Materials()[renderer->materialIndex]);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    if (auto* camera = registry.try_get<gameplay::Camera>(selection_)) {
        if (ImGui::CollapsingHeader("相机###Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::Checkbox("启用相机###Active", &camera->active);
            changed |= ImGui::SliderFloat("垂直视场角###Vertical FOV", &camera->verticalFovDegrees, 10, 120, "%.1f");
            changed |= ImGui::DragFloat("近裁剪面###Near", &camera->nearPlane, 0.005f, 0.001f, std::max(0.001f, camera->farPlane - 0.01f), "%.3f", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("远裁剪面###Far", &camera->farPlane, 0.25f, camera->nearPlane + 0.01f, 10000, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
    }
    if (auto* light = registry.try_get<gameplay::Light>(selection_)) {
        if (ImGui::CollapsingHeader("光源###Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            int type = static_cast<int>(light->type);
            if (ImGui::Combo("类型###Type", &type, lightTypes)) {
                light->type = static_cast<gameplay::LightType>(type);
                // 导入的点状光源可能携带零尺寸，切换到面积光时恢复有效边长。
                light->width = std::max(light->width, 0.01f);
                light->height = std::max(light->height, 0.01f);
                light->outerAngleDegrees = std::clamp(light->outerAngleDegrees, 0.1f, 89.9f);
                light->innerAngleDegrees = std::clamp(light->innerAngleDegrees, 0.0f, light->outerAngleDegrees);
                changed = true;
            }
            ImGui::BeginDisabled(!light->enabled && LightCounts(registry)[1] >= maximumLights);
            changed |= ImGui::Checkbox("启用###Enabled", &light->enabled);
            ImGui::EndDisabled();
            changed |= EditColor("颜色###Color", light->color.x, light->color.y, light->color.z);
            changed |= EditScalar("强度###Intensity", light->intensity, 0.1f, 0, 100000, "%.2f");
            if (light->type == gameplay::LightType::Rectangle || light->type == gameplay::LightType::Disk) {
                changed |= EditScalar("宽度（米）###Width (m)", light->width, 0.01f, 0.01f, 1000);
                changed |= EditScalar("高度（米）###Height (m)", light->height, 0.01f, 0.01f, 1000);
            }
            if (light->type == gameplay::LightType::Point || light->type == gameplay::LightType::Spot)
                changed |= EditScalar("范围（米）###Range (m)", light->range, 0.1f, 0, 10000);
            if (light->type == gameplay::LightType::Directional)
                changed |= EditScalar("角半径（弧度）###Angular radius (rad)", light->sourceRadius, 0.0001f, 0, 0.5f, "%.4f");
            else
                changed |= EditScalar("光源半径（米）###Source radius (m)", light->sourceRadius, 0.001f, 0, 100);
            if (light->type == gameplay::LightType::Spot) {
                if (EditScalar("外锥角（度）###Outer angle (deg)", light->outerAngleDegrees, 0.1f, 0.1f, 89.9f)) {
                    light->innerAngleDegrees = std::min(light->innerAngleDegrees, light->outerAngleDegrees);
                    changed = true;
                }
                changed |= EditScalar("内锥角（度）###Inner angle (deg)", light->innerAngleDegrees, 0.1f, 0, light->outerAngleDegrees);
            }
            if (ImGui::Button("删除光源###Delete Light", ImVec2(-1, 0))) {
                // 删除完整光源实体，避免遗留孤立的模拟或位姿组件。
                world.DestroyEntity(selection_);
                selection_ = entt::null;
                CancelGizmo();
                settings.reset = true;
                return;
            }
        }
    }
    if (auto* velocity = registry.try_get<gameplay::Velocity>(selection_)) {
        if (ImGui::CollapsingHeader("速度###Velocity", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(emitterMesh);
            changed |= EditVector("线速度（米/秒）###Linear m/s", velocity->linear, 0.01f, -100, 100);
            if (ImGui::Button("移除速度组件###Remove Velocity")) { registry.remove<gameplay::Velocity>(selection_); changed = true; }
            ImGui::EndDisabled();
        }
    } else {
        ImGui::BeginDisabled(emitterMesh);
        if (ImGui::Button("添加速度组件###Add Velocity", ImVec2(-1, 0))) { registry.emplace<gameplay::Velocity>(selection_); changed = true; }
        ImGui::EndDisabled();
    }
    MarkEdited(settings, world, changed);
}

void EditorUi::EndFrame() {
    ImGui::SetCurrentContext(context_);
    ImGui::Render();
}

void EditorUi::Draw(ID3D12GraphicsCommandList* commands) const {
    ImGui::SetCurrentContext(context_);
    if (!commands) throw std::invalid_argument("Editor draw requires a command list");
    // 运行时在最终 RDG 阶段绑定目标，并声明显示图像和后备缓冲区访问。
    ID3D12DescriptorHeap* heaps[]{descriptors_.ShaderHeap()};
    commands->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commands);
}

bool EditorUi::UpdatePlatformWindows() {
    ImGui::SetCurrentContext(context_);
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) return false;
    // 即使没有额外窗口也必须更新，以完成拖出、关闭与原生交换链的生命周期操作。
    ImGui::UpdatePlatformWindows();
    return ImGui::GetPlatformIO().Viewports.Size > 1;
}

void EditorUi::DrawPlatformWindows() const {
    ImGui::SetCurrentContext(context_);
    // 调用方须先等待主图完成；后端自持的各平台队列、交换链和围栏在呈现后等待完成。
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ImGui::RenderPlatformWindowsDefault();
}
}
