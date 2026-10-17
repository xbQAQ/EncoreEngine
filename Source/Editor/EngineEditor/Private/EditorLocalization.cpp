#include "EditorLocalization.h"
#include <imgui.h>
#include <Windows.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace render::editor {
void ConfigureEditorLanguage() {
    wchar_t directory[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) throw std::runtime_error("Cannot locate Windows fonts for the Chinese editor");
    auto& io = ImGui::GetIO();
    // 通过宽字符路径读取，兼容 Windows 位于非 ASCII 目录的安装。
    for (const auto* filename : {L"msyh.ttc", L"simhei.ttf", L"simsun.ttc"}) {
        std::ifstream input(std::filesystem::path(directory) / L"Fonts" / filename, std::ios::binary | std::ios::ate);
        if (!input) continue;
        const auto bytes = input.tellg();
        if (bytes <= 0 || bytes > std::numeric_limits<int>::max()) continue;
        auto* data = IM_ALLOC(static_cast<std::size_t>(bytes));
        if (!data) throw std::bad_alloc();
        input.seekg(0);
        if (!input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes))) { IM_FREE(data); continue; }

        // 字体图集拥有该缓冲区，完整基本 CJK 字形同时支持用户输入的中文名称。
        ImFontConfig config;
        config.OversampleH = config.OversampleV = 1;
        io.Fonts->TexDesiredWidth = 4096;
        io.FontDefault = io.Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes), 17.0f,
            &config, io.Fonts->GetGlyphRangesChineseFull());
        return;
    }
    throw std::runtime_error("Chinese editor requires Microsoft YaHei, SimHei, or SimSun in Windows Fonts");
}

const char* EditorPassName(std::string_view identifier) {
    // 本地化仅用于编辑器显示，渲染器保留稳定的诊断和依赖名称。
    static constexpr std::array labels{
        std::pair{"Raster.GBuffer", "光栅几何缓冲"},
        std::pair{"Lighting.Direct", "直接光照"},
        std::pair{"GI.PathTracing", "路径追踪"},
        std::pair{"GI.SSGI", "屏幕空间全局光照"},
        std::pair{"Sky.Background", "天空背景"},
        std::pair{"Effects.SSAO", "屏幕空间环境遮蔽"},
        std::pair{"Effects.SSR", "屏幕空间反射"},
        std::pair{"Effects.SSIL", "屏幕空间间接光"},
        std::pair{"Effects.SSS.Horizontal", "次表面水平扩散"},
        std::pair{"Effects.SSS.Vertical", "次表面垂直扩散"},
        std::pair{"ReSTIR.Candidates", "ReSTIR 候选生成"},
        std::pair{"ReSTIR.Temporal", "ReSTIR 时间复用"},
        std::pair{"ReSTIR.Spatial", "ReSTIR 空间复用"},
        std::pair{"ReSTIR.Evaluate", "ReSTIR 光照评估"},
        std::pair{"Resolve.Temporal", "时间重建"},
        std::pair{"Resolve.Spatial", "空间重建"},
        std::pair{"Post.VolumeBloomACES", "体积、泛光与色调映射"},
        std::pair{"History.GI", "保存全局光照历史"},
        std::pair{"History.Position", "保存位置历史"},
        std::pair{"History.Normal", "保存法线历史"},
        std::pair{"History.Resolved", "保存重建历史"},
        std::pair{"History.Reservoir", "保存蓄水池历史"},
        std::pair{"Shadow.RasterDepth", "阴影深度"},
        std::pair{"Shadow.BlurHorizontal", "阴影水平模糊"},
        std::pair{"Shadow.BlurVertical", "阴影垂直模糊"},
        std::pair{"Editor.ImGui", "编辑器界面叠加"},
        std::pair{"Editor.UploadFonts", "上传界面字体"},
        std::pair{"DXR.BuildBLAS", "构建底层加速结构"},
        std::pair{"DXR.BuildTLAS", "构建顶层加速结构"}
    };
    for (const auto& [key, value] : labels) if (identifier == key) return value;
    return nullptr;
}
}
