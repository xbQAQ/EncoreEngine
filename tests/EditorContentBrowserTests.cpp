#include "editor/EditorContentBrowser.h"
#include "editor/EditorContentFiles.h"
#include "gameplay/ScriptSystem.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    // 拖放目标也会使用浏览器操作，因此独立于指针输入验证其行为。
    if (!condition) throw std::runtime_error(message);
}

void CheckTextReading(const std::filesystem::path& directory) {
    // 文本分类不会把二进制世界或图像载荷误交给预览控件。
    for (const auto* name : {L"说明.TXT", L"README", L"配置.json", L"表面.hlsl", L"行为.lua", L"世界.textproto"})
        Require(render::editor::IsTextContentFile(name), "Common text and script formats must support preview");
    for (const auto* name : {L"角色.pb", L"模型.fbx", L"像素.rgba", L"图像.png"})
        Require(!render::editor::IsTextContentFile(name), "Binary assets must retain their existing file handling");
    const auto path = directory / L"中文预览.txt";
    auto write = [&](const std::string& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(static_cast<bool>(file), "Write the text preview fixture");
    };
    auto reject = [&]() {
        bool rejected = false;
        try { (void)render::editor::ReadContentText(path); }
        catch (const std::exception& failure) { rejected = *failure.what() != '\0'; }
        Require(rejected, "Unreadable, damaged, binary or oversized text must report a preview error");
    };

    // 中文正文在不同 BOM 与字节序下保持一致，空文件也属于有效预览。
    const std::string source = "中文\n";
    write(source);
    Require(render::editor::ReadContentText(path) == source, "UTF-8 preview preserves Chinese and line endings");
    write("\xef\xbb\xbf" + source);
    Require(render::editor::ReadContentText(path) == source, "UTF-8 BOM is omitted from the preview text");
    write(std::string("\xff\xfe\x2d\x4e\x87\x65\x0a\x00", 8));
    Require(render::editor::ReadContentText(path) == source, "UTF-16 LE text is converted without losing Chinese");
    write(std::string("\xfe\xff\x4e\x2d\x65\x87\x00\x0a", 8));
    Require(render::editor::ReadContentText(path) == source, "UTF-16 BE text is converted using its explicit byte order");
    write("");
    Require(render::editor::ReadContentText(path).empty(), "An empty file has a valid empty preview");
    write("\xff\xfe");
    Require(render::editor::ReadContentText(path).empty(), "A BOM-only UTF-16 file has an empty preview");

    // 拒绝截断字符、孤立代理项及空字符，不能静默替换损坏编码或截断二进制内容。
    write(std::string("\xff\xfe\x41", 3)); reject();
    write(std::string("\xff\xfe\x00\xd8", 4)); reject();
    write("\xc3\x28"); reject();
    write(std::string("a\0b", 3)); reject();
    write(std::string("a\x01" "b", 3)); reject();
    write(std::string(1024 * 1024, 'x'));
    Require(render::editor::ReadContentText(path).size() == 1024 * 1024, "Text at the preview size limit remains readable");
    write(std::string(1024 * 1024 + 1, 'x')); reject();
    std::filesystem::remove(path); reject();
}

void CheckRenaming(const std::filesystem::path& root) {
    // 文件及非空目录改名必须保留内容，大小写修改与同名无操作也需保持有效。
    using render::editor::RenameContentEntry;
    const auto source = root / L"旧名称.txt";
    { std::ofstream file(source, std::ios::binary); file << "中文正文"; }
    auto renamed = RenameContentEntry(root, source, "新名称.txt");
    Require(!std::filesystem::exists(source) && render::editor::ReadContentText(renamed) == "中文正文",
        "Renaming a Unicode file must preserve its bytes and remove the old name");
    Require(RenameContentEntry(root, renamed, "新名称.txt") == renamed,
        "An unchanged filename is a successful no-op");
    renamed = RenameContentEntry(root, renamed, "Case.TXT");
    renamed = RenameContentEntry(root, renamed, "case.txt");
    bool exactCase = false;
    for (const auto& item : std::filesystem::directory_iterator(root))
        if (item.path().filename() == L"case.txt") exactCase = true;
    Require(exactCase && render::editor::ReadContentText(renamed) == "中文正文",
        "Case-only rename must update the actual directory entry without losing contents");
    const auto folder = root / L"原目录";
    std::filesystem::create_directory(folder);
    { std::ofstream file(folder / L"child.txt"); file << "nested"; }
    const auto movedFolder = RenameContentEntry(root, folder, "新目录");
    Require(!std::filesystem::exists(folder) && std::filesystem::is_regular_file(movedFolder / L"child.txt"),
        "Renaming a nonempty directory preserves its children");

    // 重名、路径穿越、无效名称和 Content 外的源必须失败，已有目标不能被替换。
    const auto collision = root / L"occupied.txt";
    { std::ofstream file(collision); file << "keep"; }
    auto reject = [&](const std::filesystem::path& entry, std::string_view name) {
        bool rejected = false;
        try { (void)RenameContentEntry(root, entry, name); }
        catch (const std::exception& failure) { rejected = *failure.what() != '\0'; }
        Require(rejected, "Invalid or colliding rename must report an error");
    };
    reject(renamed, "occupied.txt");
    reject(movedFolder, "occupied.txt");
    reject(renamed, "新目录");
    for (const auto* name : {"", "..", "../escape.txt", "sub/name.txt", "file:stream", "CON.txt", "bad?.txt", "trailing.", "trailing "})
        reject(renamed, name);
    reject(renamed, std::string(1, static_cast<char>(0xff)));
    reject(root, "root-renamed");
    reject(root.parent_path(), "outside");
    reject(root / L"missing.txt", "created.txt");
    Require(render::editor::ReadContentText(collision) == "keep" &&
        render::editor::ReadContentText(renamed) == "中文正文" && std::filesystem::is_directory(movedFolder),
        "Rejected rename must preserve the source and every existing destination");
}

void CheckTextPreviewUi(gameplay::World& world, gameplay::Entity mesh, render::RenderSettings& settings) {
    // 通过正式列表选择和鼠标双击验证预览，防止浏览源码时隐式应用脚本。
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 600};
    io.ConfigInputTrickleEventQueue = false;
    io.Fonts->AddFontDefault();
    Require(io.Fonts->Build(), "Build the text preview UI atlas");
    render::editor::EditorContentBrowser browser;
    const auto root = render::DefaultWorldPath().parent_path().parent_path();
    const std::string prefix = "TextPreviewUi-" + std::to_string(GetCurrentProcessId());
    const auto text = root / (prefix + ".txt"), script = root / (prefix + ".hlsl");
    const std::string source = "中文预览\n第二行\n";
    Require(!std::filesystem::exists(text) && !std::filesystem::exists(script), "Preview fixture names must be unused");
    const auto revision = world.Revision();
    const auto scripts = world.MaterialScripts();
    settings.reset = false;
    ImVec2 panelSize{1000, 600};
    auto frame = [&](ImVec2 mouse = {-100, -100}, bool pressed = false) {
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(panelSize);
        ImGui::Begin("Text Preview UI");
        browser.DrawFilesPanel(world, mesh, settings);
        ImGui::End(); ImGui::Render();
    };
    try {
        { std::ofstream file(text, std::ios::binary); file << source; }
        { std::ofstream file(script, std::ios::binary); file << "void Surface(inout Material m, float2 uv, float4 t) { m.baseRoughness.xyz = 0; }"; }
        frame(); frame();
        auto* window = ImGui::FindWindowByName("Text Preview UI");
        const auto scope = window->GetID(&browser);
        auto select = [&](const std::string& name) {
            const auto tableId = ImHashStr("ContentFiles", 0, scope);
            ImGui::ActivateItemByID(ImHashStr(name.c_str(), 0, ImHashStr(name.c_str(), 0, tableId)));
            frame(); frame();
        };
        auto previewWindow = [&]() -> ImGuiWindow* {
            const auto id = ImHashStr("TextFilePreview", 0, scope);
            for (auto* candidate : ImGui::GetCurrentContext()->Windows)
                if (candidate->ChildId == id && candidate->Active) return candidate;
            return nullptr;
        };
        select(prefix + ".txt");
        auto* preview = previewWindow();
        Require(preview, "Selecting a text file must display the production preview pane");
        ImGui::ActivateItemByID(preview->GetID("##FileText")); frame();
        auto& input = ImGui::GetCurrentContext()->InputTextState;
        Require((input.Flags & ImGuiInputTextFlags_ReadOnly) && input.TextLen == static_cast<int>(source.size()),
            "The text preview must contain the selected UTF-8 file in a selectable read-only control");
        io.AddInputCharactersUTF8("changed"); frame();
        Require(input.TextLen == static_cast<int>(source.size()) && std::filesystem::file_size(text) == source.size(),
            "Typing in the preview must preserve its contents and the source file");

        // 外部更新在显式刷新后重新显示，长行和多行由正文控件承担滚动。
        std::string updated = "刷新后的中文\n" + std::string(600, 'x') + "\n";
        for (int i = 0; i < 80; ++i) updated += "可滚动的中文正文\n";
        { std::ofstream file(text, std::ios::binary | std::ios::trunc); file << updated; }
        ImGui::ActivateItemByID(ImHashStr("刷新", 0, scope)); frame(); frame();
        preview = previewWindow();
        Require(preview, "Refreshing the directory retains the selected text preview");
        ImGui::ActivateItemByID(preview->GetID("##FileText")); frame();
        Require(input.TextLen == static_cast<int>(updated.size()), "Refresh must replace stale preview text with the updated file");
        bool scrollable = false;
        for (auto* child : ImGui::GetCurrentContext()->Windows)
            if (child->ChildId == preview->GetID("##FileText") && child->Active) scrollable = child->ScrollMax.y > 0;
        Require(scrollable, "Long text must scroll inside the read-only preview control");

        // 收窄面板后改为上下布局，列表和文本都必须保持在父面板可见范围内。
        panelSize = {380, 300}; frame(); frame();
        preview = previewWindow();
        Require(preview && preview->Size.x > 300 && preview->Size.y > 60 && window->ScrollMax.y == 0,
            "Narrow content panels must stack the text preview without an outer scrollbar");
        panelSize = {1000, 600}; frame(); frame();
        auto* fileTable = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("ContentFiles", 0, scope));
        const float listTop = fileTable->OuterRect.Min.y;
        select(prefix + ".hlsl");
        Require(previewWindow(), "Selecting HLSL must preview source without applying it");
        Require(fileTable->OuterRect.Min.y == listTop,
            "Selection actions must not shift file rows between the two clicks of a double-click");

        // 过滤到单行后在真实表格坐标发送双击，覆盖鼠标激活分支。
        ImGui::ActivateItemByID(ImHashStr("##FileSearch", 0, scope)); frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_A, false);
        io.AddInputCharactersUTF8((prefix + ".hlsl").c_str()); frame(); frame();
        auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("ContentFiles", 0, scope));
        Require(table && table->InnerWindow, "Script preview retains the production file table");
        const ImVec2 row{table->WorkRect.Min.x + 24, (table->RowPosY1 + table->RowPosY2) * .5f};
        frame(row, true); frame(row); frame(row, true);
        Require(io.MouseClickedCount[ImGuiMouseButton_Left] == 2, "The script activation regression must send a real double-click");
        frame(row);
        Require(world.Revision() == revision && world.MaterialScripts() == scripts && !settings.reset,
            "Single and double clicking text or scripts must not change the world, material or render history");
        browser.Reset(); frame();
        Require(!previewWindow(), "Replacing the world must discard the previous file preview");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(text, ignored); std::filesystem::remove(script, ignored);
        throw;
    }
    std::filesystem::remove(text); std::filesystem::remove(script);
}

void CheckRenameUi(gameplay::World& world, gameplay::Entity mesh, render::RenderSettings& settings) {
    // 用真实 F2 与右键菜单覆盖源条目选择、失败修正、取消及预览路径更新。
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1000, 600}; io.ConfigInputTrickleEventQueue = false;
    io.Fonts->AddFontDefault();
    Require(io.Fonts->Build(), "Build the rename UI atlas");
    render::editor::EditorContentBrowser browser;
    const auto root = render::DefaultWorldPath().parent_path().parent_path();
    const auto prefix = "RenameUi-" + std::to_string(GetCurrentProcessId());
    const auto file = root / (prefix + ".txt"), renamed = root / (prefix + "-renamed.md");
    const auto folder = root / (prefix + "-folder"), renamedFolder = root / (prefix + "-new-folder");
    const auto occupied = root / (prefix + "-occupied.txt");
    for (const auto& path : {file, renamed, folder, renamedFolder, occupied})
        Require(!std::filesystem::exists(path), "Rename UI fixtures must have unique names");
    const auto revision = world.Revision();
    settings.reset = false;
    auto frame = [&](ImVec2 mouse = {-100, -100}) {
        io.DeltaTime = 1.0f / 60; io.AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({1000, 600});
        ImGui::Begin("Rename UI"); browser.DrawFilesPanel(world, mesh, settings);
        ImGui::End(); ImGui::Render();
    };
    auto cleanup = [&]() {
        std::error_code ignored;
        for (const auto& path : {file, renamed, folder, renamedFolder, occupied}) std::filesystem::remove_all(path, ignored);
    };
    try {
        { std::ofstream output(file, std::ios::binary); output << "改名后的预览"; }
        { std::ofstream output(occupied); output << "keep"; }
        std::filesystem::create_directory(folder);
        { std::ofstream output(folder / "child.txt"); output << "child"; }
        frame(); frame();
        auto* window = ImGui::FindWindowByName("Rename UI");
        const auto scope = window->GetID(&browser);
        auto select = [&](const std::string& name) {
            const auto table = ImHashStr("ContentFiles", 0, scope);
            ImGui::ActivateItemByID(ImHashStr(name.c_str(), 0, ImHashStr(name.c_str(), 0, table)));
            frame(); frame();
        };
        auto f2 = [&]() {
            ImGui::FocusWindow(window);
            io.AddKeyEvent(ImGuiKey_F2, true); frame();
            io.AddKeyEvent(ImGuiKey_F2, false); frame(); frame();
            auto* modal = ImGui::FindWindowByName("###Rename Content Dialog");
            Require(modal && modal->Active, "F2 must open the production rename modal");
            return modal;
        };
        auto confirm = [&](ImGuiWindow* modal, const std::string& name) {
            ImGui::ActivateItemByID(modal->GetID("##Rename Entry Name")); frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_A, false);
            io.AddInputCharactersUTF8(name.c_str()); frame();
            ImGui::ActivateItemByID(modal->GetID("###Confirm Rename")); frame(); frame();
        };
        select(prefix + "-folder");
        auto* modal = f2();
        ImGui::ActivateItemByID(modal->GetID("###Cancel Rename")); frame(); frame();
        Require(!modal->Active && std::filesystem::is_directory(folder), "Cancelling rename must leave the original directory intact");
        confirm(f2(), prefix + "-new-folder");
        Require(std::filesystem::is_regular_file(renamedFolder / "child.txt") && !std::filesystem::exists(folder),
            "F2 must rename nonempty folders through the production UI");

        // 先选目录，再右键另一个文件，必须改名鼠标下的文件而不是旧选择。
        ImGui::ActivateItemByID(ImHashStr("##FileSearch", 0, scope)); frame();
        io.AddInputCharactersUTF8((prefix + ".txt").c_str()); frame(); frame();
        auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("ContentFiles", 0, scope));
        const ImVec2 row{table->WorkRect.Min.x + 24, (table->RowPosY1 + table->RowPosY2) * .5f};
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); frame(row);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); frame(row);
        const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
        Require(!popups.empty() && popups.back().Window, "Right-clicking the file row opens the content menu");
        ImGui::ActivateItemByID(popups.back().Window->GetID("###Rename Content")); frame(); frame();
        modal = ImGui::FindWindowByName("###Rename Content Dialog");
        Require(modal && modal->Active, "The context menu must open the shared rename modal");
        confirm(modal, prefix + "-occupied.txt");
        Require(modal->Active && std::filesystem::exists(file) && render::editor::ReadContentText(occupied) == "keep",
            "Duplicate rename must retain the modal and preserve both files");
        confirm(modal, prefix + "-renamed.md");
        Require(!modal->Active && !std::filesystem::exists(file) && render::editor::ReadContentText(renamed) == "改名后的预览",
            "Correcting a duplicate name renames the right-clicked file and preserves its text");
        const auto previewId = ImHashStr("TextFilePreview", 0, scope);
        ImGuiWindow* preview = nullptr;
        for (auto* candidate : ImGui::GetCurrentContext()->Windows)
            if (candidate->ChildId == previewId && candidate->Active) preview = candidate;
        Require(preview, "Renaming a text file must keep the production preview visible");
        ImGui::ActivateItemByID(preview->GetID("##FileText")); frame();
        Require(ImGui::GetCurrentContext()->InputTextState.TextLen == static_cast<int>(std::string("改名后的预览").size()),
            "The renamed file remains readable at its new path");
        Require(world.Revision() == revision && !settings.reset && browser.ConsumeImportRequest().empty() &&
            browser.ConsumeWorldOpenRequest().empty(), "Renaming content must not load or modify the world");
    } catch (...) { cleanup(); throw; }
    cleanup();
}

void CheckCreationUi(gameplay::World& world, gameplay::Entity mesh, render::RenderSettings& settings) {
    // 使用正式浏览器和原生 ImGui 输入验证菜单、命名、错误恢复与取消，不需要图形设备。
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 600};
    io.ConfigInputTrickleEventQueue = false;
    io.Fonts->AddFontDefault();
    Require(io.Fonts->Build(), "Build the CPU-only ImGui atlas");
    render::editor::EditorContentBrowser browser;
    const auto root = render::DefaultWorldPath().parent_path().parent_path();
    const std::string prefix = "BrowserUi-" + std::to_string(GetCurrentProcessId());
    const auto folder = root / (prefix + "-folder"), file = root / (prefix + ".txt");
    Require(!std::filesystem::exists(folder) && !std::filesystem::exists(file), "UI fixture names must be unused");
    const auto revision = world.Revision();
    ImVec2 panelSize{1000, 600};
    auto frame = [&](ImVec2 mouse = {-100, -100}, bool pressed = false) {
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, pressed);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(panelSize);
        ImGui::Begin("Content Creation UI");
        browser.DrawFilesPanel(world, mesh, settings);
        ImGui::End();
        ImGui::Render();
    };
    try {
        // 文件浏览器直接嵌入独立窗口，内容高度不再被内层页签再次扣除。
        frame(); frame();
        auto* window = ImGui::FindWindowByName("Content Creation UI");
        const auto filesTabId = window->GetID(&browser);
        // 底部面板缩小时不能为尚未选择的文件预留空白操作区，列表仍应容纳多行。
        panelSize.y = 220;
        frame(); frame();
        auto* compactFiles = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("ContentFiles", 0, filesTabId));
        Require(compactFiles && compactFiles->OuterRect.GetHeight() >= ImGui::GetTextLineHeightWithSpacing() * 5,
            "A 220-pixel content panel must retain a header and at least four file rows");
        const float compactHeight = compactFiles->OuterRect.GetHeight();
        panelSize.y = 460;
        frame(); frame();
        Require(compactFiles->OuterRect.GetHeight() >= compactHeight + 230 && window->ScrollMax.y == 0,
            "Growing the parent panel expands the file list without an outer scrollbar");
        panelSize.y = 600;
        frame(); frame();
        auto openCreation = [&](const char* item) {
            ImGui::ActivateItemByID(ImHashStr("###New Content", 0, filesTabId));
            frame();
            const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
            Require(!popups.empty() && popups.back().Window, "New content button opens its real menu");
            ImGui::ActivateItemByID(popups.back().Window->GetID(item));
            frame(); frame();
            auto* modal = ImGui::FindWindowByName("###Create Content");
            Require(modal && modal->Active, "Creation menu opens the shared naming modal");
            return modal;
        };
        auto nameAndCreate = [&](ImGuiWindow* modal, const std::string& name) {
            ImGui::ActivateItemByID(modal->GetID("##New Entry Name"));
            frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiMod_Ctrl, false); io.AddKeyEvent(ImGuiKey_A, false);
            io.AddInputCharactersUTF8(name.c_str()); frame();
            ImGui::ActivateItemByID(modal->GetID("###Create Entry"));
            frame(); frame();
        };
        nameAndCreate(openCreation("###New Folder"), prefix + "-folder");
        Require(std::filesystem::is_directory(folder), "Folder creation reaches the filesystem through the production UI");
        auto* modal = openCreation("###New File");
        nameAndCreate(modal, prefix + "-folder");
        Require(modal->Active && std::filesystem::is_directory(folder), "Duplicate names retain the modal and existing directory");
        nameAndCreate(modal, prefix + ".txt");
        Require(std::filesystem::is_regular_file(file) && std::filesystem::file_size(file) == 0,
            "Correcting a failed filename creates an empty file through the real UI");
        // 在实际滚动列表中发送右键事件，验证另一入口也归属同一个命名对话框。
        auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("ContentFiles", 0, filesTabId));
        Require(table && table->InnerWindow, "Project files use the production scrolling table");
        const auto blank = table->InnerWindow->InnerRect.GetCenter();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); frame(blank);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); frame(blank);
        const auto& contextPopups = ImGui::GetCurrentContext()->OpenPopupStack;
        Require(!contextPopups.empty() && contextPopups.back().Window, "Right-clicking the content list opens its creation menu");
        ImGui::ActivateItemByID(contextPopups.back().Window->GetID("###New Gameplay Script"));
        frame(); frame();
        modal = ImGui::FindWindowByName("###Create Content");
        Require(modal && modal->Active, "The context menu opens the shared creation modal");
        ImGui::ActivateItemByID(modal->GetID("###Cancel Create"));
        frame(); frame();
        Require(!modal->Active && world.Revision() == revision,
            "Cancelling creation closes the modal without changing the scene or its undo state");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored); std::filesystem::remove(folder, ignored);
        throw;
    }
    std::filesystem::remove(file); std::filesystem::remove(folder);
}
}

int main() {
    try {
        // 枚举按作者名称顺序借用实时资源，不改变其所有者。
        engine::ResourceStore resources;
        const auto late = resources.Create<render::MaterialResource>("Zinc");
        const auto early = resources.Create<render::MaterialResource>("Aluminum");
        const auto same = resources.Create<render::MaterialResource>("Aluminum");
        const auto all = resources.All();
        Require(all.size() == 3 && all[0] == resources.Resolve(early) && all[1] == resources.Resolve(same) &&
            all[2] == resources.Resolve(late), "Resource browser enumeration must be stable, sorted and borrowed");

        // FBX 与 glTF 共用一次性导入队列，保留中文路径并忽略扩展名大小写。
        render::editor::EditorContentBrowser browser;
        const std::filesystem::path fbx = L"D:/模型/测试.FBX";
        Require(browser.QueueModelImport(fbx) && browser.ConsumeImportRequest() == fbx.wstring() &&
            browser.ConsumeImportRequest().empty(), "FBX model imports retain Unicode paths and are consumed once");
        const std::filesystem::path gltf = L"D:/模型/旧场景.glb";
        Require(browser.QueueModelImport(gltf) && !browser.QueueModelImport(L"D:/模型/无关.txt") &&
            browser.ConsumeImportRequest() == gltf.wstring(), "Unsupported files must preserve the queued glTF request");

        // 双击世界配置使用独立请求，既保持 Unicode 也不会误入模型解析器。
        const std::filesystem::path worldFile = L"D:/场景/角色.pb";
        Require(browser.QueueWorldOpen(worldFile) && browser.ConsumeImportRequest().empty() &&
            browser.ConsumeWorldOpenRequest() == worldFile.wstring() && browser.ConsumeWorldOpenRequest().empty(),
            "World files must use a separate one-shot open queue");
        Require(browser.QueueWorldOpen(L"D:/场景/角色.textproto") && !browser.QueueWorldOpen(L"D:/模型/角色.fbx"),
            "World queue must accept formal documents and reject model files");
        browser.Reset();
        Require(browser.ConsumeWorldOpenRequest().empty(), "World replacement must discard stale browser open requests");

        // 现有材质槽允许重新分配，但解析发光体拥有的几何保持受保护。
        gameplay::World world;
        world.CreateCornell(render::BuildCornellScene());
        gameplay::Entity mesh = entt::null, emitter = entt::null;
        for (auto entity : world.Registry().view<gameplay::MeshRenderer>()) {
            const auto& renderer = world.Registry().get<gameplay::MeshRenderer>(entity);
            (renderer.lightOwned ? emitter : mesh) = entity;
        }
        Require(mesh != entt::null && emitter != entt::null, "Cornell fixture must contain ordinary and light-owned geometry");
        render::RenderSettings settings;
        settings.reset = false;
        const auto materialCount = world.Materials().size();
        const auto oldIndex = world.Registry().get<gameplay::MeshRenderer>(mesh).materialIndex;
        const auto newIndex = (oldIndex + 1u) % static_cast<unsigned>(materialCount);
        const auto revision = world.Revision();
        Require(render::editor::ApplyMaterialAsset(world, mesh, newIndex, settings), "A valid existing material must apply to a mesh");
        Require(world.Revision() == revision + 1 && settings.reset && world.Materials().size() == materialCount &&
            world.Registry().get<gameplay::MeshRenderer>(mesh).materialIndex == newIndex,
            "Material assignment must invalidate render history without creating new GPU material slots");
        Require(!render::editor::ApplyMaterialAsset(world, mesh, newIndex, settings), "Applying the same slot must remain a no-op");
        Require(!render::editor::ApplyMaterialAsset(world, mesh, ~0u, settings) &&
            !render::editor::ApplyMaterialAsset(world, world.CameraEntity(), 0, settings) &&
            !render::editor::ApplyMaterialAsset(world, emitter, 0, settings), "Invalid slots, non-meshes and analytic emitters must reject assignment");

        // 与正式世界定位共用发布目录或源码回退，CTest 的工作目录不能改变示例来源。
        const auto content = render::DefaultWorldPath().parent_path().parent_path() / L"Scripts";
        std::string error;
        Require(render::editor::LoadContentScript(world, mesh, content / L"example.hlsl", settings, error),
            "The shipped material example must load into the selected mesh material");
        Require(!world.Material(newIndex)->ScriptSource().empty() && !world.Material(newIndex)->ScriptPath().empty(),
            "Loaded material source must preserve its authoring provenance");
        Require(!render::editor::LoadContentScript(world, emitter, content / L"example.hlsl", settings, error),
            "A light-owned material script must remain protected");
        Require(render::editor::LoadContentScript(world, mesh, content / L"example.lua", settings, error),
            "The shipped Lua example must attach to the selected entity");
        gameplay::ScriptSystem scripts;
        const auto result = scripts.Compile(world, mesh);
        Require(result.success, "The shipped Lua example must compile using the registered ECS API");

        // 临时文件按引擎与进程隔离，并覆盖 Unicode 路径、超大脚本和非 UTF-8 文本拒绝。
        const auto temporary = std::filesystem::temp_directory_path() /
            (L"EncoreBrowser-\u6d4b\u8bd5-" + std::to_wstring(GetCurrentProcessId()) + L".hlsl");
        const auto previous = world.Material(newIndex)->ScriptSource();
        {
            std::ofstream file(temporary, std::ios::binary);
            const std::string oversized(1024 * 1024 + 1, ' ');
            file.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
        }
        Require(!render::editor::LoadContentScript(world, mesh, temporary, settings, error) &&
            world.Material(newIndex)->ScriptSource() == previous && !error.empty(),
            "Oversized Unicode-path input must report a recoverable error without replacing the script");
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.put(static_cast<char>(0xff));
        }
        Require(!render::editor::LoadContentScript(world, mesh, temporary, settings, error) &&
            world.Material(newIndex)->ScriptSource() == previous, "Invalid UTF-8 source must not reach the script editor");
        std::filesystem::remove(temporary);

        // 新建操作使用引擎专属临时 Content，覆盖中文名称且不污染正式资源或旧版测试目录。
        const auto creationRoot = std::filesystem::temp_directory_path() /
            (L"EncoreContentCreate-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(creationRoot);
        const auto folder = render::editor::CreateContentEntry(creationRoot, creationRoot, "脚本目录",
            render::editor::ContentEntryKind::Folder);
        Require(std::filesystem::is_directory(folder) && folder.filename() == L"脚本目录",
            "Creating a Unicode folder must produce a real directory inside Content");
        const auto emptyFile = render::editor::CreateContentEntry(creationRoot, folder, "说明.txt",
            render::editor::ContentEntryKind::File);
        Require(std::filesystem::is_regular_file(emptyFile) && std::filesystem::file_size(emptyFile) == 0,
            "Creating a regular file must target the chosen subdirectory");
        const auto materialFile = render::editor::CreateContentEntry(creationRoot, folder, "表面",
            render::editor::ContentEntryKind::MaterialScript);
        Require(materialFile.extension() == L".hlsl" &&
            render::editor::LoadContentScript(world, mesh, materialFile, settings, error) &&
            world.Material(newIndex)->ScriptSource().find("void Surface(") != std::string::npos,
            "New HLSL scripts must include the supported material entry point");
        const auto luaFile = render::editor::CreateContentEntry(creationRoot, folder, "行为.LUA",
            render::editor::ContentEntryKind::GameplayScript);
        Require(render::editor::LoadContentScript(world, mesh, luaFile, settings, error) &&
            scripts.Compile(world, mesh).success, "New Lua templates must compile through the real gameplay runtime");

        // 独占创建拒绝重名，不能截断作者已有的内容或把目录替换成文件。
        const auto originalBytes = std::filesystem::file_size(materialFile);
        auto rejectCreation = [&](const std::filesystem::path& directory, std::string_view name,
            render::editor::ContentEntryKind kind = render::editor::ContentEntryKind::File) {
            bool rejected = false;
            try { (void)render::editor::CreateContentEntry(creationRoot, directory, name, kind); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid or duplicate content entries must be rejected before replacement");
        };
        rejectCreation(folder, "表面.hlsl");
        rejectCreation(creationRoot, "脚本目录");
        rejectCreation(folder, "说明.txt", render::editor::ContentEntryKind::Folder);
        rejectCreation(creationRoot.parent_path(), "outside.txt");
        for (const auto* name : {"", "..", "../outside.txt", "nested/file.txt", "nested\\file.txt", "file:stream", "CON", "nul.txt",
            "COM1.lua", "name.", "name ", "bad?.txt"}) rejectCreation(folder, name);
        rejectCreation(folder, std::string(1, static_cast<char>(0xff)));
        rejectCreation(folder, std::string_view("bad\0name", 8));
        rejectCreation(folder, "wrong.txt", render::editor::ContentEntryKind::MaterialScript);
        Require(std::filesystem::file_size(materialFile) == originalBytes &&
            std::filesystem::is_directory(folder), "Rejected operations must preserve existing files and folders");
        CheckTextReading(folder);
        CheckRenaming(creationRoot);
        std::filesystem::remove_all(creationRoot);
        CheckTextPreviewUi(world, mesh, settings);
        CheckCreationUi(world, mesh, settings);
        CheckRenameUi(world, mesh, settings);
        std::cout << "Content browser resource assignment, script examples and file validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
