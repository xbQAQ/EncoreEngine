// 材质层负责源码与着色器变体编译，管线创建留给调用方。
#include "Materials/MaterialShaderCompiler.h"
#include "dx12/Dx12Common.h"
#include <dxcapi.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace render {
namespace {
using namespace dx12;
class CompilerLibrary {
public:
    CompilerLibrary() {
        const auto path = std::filesystem::path(RENDER_DXC_EXECUTABLE).parent_path() / L"dxcompiler.dll";
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) throw std::runtime_error("Cannot load DXC compiler library");
        create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module_, "DxcCreateInstance"));
        if (!create) { FreeLibrary(module_); module_ = nullptr; throw std::runtime_error("DXC create function is unavailable"); }
    }
    ~CompilerLibrary() { if (module_) FreeLibrary(module_); }
    DxcCreateInstanceProc create{}; // 仅在所拥有的动态库保持加载期间借用入口函数。
private:
    HMODULE module_{}; // 与离线着色器构建一致的 SDK 编译器动态库。
};

class MaterialIncludes final : public IDxcIncludeHandler {
public:
    MaterialIncludes(IDxcUtils* utils, std::string source) : utils_(utils), source_(std::move(source)) {
        Check(utils_->CreateDefaultIncludeHandler(&fallback_), "DXC default include handler");
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDxcIncludeHandler)) { *output = this; AddRef(); return S_OK; }
        *output = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** output) override {
        if (!output) return E_POINTER;
        // 只有生成的材质分发头使用虚拟源码，引擎头文件沿用 DXC 的文件查找规则。
        if (std::filesystem::path(filename).filename() == L"MaterialScripts.generated.hlsli") {
            ComPtr<IDxcBlobEncoding> blob;
            const auto result = utils_->CreateBlob(source_.data(), static_cast<UINT32>(source_.size()), DXC_CP_UTF8, &blob);
            if (FAILED(result)) return result;
            *output = blob.Detach(); return S_OK;
        }
        return fallback_->LoadSource(filename, output);
    }
private:
    std::atomic<ULONG> references_{1}; // 同步编译过程中共享的 COM 引用计数。
    ComPtr<IDxcUtils> utils_; // 通过保留 COM 引用借用二进制块工厂。
    ComPtr<IDxcIncludeHandler> fallback_; // 引擎着色器头文件使用的原始文件解析器。
    std::string source_; // 候选脚本函数及材质槽分发源码，不写入项目源目录。
};

std::string Generate(std::span<const MaterialShaderSource> scripts) {
    std::ostringstream output;
    for (std::size_t index = 0; index < scripts.size(); ++index) {
        output << "namespace MaterialScript" << index << " {\n#line 1 \"Material_" << scripts[index].materialIndex << ".hlsl\"\n";
        // 文件开头的 UTF-8 BOM 不能嵌入生成头文件中间，但不应改变作者源码及其行号。
        std::string_view source = scripts[index].source;
        if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
        output << source << "\n}\n";
    }
    output << "#line 1 \"MaterialScripts.generated.hlsli\"\nvoid ApplyMaterialScript(uint index, inout Material material, float2 uv, float4 tangent) {\n";
    for (std::size_t index = 0; index < scripts.size(); ++index)
        output << "if (index == " << scripts[index].materialIndex << "u) { MaterialScript" << index << "::Surface(material, uv, tangent); return; }\n";
    output << "}\n";
    return output.str();
}
}
MaterialShaderBundle CompileMaterialShaders(std::span<const MaterialShaderSource> sources, bool dxrSupported) {
    MaterialShaderBundle bundle;
    try {
        CompilerLibrary library;
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        Check(library.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DXC utilities");
        Check(library.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DXC compiler");
        ComPtr<IDxcIncludeHandler> includes;
        includes.Attach(new MaterialIncludes(utils.Get(), Generate(sources)));
        const auto root = std::filesystem::path(RENDER_SHADER_SOURCE_DIRECTORY);
        const auto common = (root / L"Common").wstring();
        auto compile = [&](const wchar_t* file, const std::wstring& key, const wchar_t* entry, const wchar_t* profile, unsigned backend) {
            const auto path = root / (std::wstring(file) + L".hlsl");
            ComPtr<IDxcBlobEncoding> source;
            Check(utils->LoadFile(path.c_str(), nullptr, &source), "Read runtime shader source");
            const auto name = path.wstring();
            const wchar_t* arguments[]{name.c_str(), L"-E", entry, L"-T", profile, L"-HV", L"2021", L"-O3", L"-WX",
                L"-I", common.c_str(), L"-D", backend == 1 ? L"USE_DXR=1" : L"USE_DXR=0", L"-D", L"RENDER_MATERIAL_SCRIPTS=1",
                L"-D", backend == 2 ? L"RASTER_ONLY=1" : L"RASTER_ONLY=0"};
            DxcBuffer input{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
            ComPtr<IDxcResult> result;
            Check(compiler->Compile(&input, arguments, static_cast<UINT32>(std::size(arguments)), includes.Get(), IID_PPV_ARGS(&result)), "DXC compile invocation");
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            HRESULT status{}; Check(result->GetStatus(&status), "DXC compile status");
            if (FAILED(status)) throw std::runtime_error(errors && errors->GetStringLength() ? errors->GetStringPointer() : "DXC compilation failed");
            ComPtr<IDxcBlob> object;
            Check(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "DXC object output");
            const auto* begin = static_cast<const std::byte*>(object->GetBufferPointer());
            bundle.programs.emplace(key, std::vector<std::byte>(begin, begin + object->GetBufferSize()));
        };
        // 完整候选程序集合确保所有算法和后端排列使用同一份材质源码版本。
        compile(L"Deferred/GBuffer", L"GBuffer_VS", L"VSMain", L"vs_6_0", 0);
        compile(L"Deferred/GBuffer", L"GBuffer_PS", L"PSMain", L"ps_6_0", 0);
        compile(L"Shadows/RasterShadowDepth", L"ShadowDepth_VS", L"VSMain", L"vs_6_0", 0);
        compile(L"Shadows/RasterShadowDepth", L"ShadowDepth_PS", L"PSMain", L"ps_6_0", 0);
        for (unsigned backend : {0u, 1u, 2u}) {
            if (backend == 1 && !dxrSupported) continue;
            const auto suffix = L"_" + std::to_wstring(backend);
            for (const auto& [file, name] : {std::pair{L"Lighting/DirectLighting", L"DirectLighting"},
                 {L"PathTracing/PathTrace", L"PathTrace"}, {L"ScreenSpaceGI/SSGI", L"SSGI"}, {L"PostProcess/Post", L"Post"},
                 {L"ScreenSpaceEffects/SSAO", L"SSAO"}, {L"ScreenSpaceEffects/SSR", L"SSR"},
                 {L"ScreenSpaceEffects/SSIL", L"SSIL"}, {L"Sky/Sky", L"Sky"}}) {
                if (backend == 2 && std::wstring_view(name) == L"PathTrace") continue;
                compile(file, std::wstring(name) + suffix, L"CSMain", L"cs_6_5", backend);
            }
            if (backend != 2) {
                for (const auto* entry : {L"Candidate", L"Temporal", L"Spatial", L"Evaluate"})
                    compile(L"ReSTIRGI/ReSTIR", L"ReSTIR_" + std::wstring(entry) + suffix, (std::wstring(entry) + L"Main").c_str(), L"cs_6_5", backend);
            }
            for (const auto* entry : {L"Temporal", L"Spatial"})
                compile(L"Denoiser/RadianceResolve", L"RadianceResolve_" + std::wstring(entry) + suffix, (std::wstring(entry) + L"Main").c_str(), L"cs_6_5", backend);
            compile(L"Shadows/VSMGaussianBlur", L"ShadowBlurH" + suffix, L"HorizontalMain", L"cs_6_5", backend);
            compile(L"Shadows/VSMGaussianBlur", L"ShadowBlurV" + suffix, L"VerticalMain", L"cs_6_5", backend);
            // 材质脚本能修改次表面量，运行时重载必须同时替换两向扩散程序。
            compile(L"ScreenSpaceEffects/SSS", L"SSS_Horizontal" + suffix, L"HorizontalMain", L"cs_6_5", backend);
            compile(L"ScreenSpaceEffects/SSS", L"SSS_Vertical" + suffix, L"VerticalMain", L"cs_6_5", backend);
        }
        bundle.result = {true, {}};
    } catch (const std::exception& error) {
        bundle.programs.clear();
        bundle.result = {false, std::string("材质编译失败：") + error.what()};
    }
    return bundle;
}
}
