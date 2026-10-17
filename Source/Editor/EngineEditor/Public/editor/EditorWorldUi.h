#pragma once
#include "gameplay/World.h"
#include <optional>
#include <string>
#include <vector>

namespace render::editor {
enum class WorldUiRequestKind {
    Open,        // 从用户选择的官方 Protobuf 文档替换世界。
    Save,        // 将当前世界编辑导出到用户选择的文档路径。
    SwitchScene, // 保存内存编辑后激活同一文档中的目标场景。
};
struct WorldUiRequest {
    WorldUiRequestKind kind{WorldUiRequestKind::Open}; // 宿主在安全帧边界执行的操作。
    std::wstring path;                                 // 打开或保存的原生 Windows 路径。
    std::string sceneId;                               // 切换场景时使用的稳定场景标识。
};
class EditorWorldUi {
  public:
    // 常驻入口提供项目世界列表与外部文档选择，仍只排队一次性请求。
    void DrawSceneSelector();
    // 填充调用者 WorldPanel 顶部，不直接修改 World 或 GPU 资源。
    void Draw(const gameplay::World &world);
    std::optional<WorldUiRequest> ConsumeRequest();
    void Reset();

  private:
    void RequestOpen();
    std::optional<WorldUiRequest> request_; // 等待宿主消费的一次性世界操作。
    std::wstring lastPath_;                 // 文件对话框默认位置，不代表当前世界已成功保存。
    std::string error_;                     // 原生文件对话框失败的中文诊断。
    std::vector<std::filesystem::path> sceneFiles_; // 仅在打开选择器时刷新，避免每帧扫描项目目录。
};
}
