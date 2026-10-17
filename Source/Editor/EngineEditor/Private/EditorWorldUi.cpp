#include "editor/EditorWorldUi.h"
#include <imgui.h>
#include <Windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cwctype>
#include <utility>

namespace render::editor {
namespace {
std::wstring SelectWorldPath(bool save, const std::wstring &previous, std::string &error) {
    // 原生宽字符对话框保留中文路径，并禁止对话框改变进程工作目录。
    std::wstring buffer(32768, L'\0');
    if (previous.size() < buffer.size())
        buffer.replace(0, previous.size(), previous);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"场景配置 (*.textproto;*.pb;*.world)\0*.textproto;*.pbtxt;*.prototxt;*.pb;*.world\0Protobuf 文本 "
                         L"(*.textproto)\0*.textproto;*.pbtxt;*.prototxt\0Protobuf 二进制 (*.pb;*.world)\0*.pb;*.world\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrTitle = save ? L"保存世界文档" : L"选择场景";
    const auto directory = DefaultWorldPath().parent_path().wstring();
    dialog.lpstrInitialDir = directory.c_str();
    dialog.lpstrDefExt = L"textproto";
    dialog.Flags =
        OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const bool accepted = (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) != FALSE;
    if (!accepted) {
        const auto code = CommDlgExtendedError();
        if (code)
            error = "选择世界文档失败，错误代码：" + std::to_string(code);
        return {};
    }
    buffer.resize(wcslen(buffer.c_str()));
    return buffer;
}
}

void EditorWorldUi::RequestOpen() {
    // 项目列表和原生选择器进入相同的安全加载边界，取消时不生成请求。
    error_.clear();
    auto path = SelectWorldPath(false, lastPath_, error_);
    if (path.empty()) return;
    lastPath_ = path;
    request_ = WorldUiRequest{WorldUiRequestKind::Open, std::move(path), {}};
}

void EditorWorldUi::DrawSceneSelector() {
    if (ImGui::Button("选择场景...###Choose Scene", ImVec2(-1, 0))) {
        // 只枚举正式世界文件，不在界面线程解析模型和纹理；新文件在下次打开时可见。
        sceneFiles_.clear();
        error_.clear();
        try {
            const auto directory = DefaultWorldPath().parent_path();
            if (std::filesystem::is_directory(directory)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                    if (!entry.is_regular_file()) continue;
                    auto extension = entry.path().extension().wstring();
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (extension == L".textproto" || extension == L".pb" || extension == L".world" ||
                        extension == L".pbtxt" || extension == L".prototxt")
                        sceneFiles_.push_back(entry.path().lexically_relative(directory));
                }
            }
            std::sort(sceneFiles_.begin(), sceneFiles_.end());
        } catch (const std::filesystem::filesystem_error&) {
            error_ = "无法完整读取项目场景目录，仍可浏览场景文件";
        }
        ImGui::OpenPopup("项目场景###Scene Selector");
    }
    // 路径保持原生宽字符，只有菜单文字转换为 UTF-8，保证中文目录可正常打开。
    if (ImGui::BeginPopup("项目场景###Scene Selector")) {
        ImGui::SeparatorText("项目场景");
        if (sceneFiles_.empty()) ImGui::TextDisabled("暂无项目场景");
        for (const auto& file : sceneFiles_) {
            const auto label = file.generic_u8string();
            if (ImGui::MenuItem(reinterpret_cast<const char*>(label.c_str()))) {
                const auto path = DefaultWorldPath().parent_path() / file;
                lastPath_ = path.wstring();
                request_ = WorldUiRequest{WorldUiRequestKind::Open, lastPath_, {}};
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("浏览场景文件...")) RequestOpen();
        ImGui::EndPopup();
    }
    // 对话框和保存错误在常驻入口显示，不依赖用户当前打开的编辑器页签。
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .35f, .3f, 1));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
    }
}

void EditorWorldUi::Draw(const gameplay::World &world) {
    ImGui::PushID(this);
    // 这里只发出请求，宿主完成读写、渲染资源重建和选择历史基线替换。
    if (ImGui::Button("打开", ImVec2(54, 0))) RequestOpen();
    ImGui::SetItemTooltip("打开世界文档");
    ImGui::SameLine();
    if (ImGui::Button("保存", ImVec2(54, 0))) {
        error_.clear();
        auto path = SelectWorldPath(true, lastPath_, error_);
        if (!path.empty()) {
            lastPath_ = path;
            request_ = WorldUiRequest{WorldUiRequestKind::Save, std::move(path), {}};
        }
    }
    ImGui::SetItemTooltip("保存世界文档");
    const auto &document = world.Document();
    const char *selected = "导入场景";
    for (const auto &level : document.levels())
        for (const auto &scene : level.scenes())
            if (scene.id() == world.ActiveSceneId())
                selected = scene.name().empty() ? scene.id().c_str() : scene.name().c_str();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##世界场景", selected)) {
        for (const auto &level : document.levels()) {
            ImGui::SeparatorText(level.name().empty() ? level.id().c_str() : level.name().c_str());
            for (const auto &scene : level.scenes()) {
                const bool active = scene.id() == world.ActiveSceneId();
                ImGui::PushID(scene.id().c_str());
                if (ImGui::Selectable(scene.name().empty() ? scene.id().c_str() : scene.name().c_str(), active) &&
                    !active)
                    request_ = WorldUiRequest{WorldUiRequestKind::SwitchScene, {}, scene.id()};
                if (active)
                    ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
    ImGui::PopID();
}
std::optional<WorldUiRequest> EditorWorldUi::ConsumeRequest() {
    auto request = std::move(request_);
    request_.reset();
    return request;
}
void EditorWorldUi::Reset() {
    // 世界切换不复用旧请求或错误，对话框目录仍方便连续编辑。
    request_.reset();
    error_.clear();
    sceneFiles_.clear();
}
}
