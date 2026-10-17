#pragma once

#include "gameplay/Components.h"
#include "editor/EditorContentFiles.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace gameplay { class World; }
namespace render { struct RenderSettings; }
namespace render::editor {
// 资源列表、属性目标及视口材质拖放共用同一分配操作。
bool ApplyMaterialAsset(gameplay::World&, gameplay::Entity, unsigned materialIndex, RenderSettings&);
// 将 UTF-8 源码加载到现有资源或组件，此处不执行编译或运行。
bool LoadContentScript(gameplay::World&, gameplay::Entity, const std::filesystem::path&, RenderSettings&, std::string& error);

class EditorContentBrowser {
public:
    void DrawResourcesPanel(gameplay::World&, gameplay::Entity selection, RenderSettings&);
    void DrawFilesPanel(gameplay::World&, gameplay::Entity selection, RenderSettings&);
    std::wstring ConsumeImportRequest();
    std::wstring ConsumeWorldOpenRequest();
    // 校验支持的模型扩展名并排队路径，解析仍由帧结束后的宿主执行。
    bool QueueModelImport(const std::filesystem::path& path);
    // 世界文件同样延迟到帧结束打开，失败不能在浏览器遍历中替换注册表。
    bool QueueWorldOpen(const std::filesystem::path& path);
    void Reset();

private:
    struct FileEntry {
        std::filesystem::path path; // 项目 Content 目录内的原生 Unicode 文件路径。
        std::string name; // 过滤器及 ImGui 标签使用的已校验 UTF-8 文件名。
        std::uintmax_t bytes{}; // 元数据使用的文件长度，目录保持为零。
        bool directory{}; // 双击时是否进入目录，文件则执行激活操作。
    };
    void DrawResources(gameplay::World&, gameplay::Entity, RenderSettings&);
    void DrawFiles(gameplay::World&, gameplay::Entity, RenderSettings&);
    void DrawError();
    void PreviewFile(const std::filesystem::path&);
    void DrawTextPreview(float width, float height);
    void RefreshDirectory();
    void ActivateFile(const std::filesystem::path&, bool directory);
    void DrawCreateMenu();
    void DrawCreateDialog();
    void BeginCreate(ContentEntryKind kind);
    void BeginRename(const std::filesystem::path&);
    void DrawRenameDialog();

    std::array<char, 256> resourceSearch_{}; // 对资源名称和类型执行 UTF-8 子串过滤。
    std::array<char, 256> fileSearch_{}; // 对当前目录文件名执行 UTF-8 子串过滤。
    std::uint64_t selectedResource_{}; // 借用的资源标识，World 变化后逐帧重新解析。
    int resourceType_{}; // 工具栏选择的全部、材质、纹理或网格过滤类型。
    std::filesystem::path contentRoot_; // 从程序目录或源资源目录确定的规范化项目内容边界。
    std::filesystem::path directory_; // 受项目内容边界约束的当前目录。
    std::filesystem::path selectedFile_; // 当前选中行的原生路径，不依赖缓存项地址。
    std::filesystem::path previewFile_; // 当前只读预览的文本路径，目录或二进制选择时清除。
    std::string previewText_; // 选中或刷新时读取的 UTF-8 正文快照，不写回源文件。
    std::string previewError_; // 文件过大、读取失败或编码损坏时在预览区显示的诊断。
    std::vector<FileEntry> files_; // 排序后的目录快照，仅在导航或显式刷新时更新。
    std::wstring importRequest_; // 交由编辑器统一模型导入流程处理的延迟请求。
    std::wstring worldOpenRequest_; // 交由宿主正式世界文档加载流程处理的延迟路径。
    std::string error_; // 可恢复的 UTF-8 浏览器错误，不丢弃当前 World。
    bool directoryDirty_{true}; // 延迟到下一次遍历再更新目录，保持当前行引用有效。
    ContentEntryKind createKind_{ContentEntryKind::Folder}; // 新建对话框当前创建的文件或目录类型。
    std::array<char, 1024> newEntryName_{}; // 用户输入的 UTF-8 名称，创建失败时保留草稿。
    std::string creationError_; // 仅在新建对话框显示的名称、重名或磁盘写入诊断。
    bool creationRequested_{}; // 在菜单遍历结束后，由父窗口统一打开模态对话框。
    bool focusCreationName_{}; // 对话框首次显示时将键盘焦点交给名称输入。
    bool revealCreated_{}; // 下次目录列表绘制时滚动到刚创建并选中的条目。
    std::filesystem::path renameSource_; // 打开对话框时固定的源条目，不随列表选择变化。
    std::array<char, 1024> renameName_{}; // 带扩展名的 UTF-8 名称，失败后保留以便修正。
    std::string renameError_; // 名称无效、重名或操作系统拒绝访问时的局部诊断。
    bool renameRequested_{}; // 离开菜单 ID 栈后再打开父窗口的重命名对话框。
    bool focusRenameName_{}; // 首次显示时聚焦并选中完整名称。
};
}
