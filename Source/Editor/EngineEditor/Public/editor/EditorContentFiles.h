#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace render::editor {
enum class ContentEntryKind {
    Folder,         // 当前内容目录下的新子目录。
    File,           // 保留用户所填扩展名的空文件。
    MaterialScript, // 带 Surface 入口的 HLSL 材质脚本。
    GameplayScript, // 带 update 入口的 Lua 游戏脚本。
};

// 独占创建不覆盖已有条目，名称只允许单个文件名，父目录必须位于 Content 内。
std::filesystem::path CreateContentEntry(const std::filesystem::path& contentRoot,
    const std::filesystem::path& directory, std::string_view utf8Name, ContentEntryKind kind);
// 只在原目录内重命名已有条目，不覆盖同名目标，也不允许改动 Content 根目录。
std::filesystem::path RenameContentEntry(const std::filesystem::path& contentRoot,
    const std::filesystem::path& entry, std::string_view utf8Name);
// 文本类型只影响只读预览，不参与模型导入、世界打开或脚本应用。
bool IsTextContentFile(const std::filesystem::path& path);
// 限制读取容量，严格转换 UTF-8/UTF-16 文本，二进制或损坏编码以异常报告。
std::string ReadContentText(const std::filesystem::path& path);
}
