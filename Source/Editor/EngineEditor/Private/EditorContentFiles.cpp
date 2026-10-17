#include "editor/EditorContentFiles.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string>

namespace render::editor {
namespace {
std::wstring EntryName(std::string_view text) {
    // 先严格解码 UTF-8，再按 Windows 单个文件名规则检查，防止路径穿越与 ADS。
    if (text.empty() || text.size() > 1024 || text.find('\0') != std::string_view::npos)
        throw std::runtime_error("名称不能为空或过长，也不能包含空字符");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count || count > 255) throw std::runtime_error("名称必须是有效 UTF-8，且不超过 255 个 UTF-16 单元");
    std::wstring name(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), name.data(), count) != count)
        throw std::runtime_error("无法转换文件名称");
    if (name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
        throw std::runtime_error("名称不能包含路径分隔符或非法字符，也不能以点或空格结尾");

    // 设备名称即使带扩展名也不能用作普通文件，避免生成无法正常访问的内容。
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    const bool numberedDevice = base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
        ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3');
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" ||
        base == L"CONOUT$" || numberedDevice)
        throw std::runtime_error("名称属于 Windows 保留设备名，请更换名称");
    return name;
}

std::filesystem::path ContentParent(const std::filesystem::path& contentRoot, const std::filesystem::path& directory) {
    // 创建与重命名共用真实目录边界，目录链接不能将操作引向 Content 之外。
    const auto root = std::filesystem::canonical(contentRoot);
    const auto parent = std::filesystem::canonical(directory);
    if (!std::filesystem::is_directory(root) || !std::filesystem::is_directory(parent))
        throw std::runtime_error("内容根目录和当前路径必须是已有文件夹");
    for (auto ancestor = parent; !std::filesystem::equivalent(ancestor, root);) {
        const auto next = ancestor.parent_path();
        if (next == ancestor) throw std::runtime_error("只能在项目 Content 目录内修改内容");
        ancestor = next;
    }
    return parent;
}

std::string_view InitialSource(ContentEntryKind kind) {
    // 新脚本直接采用现有运行时入口，默认行为不改变对象或材质。
    switch (kind) {
    case ContentEntryKind::Folder:
    case ContentEntryKind::File: return {};
    case ContentEntryKind::MaterialScript:
        return "// 在此修改所选物体的材质参数。\n"
               "void Surface(inout Material material, float2 uv, float4 tangent)\n"
               "{\n    material.baseRoughness.w = saturate(material.baseRoughness.w);\n}\n";
    case ContentEntryKind::GameplayScript:
        return "-- 每次模拟更新时执行当前实体的游戏逻辑。\n"
               "function update(self, dt)\nend\n";
    }
    throw std::runtime_error("不支持的内容创建类型");
}
}

bool IsTextContentFile(const std::filesystem::path& path) {
    // 只按 ASCII 扩展名忽略大小写，中文文件名保持原样。
    auto extension = path.extension().wstring();
    for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    for (const auto* known : {L".txt", L".md", L".json", L".ini", L".cfg", L".csv", L".log", L".xml",
        L".yaml", L".yml", L".hlsl", L".hlsli", L".lua", L".proto", L".textproto", L".gltf",
        L".c", L".cpp", L".h", L".hpp", L".py", L".cmake"})
        if (extension == known) return true;
    auto name = path.filename().wstring();
    for (auto& c : name) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return name == L"readme" || name == L"license" || name == L"cmakelists.txt";
}

std::string ReadContentText(const std::filesystem::path& path) {
    // 先限制磁盘读取量，再一次性建立快照，避免每帧读取或为大文件分配无界内存。
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0) throw std::runtime_error("无法读取文本文件");
    if (file.tellg() > 1024 * 1024) throw std::runtime_error("文本文件超过 1 MiB 预览限制");
    std::string text(static_cast<std::size_t>(file.tellg()), '\0');
    file.seekg(0);
    if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("文本文件读取不完整");

    // Windows 文本的 UTF-16 必须带 BOM；按明确字节序转换并严格拒绝损坏的代理项。
    const bool little = text.starts_with("\xff\xfe"), big = text.starts_with("\xfe\xff");
    if (little || big) {
        if (text.size() % 2) throw std::runtime_error("UTF-16 文本包含不完整的字符");
        std::wstring wide((text.size() - 2) / 2, L'\0');
        for (std::size_t i = 0; i < wide.size(); ++i) {
            const auto a = static_cast<unsigned char>(text[2 + i * 2]);
            const auto b = static_cast<unsigned char>(text[3 + i * 2]);
            wide[i] = static_cast<wchar_t>(little ? a | (b << 8) : (a << 8) | b);
        }
        const auto size = wide.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (!wide.empty() && !size) throw std::runtime_error("UTF-16 文本包含无效的 Unicode 字符");
        text.resize(static_cast<std::size_t>(size));
        if (size && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
            text.data(), size, nullptr, nullptr) != size) throw std::runtime_error("无法转换 UTF-16 文本");
    } else {
        // UTF-8 BOM 不属于正文；无 BOM 的文本也必须严格通过 UTF-8 校验。
        if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
        if (!text.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0))
            throw std::runtime_error("文本必须使用 UTF-8 或带 BOM 的 UTF-16 编码");
    }
    // 空字符与二进制控制字节不能交给以零结尾的 ImGui 文本控件，避免静默截断。
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 && c != '\t' && c != '\r' && c != '\n'; }))
        throw std::runtime_error("文件包含二进制数据，无法作为文本预览");
    return text;
}

std::filesystem::path CreateContentEntry(const std::filesystem::path& contentRoot,
    const std::filesystem::path& directory, std::string_view utf8Name, ContentEntryKind kind) {
    auto name = EntryName(utf8Name);
    const auto source = InitialSource(kind);
    // 模板自动补全省略的扩展名，明确写错扩展名则保留输入并提示用户修改。
    const wchar_t* expected = kind == ContentEntryKind::MaterialScript ? L".hlsl" :
        kind == ContentEntryKind::GameplayScript ? L".lua" : nullptr;
    if (expected) {
        auto extension = std::filesystem::path(name).extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension.empty()) name += expected;
        else if (extension != expected) throw std::runtime_error("脚本扩展名与所选类型不一致");
        if (name.size() > 255) throw std::runtime_error("补全扩展名后的文件名过长");
    }

    // 写入之前解析实际父目录，创建和重命名遵循相同内容边界。
    const auto parent = ContentParent(contentRoot, directory);
    const auto target = parent / name;
    if (kind == ContentEntryKind::Folder) {
        std::error_code failure;
        if (!std::filesystem::create_directory(target, failure)) {
            std::error_code lookup;
            if (!failure || std::filesystem::exists(target, lookup))
                throw std::runtime_error("已存在同名文件或文件夹");
            throw std::runtime_error("创建文件夹失败：" + failure.message());
        }
        return target;
    }

    // CREATE_NEW 保证重名检查与创建不可分割，已有文件、目录或链接一律不会被截断。
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        // 与已有目录重名时 Win32 可能返回拒绝访问，补查存在状态以给出准确提示。
        std::error_code lookup;
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS || std::filesystem::exists(target, lookup))
            throw std::runtime_error("已存在同名文件或文件夹");
        throw std::runtime_error("创建文件失败，错误代码：" + std::to_string(code));
    }
    DWORD written = 0;
    const bool success = (source.empty() || (WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) &&
        written == source.size())) && FlushFileBuffers(file);
    if (!success) {
        // 写入失败时按已打开的文件句柄回收本次新文件，避免留下半份模板。
        const auto code = GetLastError();
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        CloseHandle(file);
        throw std::runtime_error("写入新文件失败，错误代码：" + std::to_string(code));
    }
    if (!CloseHandle(file)) throw std::runtime_error("关闭新文件失败，错误代码：" + std::to_string(GetLastError()));
    return target;
}

std::filesystem::path RenameContentEntry(const std::filesystem::path& contentRoot,
    const std::filesystem::path& entry, std::string_view utf8Name) {
    // 保留父目录和条目身份；源路径不能用根目录、点或父目录别名绕过检查。
    const auto name = EntryName(utf8Name);
    if (entry.filename().empty() || entry.filename() == L"." || entry.filename() == L"..")
        throw std::runtime_error("请选择要重命名的文件或文件夹");
    const auto parent = ContentParent(contentRoot, entry.parent_path());
    const auto source = parent / entry.filename();
    if (!std::filesystem::exists(source)) throw std::runtime_error("要重命名的条目已不存在");
    if (std::filesystem::equivalent(source, std::filesystem::canonical(contentRoot)))
        throw std::runtime_error("不能重命名项目 Content 根目录");
    const auto target = parent / name;
    if (source.filename().native() == name) return target;

    // 不设置 REPLACE_EXISTING，由同目录原子改名拒绝重名竞争，同时支持仅修改大小写。
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::error_code lookup;
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS || std::filesystem::exists(target, lookup))
            throw std::runtime_error("已存在同名文件或文件夹");
        throw std::runtime_error("重命名失败，错误代码：" + std::to_string(code));
    }
    return target;
}
}
