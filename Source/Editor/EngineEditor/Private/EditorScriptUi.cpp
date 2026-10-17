#include "editor/EditorScriptUi.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <imgui.h>
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace render::editor {
namespace {
int ResizeText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto& text = *static_cast<std::string*>(data->UserData);
        text.resize(static_cast<std::size_t>(data->BufTextLen)); data->Buf = text.data();
    }
    return 0;
}
bool SourceEditor(const char* label, std::string& source) {
    // 脚本窗口本身可浮动缩放；输入框填满剩余高度，只保留原生文本滚动与撤销。
    ImGui::PushID(label);
    const bool changed = ImGui::InputTextMultiline(label, source.data(), source.capacity() + 1,
        ImVec2(-1, std::max(1.0f, ImGui::GetContentRegionAvail().y)),
        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeText, &source);
    ImGui::PopID();
    return changed;
}
std::string Utf8Path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::filesystem::path PickFile(bool save, bool lua) {
    wchar_t buffer[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = lua ? L"Lua 脚本 (*.lua)\0*.lua\0所有文件\0*.*\0" : L"HLSL 材质 (*.hlsl)\0*.hlsl\0所有文件\0*.*\0";
    dialog.lpstrFile = buffer; dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrDefExt = lua ? L"lua" : L"hlsl";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) return std::filesystem::path(buffer);
    return {};
}
bool FileControls(std::string& source, std::string& storedPath, bool lua, std::string& error) {
    bool loaded = false;
    if (ImGui::Button("打开...")) {
        const auto path = PickFile(false, lua);
        if (!path.empty()) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file || file.tellg() < 0 || file.tellg() > 1024 * 1024) error = "打开失败：源文件不可读或超过 1 MiB";
            else {
                std::string text(static_cast<std::size_t>(file.tellg()), '\0'); file.seekg(0);
                if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) error = "打开失败：读取不完整";
                // 与内容浏览器保持一致，拒绝二进制或损坏编码，避免破坏当前可编辑草稿。
                else if (text.find('\0') != std::string::npos || (!text.empty() &&
                    !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)))
                    error = "打开失败：源文件必须是有效的 UTF-8 文本";
                else { source = std::move(text); storedPath = Utf8Path(path); loaded = true; error.clear(); }
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("保存")) {
        // C++20 的 char8_t 路径构造明确保持 UTF-8，不依赖本地 ANSI 代码页。
        const auto path = storedPath.empty() ? PickFile(true, lua) : std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(storedPath.data()), storedPath.size()));
        if (!path.empty()) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(source.data(), static_cast<std::streamsize>(source.size()))) error = "保存失败：无法写入源文件";
            else { storedPath = Utf8Path(path); error.clear(); }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("另存为...")) {
        const auto path = PickFile(true, lua);
        if (!path.empty()) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(source.data(), static_cast<std::streamsize>(source.size()))) error = "保存失败：无法写入源文件";
            else { storedPath = Utf8Path(path); error.clear(); }
        }
    }
    if (!storedPath.empty()) ImGui::TextWrapped("%s", storedPath.c_str());
    return loaded;
}
void Diagnostic(const std::string& text, bool success) {
    if (text.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, success ? ImVec4(0.38f, 0.82f, 0.52f, 1) : ImVec4(1, 0.43f, 0.38f, 1));
    ImGui::TextWrapped("%s", text.c_str()); ImGui::PopStyleColor();
}
}
void EditorScriptUi::DrawMaterial(gameplay::World& world, gameplay::Entity entity) {
    // 独立窗口直接呈现所选材质，操作与诊断在上，源码编辑区吸收剩余高度。
    auto& registry = world.Registry();
    const auto* mesh = registry.valid(entity) ? registry.try_get<gameplay::MeshRenderer>(entity) : nullptr;
    auto* material = mesh ? world.Material(mesh->materialIndex) : nullptr;
    if (!material) { ImGui::TextDisabled("未选择材质对象"); return; }
    auto& source = material->ScriptSource();
    if (source.empty()) {
        if (ImGui::Button("创建表面脚本")) { source = DefaultMaterialScript(); world.MarkDirty(); }
    } else {
        if (FileControls(source, material->ScriptPath(), false, fileError_)) world.MarkDirty();
        if (ImGui::Button("编译并预览")) { compileRequested_ = true; pendingEdit_ = false; }
        ImGui::Checkbox("实时预览", &livePreview_);
        ImGui::SameLine();
        if (ImGui::Button("移除脚本")) { source.clear(); world.MarkDirty(); compileRequested_ = true; }
    }
    Diagnostic(materialResult_.diagnostics, materialResult_.success);
    Diagnostic(fileError_, false);
    if (!source.empty() && SourceEditor("##MaterialSource", source)) world.MarkDirty();
}

void EditorScriptUi::DrawGameplay(gameplay::World& world, gameplay::Entity entity, RenderSettings& settings) {
    // Lua 组件拥有独立停靠窗口，与 HLSL 编辑器同时可见且可分别拖出。
    auto& registry = world.Registry();
    if (!registry.valid(entity)) { ImGui::TextDisabled("未选择实体"); return; }
    auto* script = registry.try_get<gameplay::LuaScript>(entity);
    if (!script) {
        if (ImGui::Button("添加 Lua 组件")) {
            registry.emplace<gameplay::LuaScript>(entity, std::string(gameplay::ScriptSystem::DefaultSource())); world.MarkDirty();
        }
        return;
    }
    if (ImGui::Checkbox("启用脚本", &script->enabled)) world.MarkDirty();
    ImGui::SameLine();
    if (ImGui::Button(settings.simulate ? "暂停" : "运行")) settings.simulate = !settings.simulate;
    if (FileControls(script->source, script->path, true, fileError_)) world.MarkDirty();
    if (ImGui::Button("编译 Lua")) (void)gameplay_.Compile(world, entity);
    ImGui::SameLine();
    const bool remove = ImGui::Button("移除组件");
    Diagnostic(std::string(gameplay_.Diagnostics(entity)), false);
    Diagnostic(fileError_, false);
    if (SourceEditor("##LuaSource", script->source)) world.MarkDirty();
    // 所有借用完成后再移除组件，避免源码输入框引用失效。
    if (remove) { registry.remove<gameplay::LuaScript>(entity); world.MarkDirty(); }
}

void EditorScriptUi::Observe(gameplay::World& world) {
    // 历史回放后比较权威源码，确保撤销和重做也更新同一份实时预览。
    auto scripts = world.MaterialScripts();
    if (scripts != observed_) {
        observed_ = std::move(scripts); pendingEdit_ = !compileRequested_; editTime_ = ImGui::GetTime();
    }
    if (livePreview_ && pendingEdit_ && ImGui::GetTime() - editTime_ >= 0.45) {
        compileRequested_ = true; pendingEdit_ = false;
    }
}
bool EditorScriptUi::ConsumeMaterialCompileRequest() { return std::exchange(compileRequested_, false); }
void EditorScriptUi::Reset() {
    gameplay_.Clear(); observed_.clear(); materialResult_ = {}; fileError_.clear(); pendingEdit_ = compileRequested_ = false;
}
}
