#pragma once
#include "gameplay/ScriptSystem.h"
#include "render/MaterialScripts.h"
#include <vector>

namespace render { struct RenderSettings; }
namespace gameplay { class World; }
namespace render::editor {
// 独立脚本窗口只拥有编辑界面状态，脚本执行保留在运行时模块中。
class EditorScriptUi {
public:
    void DrawMaterial(gameplay::World&, gameplay::Entity);
    void DrawGameplay(gameplay::World&, gameplay::Entity, RenderSettings&);
    void Observe(gameplay::World&);
    gameplay::ScriptSystem& Gameplay() { return gameplay_; }
    bool ConsumeMaterialCompileRequest();
    void SetMaterialCompileResult(MaterialCompileResult result) { materialResult_ = std::move(result); }
    void SetMaterialCompilePending() { materialResult_ = {true, "正在编译材质..."}; }
    void RequestMaterialCompile() { compileRequested_ = true; }
    void Reset();
private:
    gameplay::ScriptSystem gameplay_; // 各实体隔离的虚拟机，暂停状态独立于源码编辑缓冲区。
    std::vector<MaterialShaderSource> observed_; // 最近观察的源码状态，用于识别撤销、重做和文本变化。
    MaterialCompileResult materialResult_; // 最近的 DXC 或管线结果，独立于当前可编辑草稿。
    std::string fileError_; // 可恢复的本地源码文件错误，使用 UTF-8 编码。
    double editTime_{}; // 最近源码编辑时间，用于合并连续输入后的实时编译请求。
    bool pendingEdit_{}; // 是否有源码变化正在等待实时预览的延迟触发点。
    bool compileRequested_{}; // ImGui 编辑帧结束后由宿主消费的一次性编译请求。
    bool livePreview_{true}; // 输入稳定后自动编译，失败时继续显示上一版可用预览。
};
}
