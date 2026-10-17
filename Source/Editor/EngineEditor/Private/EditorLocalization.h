#pragma once

#include <string_view>

namespace render::editor {
// 在 RDG 字体上传创建图集纹理前配置 UTF-8 编辑器文本。
void ConfigureEditorLanguage();

// 翻译已知渲染图标签，不改变运行时阶段标识或抓帧内容。
const char* EditorPassName(std::string_view identifier);
}
