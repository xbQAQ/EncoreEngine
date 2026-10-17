#include "editor/EditorContentBrowser.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

#ifndef EDITOR_CONTENT_DIRECTORY
#define EDITOR_CONTENT_DIRECTORY L"Content"
#endif

namespace render::editor {
namespace {
std::string Utf8Path(const std::filesystem::path& path) {
    // 拒绝无效的原生 Unicode，避免源路径或错误消息中的字符被静默替换。
    const auto& wide = path.native();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("文件路径包含无效的 Unicode 字符");
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        result.data(), size, nullptr, nullptr) != size) throw std::runtime_error("文件路径无法转换为 UTF-8");
    return result;
}

std::string LowerAscii(std::string value) {
    // 保留多字节 UTF-8，只对常见资源扩展名及拉丁名称执行大小写无关比较。
    for (auto& letter : value) if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
    return value;
}

bool Matches(const std::string& name, const char* search) {
    return !*search || LowerAscii(name).find(LowerAscii(search)) != std::string::npos;
}

bool ModelExtension(const std::string& extension) {
    // 文件列表、命令和入队共用格式集合，避免可见模型无法真正激活。
    return extension == ".fbx" || extension == ".gltf" || extension == ".glb";
}
bool WorldExtension(const std::string& extension) {
    // 世界配置在文件列表和操作按钮之间共用识别规则，不与模型导入队列混用。
    return extension == ".pb" || extension == ".textproto" || extension == ".world";
}

std::string ReadSource(const std::filesystem::path& path) {
    // 分配内存前限制源码输入大小，任何失败都保留原有草稿。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0) throw std::runtime_error("无法打开源文件");
    if (file.tellg() > 1024 * 1024) throw std::runtime_error("源文件超过 1 MiB 限制");
    std::string source(static_cast<size_t>(file.tellg()), '\0');
    file.seekg(0);
    if (!source.empty() && !file.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::runtime_error("源文件读取不完整");
    if (source.starts_with("\xef\xbb\xbf")) source.erase(0, 3);
    if (source.find('\0') != std::string::npos) throw std::runtime_error("源文件包含空字符");
    if (!source.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
        static_cast<int>(source.size()), nullptr, 0)) throw std::runtime_error("源文件必须使用有效的 UTF-8 编码");
    return source;
}

const char* ResourceType(const engine::Resource& resource) {
    if (dynamic_cast<const MaterialResource*>(&resource)) return "材质";
    if (dynamic_cast<const TextureResource*>(&resource)) return "纹理";
    if (dynamic_cast<const MeshResource*>(&resource)) return "网格";
    return "其他资源";
}

unsigned MaterialIndex(const gameplay::World& world, const engine::Resource& resource) {
    // 资源标识映射到现有 GPU 槽位，浏览器不增加材质资源所有者。
    for (size_t index = 0; index < world.Materials().size(); ++index)
        if (world.Material(index) == &resource) return static_cast<unsigned>(index);
    return ~0u;
}

void TexturePreview(const TextureResource& texture) {
    // 有尺寸上限的最近邻 CPU 预览不需要新增 GPU 描述符或图像资源生命周期。
    const unsigned columns = std::min(24u, texture.Width()), rows = std::min(24u, texture.Height());
    const float available = std::min(96.0f, std::max(1.0f, ImGui::GetContentRegionAvail().x));
    const float cell = available / static_cast<float>(std::max(columns, rows));
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    // 棋盘背景用于区分透明纹素与空白或加载失败的图像预览。
    for (unsigned y = 0; y < rows; ++y)
        for (unsigned x = 0; x < columns; ++x)
            draw->AddRectFilled(ImVec2(origin.x + x * cell, origin.y + y * cell),
                ImVec2(origin.x + (x + 1) * cell, origin.y + (y + 1) * cell),
                ((x / 3 + y / 3) & 1u) ? IM_COL32(90, 90, 90, 255) : IM_COL32(145, 145, 145, 255));
    for (unsigned y = 0; y < rows; ++y)
        for (unsigned x = 0; x < columns; ++x) {
            const auto texel = texture.Texels()[size_t(y * texture.Height() / rows) * texture.Width() + x * texture.Width() / columns];
            draw->AddRectFilled(ImVec2(origin.x + x * cell, origin.y + y * cell),
                ImVec2(origin.x + (x + 1) * cell, origin.y + (y + 1) * cell), texel);
        }
    ImGui::Dummy(ImVec2(columns * cell, rows * cell));
}

std::filesystem::path ContentDirectory() {
    // 打包程序优先使用相邻内容目录，从源码运行时回退到配置的编辑目录。
    std::array<wchar_t, 32768> executable{};
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    std::error_code error;
    if (size && size < executable.size()) {
        const auto deployed = std::filesystem::path(executable.data()).parent_path() / L"Content";
        if (std::filesystem::is_directory(deployed, error)) return std::filesystem::weakly_canonical(deployed);
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(EDITOR_CONTENT_DIRECTORY));
}

bool InsideContent(const std::filesystem::path& root, const std::filesystem::path& directory) {
    // 比较路径分量前先解析目录链接，防止导航离开项目内容根目录。
    const auto target = std::filesystem::weakly_canonical(directory);
    auto a = root.begin(), b = target.begin();
    for (; a != root.end(); ++a, ++b) if (b == target.end() || *a != *b) return false;
    return true;
}
}

bool ApplyMaterialAsset(gameplay::World& world, gameplay::Entity entity, unsigned materialIndex, RenderSettings& settings) {
    // 光源拥有的发光几何保留作者材质标识，保证光源采样及 MIS 一致。
    auto& registry = world.Registry();
    if (!registry.valid(entity) || materialIndex >= world.Materials().size() || !world.Material(materialIndex)) return false;
    auto* mesh = registry.try_get<gameplay::MeshRenderer>(entity);
    if (!mesh || mesh->lightOwned || (mesh->materialIndex == materialIndex && !mesh->useSurfaceMaterials)) return false;
    // 显式分配材质表示覆盖整个模型，原始表面仍可在检查器中重新启用。
    mesh->materialIndex = materialIndex;
    mesh->useSurfaceMaterials = false;
    world.MarkDirty();
    settings.reset = true;
    return true;
}

bool LoadContentScript(gameplay::World& world, gameplay::Entity entity, const std::filesystem::path& path,
    RenderSettings& settings, std::string& error) {
    std::string encodedPath;
    try {
        // 文件读写前先校验目标，避免无效选择替换其他资源的源码。
        encodedPath = Utf8Path(path);
        const auto extension = LowerAscii(Utf8Path(path.extension()));
        auto& registry = world.Registry();
        if (!registry.valid(entity)) throw std::runtime_error("请先选择场景对象");
        if (extension == ".hlsl") {
            auto* mesh = registry.try_get<gameplay::MeshRenderer>(entity);
            if (!mesh || mesh->lightOwned) throw std::runtime_error("请选择可编辑的网格对象；光源发光体材质受保护");
            auto* material = world.Material(mesh->materialIndex);
            if (!material) throw std::runtime_error("选中网格没有有效材质");
            auto source = ReadSource(path);
            material->ScriptSource() = std::move(source);
            material->ScriptPath() = encodedPath;
        } else if (extension == ".lua") {
            auto source = ReadSource(path);
            const auto* previous = registry.try_get<gameplay::LuaScript>(entity);
            const bool enabled = previous ? previous->enabled : true;
            registry.emplace_or_replace<gameplay::LuaScript>(entity, std::move(source), encodedPath, enabled);
        } else throw std::runtime_error("所选文件不是 HLSL 或 Lua 脚本");
        world.MarkDirty();
        settings.reset = true;
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = "载入失败：" + encodedPath + "\n" + failure.what();
        return false;
    }
}

void EditorContentBrowser::DrawResourcesPanel(gameplay::World& world, gameplay::Entity selection, RenderSettings& settings) {
    // 两种浏览器拥有独立停靠窗口，资源与文件筛选仍归同一浏览状态所有者。
    ImGui::PushID(this);
    DrawError();
    DrawResources(world, selection, settings);
    ImGui::PopID();
}

void EditorContentBrowser::DrawFilesPanel(gameplay::World& world, gameplay::Entity selection, RenderSettings& settings) {
    ImGui::PushID(this);
    DrawError();
    DrawFiles(world, selection, settings);
    ImGui::PopID();
}

void EditorContentBrowser::DrawError() {
    // 诊断沿用两个浏览窗口共享的可恢复错误，不额外创建占高度的固定容器。
    if (!error_.empty()) {
        if (ImGui::SmallButton("X##ClearBrowserError")) error_.clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("关闭提示");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.43f, 0.38f, 1));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
    }
}

void EditorContentBrowser::DrawResources(gameplay::World& world, gameplay::Entity selection, RenderSettings& settings) {
    // 过滤作者资源列表，同时保留各个具体资源类型的检查能力。
    const float toolbarWidth = ImGui::GetContentRegionAvail().x;
    const float typeWidth = std::min(110.0f, std::max(60.0f, toolbarWidth * 0.3f));
    ImGui::SetNextItemWidth(std::max(1.0f, toolbarWidth - typeWidth - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##ResourceSearch", "搜索资源", resourceSearch_.data(), resourceSearch_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(typeWidth);
    ImGui::Combo("##ResourceType", &resourceType_, "全部类型\0材质\0纹理\0网格\0");
    const auto resources = world.Resources().All();
    const engine::Resource* selected = nullptr;
    for (const auto* resource : resources) if (resource->Id() == selectedResource_) selected = resource;
    if (!ImGui::BeginTable("ResourceLayout", 2, ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("资源列表", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("资源详情", ImGuiTableColumnFlags_WidthStretch, 0.3f);
    ImGui::TableNextColumn();
    if (ImGui::BeginTable("ResourceRows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable, ImVec2(0, std::max(40.0f, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupColumn("名称"); ImGui::TableSetupColumn("类型"); ImGui::TableSetupColumn("数据");
        ImGui::TableHeadersRow();
        for (const auto* resource : resources) {
            const auto* material = dynamic_cast<const MaterialResource*>(resource);
            const auto* texture = dynamic_cast<const TextureResource*>(resource);
            const auto* mesh = dynamic_cast<const MeshResource*>(resource);
            if ((resourceType_ == 1 && !material) || (resourceType_ == 2 && !texture) || (resourceType_ == 3 && !mesh) ||
                !Matches(resource->Name() + ResourceType(*resource) + std::string(resource->TypeName()), resourceSearch_.data())) continue;
            ImGui::PushID(resource);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto index = material ? MaterialIndex(world, *resource) : ~0u;
            if (ImGui::Selectable(resource->Name().empty() ? "未命名资源" : resource->Name().c_str(),
                selectedResource_ == resource->Id(), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedResource_ = resource->Id(); selected = resource;
                if (material && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    (void)ApplyMaterialAsset(world, selection, index, settings);
            }
            // 材质协议名须与视口和详情接收端一致，负载只存槽位，避免指针超出 World 寿命。
            if (material && index != ~0u && ImGui::BeginDragDropSource()) {
                const std::uint32_t payload = index;
                ImGui::SetDragDropPayload("ENCORE_MATERIAL", &payload, sizeof(payload));
                ImGui::Text("材质：%s", resource->Name().c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ResourceType(*resource));
            ImGui::TableNextColumn();
            if (texture) ImGui::Text("%u x %u", texture->Width(), texture->Height());
            else if (mesh) ImGui::Text("%zu 个三角形", mesh->Triangles().size());
            else if (material) ImGui::Text("槽位 %u", index);
            else ImGui::Text("ID %llu", static_cast<unsigned long long>(resource->Id()));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TableNextColumn();
    // 选中资源的元数据不额外套框，小型纹理预览使用 CPU 拥有的像素。
    if (selected) {
        ImGui::TextWrapped("%s", selected->Name().c_str());
        ImGui::Text("类型：%s", ResourceType(*selected));
        if (const auto* texture = dynamic_cast<const TextureResource*>(selected)) {
            ImGui::TextWrapped("%u x %u，%.1f KiB", texture->Width(), texture->Height(), texture->Texels().size_bytes() / 1024.0);
            TexturePreview(*texture);
        } else if (const auto* mesh = dynamic_cast<const MeshResource*>(selected)) {
            ImGui::Text("三角形：%zu", mesh->Triangles().size());
            ImGui::Text("表面：%zu", mesh->Surfaces().size());
            ImGui::Text("属性：%zu", mesh->Attributes().size());
        } else if (const auto* material = dynamic_cast<const MaterialResource*>(selected)) {
            const auto index = MaterialIndex(world, *selected);
            const auto* target = world.Registry().valid(selection) ? world.Registry().try_get<gameplay::MeshRenderer>(selection) : nullptr;
            ImGui::Text("槽位：%u", index);
            ImGui::Text("粗糙度：%.3f", material->Parameters().baseRoughness.w);
            ImGui::Text("金属度：%.3f", material->Parameters().emissionMetallic.w);
            ImGui::BeginDisabled(!target || target->lightOwned || (target->materialIndex == index && !target->useSurfaceMaterials) || index == ~0u);
            if (ImGui::Button("应用材质", ImVec2(-1, 0))) (void)ApplyMaterialAsset(world, selection, index, settings);
            ImGui::EndDisabled();
        } else ImGui::TextWrapped("原始类型：%.*s", static_cast<int>(selected->TypeName().size()), selected->TypeName().data());
    } else ImGui::TextDisabled("未选择资源");
    ImGui::EndTable();
}

void EditorContentBrowser::PreviewFile(const std::filesystem::path& path) {
    // 预览拥有独立快照和诊断，任何读取结果都不触及 World、脚本或撤销历史。
    previewFile_.clear(); previewText_.clear(); previewError_.clear();
    if (path.empty() || !IsTextContentFile(path)) return;
    previewFile_ = path;
    try {
        if (!InsideContent(contentRoot_, path)) throw std::runtime_error("文件位于项目 Content 范围之外");
        previewText_ = ReadContentText(path);
    } catch (const std::exception& failure) { previewError_ = "预览失败：" + std::string(failure.what()); }
}

void EditorContentBrowser::DrawTextPreview(float width, float height) {
    // 外层只负责分配空间，正文控件独立滚动并允许选择复制，禁止编辑和写回。
    if (ImGui::BeginChild("TextFilePreview", ImVec2(width, height), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::TextWrapped("%s", Utf8Path(previewFile_.filename()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Utf8Path(previewFile_).c_str());
        if (!previewError_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .43f, .38f, 1));
            ImGui::TextWrapped("%s", previewError_.c_str());
            ImGui::PopStyleColor();
        } else if (previewText_.empty()) ImGui::TextDisabled("空文件");
        else ImGui::InputTextMultiline("##FileText", previewText_.data(), previewText_.size() + 1,
            ImVec2(-1, std::max(1.0f, ImGui::GetContentRegionAvail().y)), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::EndChild();
}

void EditorContentBrowser::RefreshDirectory() {
    // 校验项目目录边界后，刷新操作整体替换缓存快照。
    directoryDirty_ = false;
    try {
        if (contentRoot_.empty()) { contentRoot_ = ContentDirectory(); directory_ = contentRoot_; }
        if (!InsideContent(contentRoot_, directory_)) throw std::runtime_error("目录位于项目 Content 范围之外");
        std::error_code failure;
        std::vector<FileEntry> entries;
        std::filesystem::directory_iterator iterator(directory_, std::filesystem::directory_options::skip_permission_denied, failure);
        if (failure) throw std::runtime_error("无法读取项目目录：" + Utf8Path(directory_));
        for (const auto& entry : iterator) {
            const bool folder = entry.is_directory(failure);
            if (failure) { failure.clear(); continue; }
            const auto bytes = folder ? 0 : entry.file_size(failure);
            entries.push_back({entry.path(), Utf8Path(entry.path().filename()), failure ? 0 : bytes, folder});
            failure.clear();
        }
        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.directory != b.directory) return a.directory;
            return LowerAscii(a.name) < LowerAscii(b.name);
        });
        files_ = std::move(entries);
        // 刷新同时重新读取所选文本；已被外部删除的文件不能继续显示旧正文。
        const auto selected = std::find_if(files_.begin(), files_.end(), [&](const FileEntry& file) { return file.path == selectedFile_; });
        if (selected == files_.end()) selectedFile_.clear();
        PreviewFile(selected == files_.end() || selected->directory ? std::filesystem::path{} : selected->path);
        error_.clear();
    } catch (const std::exception& failure) { error_ = "目录刷新失败：" + std::string(failure.what()); }
}

void EditorContentBrowser::ActivateFile(const std::filesystem::path& path, bool directory) {
    // 文件操作属于本地编辑行为，导入则延迟交给编辑器现有流程处理。
    try {
        if (directory) {
            if (!InsideContent(contentRoot_, path)) throw std::runtime_error("目录位于项目 Content 范围之外");
            directory_ = path; selectedFile_.clear(); directoryDirty_ = true;
            PreviewFile({});
        } else {
            const auto extension = LowerAscii(Utf8Path(path.extension()));
            if (ModelExtension(extension)) (void)QueueModelImport(path);
            else if (WorldExtension(extension)) (void)QueueWorldOpen(path);
            // 双击文本仍然只查看内容，应用脚本必须使用明确的操作按钮。
            else if (IsTextContentFile(path)) PreviewFile(path);
        }
    } catch (const std::exception& failure) { error_ = "文件操作失败：" + std::string(failure.what()); }
}

void EditorContentBrowser::DrawFiles(gameplay::World& world, gameplay::Entity selection, RenderSettings& settings) {
    if (directoryDirty_) RefreshDirectory();
    // 创建菜单与文件列表的右键菜单共用入口，文件写入在列表遍历结束后执行。
    if (ImGui::Button("+ 新建###New Content")) ImGui::OpenPopup("新建###New Content Menu");
    if (ImGui::BeginPopup("新建###New Content Menu")) {
        DrawCreateMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    // 导航限制在 Content 内，显式刷新用于观察外部文件变化。
    ImGui::BeginDisabled(directory_.empty() || directory_ == contentRoot_);
    if (ImGui::ArrowButton("ParentDirectory", ImGuiDir_Up)) {
        directory_ = directory_.parent_path(); selectedFile_.clear(); directoryDirty_ = true;
        PreviewFile({});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("上一级");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("刷新")) directoryDirty_ = true;
    if (ImGui::GetContentRegionAvail().x >= 220) ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##FileSearch", "搜索当前目录", fileSearch_.data(), fileSearch_.size());
    // 路径只占一行，完整目录通过悬停查看，避免深目录挤掉文件列表。
    try {
        const auto path = Utf8Path(directory_);
        ImGui::TextUnformatted(path.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    }
    catch (const std::exception& failure) { error_ = failure.what(); }
    // 操作行放在列表底部，选择变化只能影响列表高度，不能让双击的第二次点击落到另一行。
    const auto actionPath = selectedFile_;
    const auto found = std::find_if(files_.begin(), files_.end(), [&](const FileEntry& file) { return file.path == actionPath; });
    const auto actionExtension = LowerAscii(Utf8Path(actionPath.extension()));
    const char* command = found == files_.end() ? nullptr : found->directory ? "打开文件夹" : ModelExtension(actionExtension) ? "导入模型" :
        WorldExtension(actionExtension) ? "打开世界" : actionExtension == ".hlsl" ? "载入材质脚本" :
        actionExtension == ".lua" ? "添加 Lua 脚本" : nullptr;
    const float footerHeight = command ? ImGui::GetFrameHeightWithSpacing() : 0;
    // 有文本选择时共享剩余空间；窄面板改为上下排列，避免两列各自被压得过窄。
    const auto available = ImGui::GetContentRegionAvail();
    const auto spacing = ImGui::GetStyle().ItemSpacing;
    const bool preview = !previewFile_.empty(), horizontal = available.x >= 520;
    const float bodyHeight = std::max(1.0f, available.y - footerHeight);
    const float listWidth = preview && horizontal ? std::max(1.0f, (available.x - spacing.x) * .5f) : 0;
    const float listHeight = std::max(1.0f, preview && !horizontal ? (bodyHeight - spacing.y) * .45f : bodyHeight);
    // 滚动表格吸收分配的全部高度，外层面板不再出现重复的垂直滚动条。
    if (ImGui::BeginTable("ContentFiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(listWidth, listHeight))) {
        ImGui::TableSetupColumn("名称"); ImGui::TableSetupColumn("类型"); ImGui::TableSetupColumn("大小");
        ImGui::TableHeadersRow();
        for (const auto& file : files_) {
            if (!Matches(file.name, fileSearch_.data())) continue;
            ImGui::PushID(file.name.c_str());
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable(file.name.c_str(), file.path == selectedFile_,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedFile_ = file.path;
                PreviewFile(file.directory ? std::filesystem::path{} : file.path);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    ActivateFile(file.path, file.directory);
            }
            // 右键先选择指针下的条目，菜单不能误改上一次左键选中的文件。
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                selectedFile_ = file.path;
                PreviewFile(file.directory ? std::filesystem::path{} : file.path);
            }
            if (revealCreated_ && file.path == selectedFile_) {
                ImGui::SetScrollHereY();
                revealCreated_ = false;
            }
            ImGui::TableNextColumn();
            const auto extension = LowerAscii(Utf8Path(file.path.extension()));
            ImGui::TextUnformatted(file.directory ? "文件夹" : ModelExtension(extension) ? "模型" :
                WorldExtension(extension) ? "世界配置" : extension == ".hlsl" ? "材质脚本" : extension == ".lua" ? "游戏脚本" :
                IsTextContentFile(file.path) ? "文本文件" : "文件");
            ImGui::TableNextColumn(); if (!file.directory) ImGui::Text("%.1f KiB", file.bytes / 1024.0);
            ImGui::PopID();
        }
        // 列表空白区域和已有条目都允许右键新建，不依赖先选中场景对象。
        if (ImGui::BeginPopupContextWindow("ContentCreateContext", ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem("重命名###Rename Content", "F2", false, !selectedFile_.empty()))
                BeginRename(selectedFile_);
            ImGui::Separator();
            DrawCreateMenu();
            ImGui::EndPopup();
        }
        ImGui::EndTable();
    }
    if (preview) {
        if (horizontal) ImGui::SameLine();
        DrawTextPreview(0, horizontal ? listHeight : std::max(1.0f, ImGui::GetContentRegionAvail().y - footerHeight));
    }
    // 命令与本帧标签绑定同一个路径快照，文本双击不会隐式载入材质或添加 Lua 组件。
    if (command && ImGui::Button(command)) {
        if (!found->directory && (actionExtension == ".hlsl" || actionExtension == ".lua"))
            (void)LoadContentScript(world, selection, actionPath, settings, error_);
        else ActivateFile(actionPath, found->directory);
    }
    DrawCreateDialog();
    // 文件窗口及其列表拥有焦点且没有文字编辑或弹窗时，F2 才操作当前条目。
    if (!selectedFile_.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        ImGui::IsKeyPressed(ImGuiKey_F2, false)) BeginRename(selectedFile_);
    DrawRenameDialog();
}

void EditorContentBrowser::BeginRename(const std::filesystem::path& path) {
    // 保存源路径快照，完整名称含扩展名；模态操作不会随筛选和当前选中项漂移。
    const auto name = Utf8Path(path.filename());
    if (name.size() >= renameName_.size()) { error_ = "文件名称过长，无法重命名"; return; }
    renameSource_ = path;
    renameName_.fill(0);
    std::memcpy(renameName_.data(), name.data(), name.size());
    renameError_.clear();
    renameRequested_ = focusRenameName_ = true;
}

void EditorContentBrowser::DrawRenameDialog() {
    // 与创建操作采用相同父窗口作用域，右键菜单和 F2 共用一个命名对话框。
    if (std::exchange(renameRequested_, false)) ImGui::OpenPopup("重命名###Rename Content Dialog");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::clamp(ImGui::GetMainViewport()->Size.x - 32.0f, 180.0f, 420.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("重命名###Rename Content Dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("原名称：%s", Utf8Path(renameSource_.filename()).c_str());
    ImGui::TextUnformatted("名称");
    ImGui::SetNextItemWidth(-1);
    if (std::exchange(focusRenameName_, false)) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##Rename Entry Name", renameName_.data(), renameName_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (!renameError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .43f, .38f, 1));
        ImGui::TextWrapped("%s", renameError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!renameName_[0]);
    const bool rename = ImGui::Button("重命名###Confirm Rename");
    ImGui::EndDisabled();
    // 改名成功后刷新列表与预览；失败留在对话框，场景和已载入资源始终不受修改。
    if (rename || enter) {
        try {
            selectedFile_ = RenameContentEntry(contentRoot_, renameSource_, renameName_.data());
            PreviewFile(std::filesystem::is_directory(selectedFile_) ? std::filesystem::path{} : selectedFile_);
            fileSearch_.fill(0);
            directoryDirty_ = revealCreated_ = true;
            error_.clear();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& failure) { renameError_ = failure.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消###Cancel Rename") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorContentBrowser::BeginCreate(ContentEntryKind kind) {
    // 默认名称保留类型扩展名；最终是否重名交给独占创建统一判断。
    createKind_ = kind;
    const char* name = kind == ContentEntryKind::Folder ? "新建文件夹" : kind == ContentEntryKind::MaterialScript ?
        "新建材质.hlsl" : kind == ContentEntryKind::GameplayScript ? "新建脚本.lua" : "新建文件.txt";
    newEntryName_.fill(0);
    std::memcpy(newEntryName_.data(), name, std::strlen(name));
    creationError_.clear();
    creationRequested_ = focusCreationName_ = true;
}

void EditorContentBrowser::DrawCreateMenu() {
    // 入口只记录作者意图，避免在表格或弹出菜单的 ID 栈内打开另一个对话框。
    if (ImGui::MenuItem("文件夹###New Folder")) BeginCreate(ContentEntryKind::Folder);
    if (ImGui::MenuItem("文件###New File")) BeginCreate(ContentEntryKind::File);
    ImGui::Separator();
    if (ImGui::MenuItem("HLSL 材质脚本###New Material Script")) BeginCreate(ContentEntryKind::MaterialScript);
    if (ImGui::MenuItem("Lua 游戏脚本###New Gameplay Script")) BeginCreate(ContentEntryKind::GameplayScript);
}

void EditorContentBrowser::DrawCreateDialog() {
    // 对话框始终归属浏览器父窗口，右键菜单和工具栏不会生成不同的模态作用域。
    if (std::exchange(creationRequested_, false)) ImGui::OpenPopup("新建内容###Create Content");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::clamp(ImGui::GetMainViewport()->Size.x - 32.0f, 180.0f, 420.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("新建内容###Create Content", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("目录：%s", Utf8Path(directory_).c_str());
    ImGui::TextUnformatted("名称");
    ImGui::SetNextItemWidth(-1);
    if (std::exchange(focusCreationName_, false)) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##New Entry Name", newEntryName_.data(), newEntryName_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (!creationError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .43f, .38f, 1));
        ImGui::TextWrapped("%s", creationError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!newEntryName_[0]);
    const bool create = ImGui::Button("创建###Create Entry");
    ImGui::EndDisabled();
    // 成功后清除过滤器并选中新条目，失败则留在对话框修正名称，既不覆盖也不导入。
    if (create || enter) {
        try {
            selectedFile_ = CreateContentEntry(contentRoot_, directory_, newEntryName_.data(), createKind_);
            PreviewFile(createKind_ == ContentEntryKind::Folder ? std::filesystem::path{} : selectedFile_);
            fileSearch_.fill(0);
            directoryDirty_ = revealCreated_ = true;
            error_.clear();
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& failure) { creationError_ = failure.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("取消###Cancel Create") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

std::wstring EditorContentBrowser::ConsumeImportRequest() {
    // 操作只消费一次，避免延迟导入失败后逐帧重新打开同一模型。
    return std::exchange(importRequest_, std::wstring{});
}

bool EditorContentBrowser::QueueModelImport(const std::filesystem::path& path) {
    // 队列不解析资源，也不修改 World；加载失败仍由宿主保留当前场景。
    if (!ModelExtension(LowerAscii(Utf8Path(path.extension())))) return false;
    importRequest_ = path.wstring();
    return true;
}

std::wstring EditorContentBrowser::ConsumeWorldOpenRequest() {
    // 每个请求只消费一次，宿主负责解析失败后的错误显示与场景保留。
    return std::exchange(worldOpenRequest_, std::wstring{});
}

bool EditorContentBrowser::QueueWorldOpen(const std::filesystem::path& path) {
    if (!WorldExtension(LowerAscii(Utf8Path(path.extension())))) return false;
    worldOpenRequest_ = path.wstring();
    return true;
}

void EditorContentBrowser::Reset() {
    // 资源标识和延迟操作属于被替换的场景，当前项目目录则可以保留。
    selectedResource_ = 0;
    PreviewFile({});
    selectedFile_.clear(); importRequest_.clear(); worldOpenRequest_.clear(); error_.clear();
    resourceSearch_.fill(0); fileSearch_.fill(0); resourceType_ = 0; directoryDirty_ = true;
    creationRequested_ = focusCreationName_ = revealCreated_ = false;
    newEntryName_.fill(0); creationError_.clear();
    renameSource_.clear(); renameName_.fill(0); renameError_.clear();
    renameRequested_ = focusRenameName_ = false;
}
}
