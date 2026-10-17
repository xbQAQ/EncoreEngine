cmake_minimum_required(VERSION 3.24)
# 项目与可执行目标使用仓库名称，确保生成的工程和发布入口保持一致。
project(EncoreEngine VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 模块目标遵循 Runtime 与 Editor 的职责边界。
add_library(engine_options INTERFACE)
if(MSVC)
  target_compile_options(engine_options INTERFACE /W4 /permissive- /utf-8)
endif()
add_library(engine_core INTERFACE)
target_include_directories(engine_core INTERFACE Source/Runtime/Core/Public)
target_link_libraries(engine_core INTERFACE engine_options)
add_library(render_core
  Source/Runtime/RenderCore/Private/Scene.cpp
  Source/Runtime/RenderCore/Private/WorldDocument.cpp
  Source/Runtime/RenderCore/Private/PrimitiveFactory.cpp
  Source/Runtime/RenderCore/Private/Bvh.cpp
  Source/Runtime/RenderCore/Private/RenderGraph.cpp)
target_include_directories(render_core PUBLIC Source/Runtime/RenderCore/Public)
target_link_libraries(render_core PUBLIC engine_core)

include(FetchContent)
# 使用官方 Protobuf 生成器与运行库，文本配置和二进制配置共享同一份协议。
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
FetchContent_Declare(protobuf
  URL https://codeload.github.com/protocolbuffers/protobuf/zip/refs/tags/v21.12
  URL_HASH SHA256=6a31b662deaeb0ac35e6287bda2f3369b19836e6c9f8828d4da444346f420298
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(protobuf)
set(WORLD_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated")
add_custom_command(OUTPUT "${WORLD_GENERATED_DIRECTORY}/World.pb.cc" "${WORLD_GENERATED_DIRECTORY}/World.pb.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${WORLD_GENERATED_DIRECTORY}"
  COMMAND $<TARGET_FILE:protobuf::protoc> "--cpp_out=${WORLD_GENERATED_DIRECTORY}"
    "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/Schemas" "${CMAKE_CURRENT_SOURCE_DIR}/Schemas/World.proto"
  DEPENDS protobuf::protoc "Schemas/World.proto" VERBATIM)
add_library(scene_document_proto STATIC "${WORLD_GENERATED_DIRECTORY}/World.pb.cc")
target_include_directories(scene_document_proto PUBLIC "${WORLD_GENERATED_DIRECTORY}")
target_link_libraries(scene_document_proto PUBLIC protobuf::libprotobuf)
target_link_libraries(render_core PUBLIC scene_document_proto)
target_compile_definitions(render_core PRIVATE RENDER_WORLD_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}/Content/Worlds")
FetchContent_Declare(entt URL https://github.com/skypjack/entt/archive/refs/tags/v3.15.0.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(entt)
# 反射位于独立模块，Core 的公共边界继续只依赖标准库。
add_library(engine_reflection Source/Runtime/Reflection/Private/Registry.cpp)
target_include_directories(engine_reflection PUBLIC Source/Runtime/Reflection/Public)
target_link_libraries(engine_reflection PUBLIC engine_core EnTT::EnTT)
add_executable(reflection_tests tests/ReflectionTests.cpp)
target_link_libraries(reflection_tests PRIVATE engine_reflection)
add_test(NAME reflection COMMAND reflection_tests)
# 固定 cgltf 解析器版本，解码和引擎数据转换归属 AssetImport。
FetchContent_Declare(cgltf URL https://codeload.github.com/jkuhlmann/cgltf/zip/refs/tags/v1.15 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cgltf)
add_library(gameplay Source/Runtime/Gameplay/Private/World.cpp
  Source/Runtime/Gameplay/Private/WorldDocumentState.cpp
  Source/Runtime/Gameplay/Private/WorldObjectPresets.cpp
  Source/Runtime/Gameplay/Private/WorldReflection.cpp)
target_include_directories(gameplay PUBLIC Source/Runtime/Gameplay/Public)
target_link_libraries(gameplay PUBLIC render_core EnTT::EnTT engine_reflection)

# 嵌入固定版本的官方 Lua 运行库，脚本调用边界由 Gameplay 管理。
FetchContent_Declare(lua URL https://www.lua.org/ftp/lua-5.4.8.tar.gz
  URL_HASH SHA256=4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_RUNTIME_SOURCES CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM LUA_RUNTIME_SOURCES "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c")
add_library(lua_runtime STATIC ${LUA_RUNTIME_SOURCES})
target_include_directories(lua_runtime PUBLIC "${lua_SOURCE_DIR}/src")
# Lua 适配只依赖通用反射，不读取 Gameplay 或 ECS 状态。
add_library(engine_lua_reflection Source/Runtime/Reflection/Private/Lua.cpp)
target_link_libraries(engine_lua_reflection PUBLIC engine_reflection PRIVATE lua_runtime)
add_library(gameplay_scripts Source/Runtime/Gameplay/Private/ScriptSystem.cpp
  Source/Runtime/Gameplay/Private/LuaWorld.cpp)
target_link_libraries(gameplay_scripts PUBLIC gameplay PRIVATE lua_runtime engine_lua_reflection)
add_executable(lua_script_tests tests/LuaScriptTests.cpp)
target_link_libraries(lua_script_tests PRIVATE gameplay_scripts)
add_test(NAME lua_scripts COMMAND lua_script_tests)
# 反射验收通过真实 Lua 回调检查元数据与世界组件访问。
add_executable(lua_reflection_tests tests/LuaReflectionTests.cpp)
target_link_libraries(lua_reflection_tests PRIVATE gameplay_scripts)
add_test(NAME lua_reflection COMMAND lua_reflection_tests)
add_executable(lua_binding_tests tests/LuaBindingTests.cpp)
target_link_libraries(lua_binding_tests PRIVATE gameplay_scripts)
add_test(NAME lua_binding COMMAND lua_binding_tests)
add_library(render_scene Source/Runtime/Renderer/Private/Scene/SceneExtraction.cpp
  Source/Runtime/Renderer/Private/Shadows/ShadowViews.cpp)
target_include_directories(render_scene PUBLIC Source/Runtime/Renderer/Public)
target_link_libraries(render_scene PUBLIC render_core gameplay)

# 数学、文档、渲染图和 ECS 测试无需图形设备。
enable_testing()
add_executable(world_document_tests Tests/WorldDocumentTests.cpp)
target_link_libraries(world_document_tests PRIVATE render_scene)
add_test(NAME world_document COMMAND world_document_tests)
add_executable(core_tests Tests/CoreTests.cpp)
target_link_libraries(core_tests PRIVATE render_core)
add_test(NAME core_algorithms COMMAND core_tests)
add_executable(gameplay_tests Tests/GameplayTests.cpp)
target_link_libraries(gameplay_tests PRIVATE gameplay)
add_test(NAME gameplay_ecs COMMAND gameplay_tests)
add_executable(scene_extraction_tests Tests/SceneExtractionTests.cpp)
target_link_libraries(scene_extraction_tests PRIVATE render_scene)
add_test(NAME scene_extraction COMMAND scene_extraction_tests)
add_executable(shadow_tests tests/ShadowTests.cpp)
target_include_directories(shadow_tests PRIVATE Source/Runtime/Renderer/Private)
target_link_libraries(shadow_tests PRIVATE render_scene)
add_test(NAME shadow_projection COMMAND shadow_tests)
add_executable(resource_tests tests/ResourceTests.cpp)
target_link_libraries(resource_tests PRIVATE render_scene)
add_test(NAME resource_model COMMAND resource_tests)

if(WIN32)
  target_compile_definitions(engine_options INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_library(asset_import Source/Runtime/AssetImport/Private/GltfImporter.cpp
    Source/Runtime/AssetImport/Private/GltfMaterials.cpp Source/Runtime/AssetImport/Private/GltfGeometry.cpp
    Source/Runtime/AssetImport/Private/GltfImages.cpp Source/Runtime/AssetImport/Private/Cgltf.cpp
    Source/Runtime/AssetImport/Private/ImportImages.cpp
    Source/Runtime/AssetImport/Private/SceneImporter.cpp
    Source/Runtime/AssetImport/Private/FbxImporter.cpp
    Source/Runtime/AssetImport/Private/FbxGeometry.cpp
    Source/Runtime/AssetImport/Private/FbxMaterials.cpp)
  # ufbx 负责真实 FBX 解析，资产模块只转换已验证的静态数据。
  FetchContent_Declare(ufbx
    URL https://codeload.github.com/ufbx/ufbx/zip/refs/tags/v0.23.1
    URL_HASH SHA256=3e5d8219f5b634fd374949808206eba1a1da49b2b2e3ee31ff0bd228c0abd469
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(ufbx)
  add_library(ufbx STATIC "${ufbx_SOURCE_DIR}/ufbx.c")
  target_include_directories(ufbx SYSTEM PUBLIC "${ufbx_SOURCE_DIR}")
  target_include_directories(asset_import PUBLIC Source/Runtime/AssetImport/Public PRIVATE ${cgltf_SOURCE_DIR})
  target_link_libraries(asset_import PUBLIC render_core PRIVATE ufbx windowscodecs ole32)
  add_executable(fbx_import_tests tests/FbxImportTests.cpp)
  target_link_libraries(fbx_import_tests PRIVATE asset_import gameplay)
  target_compile_definitions(fbx_import_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME fbx_import COMMAND fbx_import_tests)
  # 正式资产准备工具生成可移植世界与外部纹理载荷，不依赖编辑器进程。
  add_executable(citlali_scene_tool tools/PrepareCitlaliScene.cpp)
  target_link_libraries(citlali_scene_tool PRIVATE asset_import gameplay)
  add_executable(asset_import_tests tests/AssetImportTests.cpp)
  target_link_libraries(asset_import_tests PRIVATE asset_import)
  target_link_libraries(resource_tests PRIVATE asset_import)
  add_test(NAME asset_import COMMAND asset_import_tests)
  # 相同版本的官方 docking 标签提供可拖动分隔与停靠，避免依赖浮动分支。
  # 使用解压时刻触发旧工作目录中的头文件依赖重编译，避免复用非 docking 的二进制布局。
  FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.zip
    URL_HASH SHA256=ca3471ed0c472b210fc37242e0b6338f37574f77f8bc1303befc040f0c22f06b
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)

  # 固定变换控件版本，并隔离上游示例与测试工程。
  FetchContent_Declare(imguizmo URL https://codeload.github.com/CedricGuillemet/ImGuizmo/zip/105e4fde45e4e74aae8973d98750820ef7aca7e2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR unused)
  FetchContent_MakeAvailable(imguizmo)
  add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
  target_include_directories(imguizmo PUBLIC ${imguizmo_SOURCE_DIR}/src)
  target_link_libraries(imguizmo PUBLIC imgui)
  add_library(editor_gizmo Source/Editor/EngineEditor/Private/EditorGizmo.cpp)
  target_include_directories(editor_gizmo PUBLIC Source/Editor/EngineEditor/Public)
  target_link_libraries(editor_gizmo PUBLIC gameplay render_scene)
  add_executable(editor_gizmo_tests Tests/EditorGizmoTests.cpp)
  target_link_libraries(editor_gizmo_tests PRIVATE editor_gizmo)
  add_test(NAME editor_gizmo COMMAND editor_gizmo_tests)

  # 作者事务、属性默认值和导航数学可以脱离 ImGui 与 GPU 独立验证。
  add_library(editor_history Source/Editor/EngineEditor/Private/EditorHistory.cpp)
  target_include_directories(editor_history PUBLIC Source/Editor/EngineEditor/Public)
  target_link_libraries(editor_history PUBLIC gameplay render_scene)
  # 常用物体的 CPU 回归同时检查资源共享、保存重开及编辑器撤销后的身份恢复。
  add_executable(object_preset_tests Tests/ObjectPresetTests.cpp)
  target_link_libraries(object_preset_tests PRIVATE render_scene editor_history asset_import)
  target_compile_definitions(object_preset_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME object_presets COMMAND object_preset_tests)
  add_executable(editor_history_tests tests/EditorHistoryTests.cpp)
  target_link_libraries(editor_history_tests PRIVATE editor_history)
  add_test(NAME editor_history COMMAND editor_history_tests)
  add_library(editor_defaults Source/Editor/EngineEditor/Private/EditorDefaults.cpp)
  target_include_directories(editor_defaults PUBLIC Source/Editor/EngineEditor/Public)
  target_link_libraries(editor_defaults PUBLIC editor_history)
  add_executable(editor_defaults_tests tests/EditorDefaultsTests.cpp)
  target_link_libraries(editor_defaults_tests PRIVATE editor_defaults)
  add_test(NAME editor_defaults COMMAND editor_defaults_tests)
  add_library(editor_navigation Source/Editor/EngineEditor/Private/EditorNavigation.cpp)
  target_include_directories(editor_navigation PUBLIC Source/Editor/EngineEditor/Public)
  target_link_libraries(editor_navigation PUBLIC editor_gizmo)
  add_executable(editor_navigation_tests Tests/EditorNavigationTests.cpp)
  target_link_libraries(editor_navigation_tests PRIVATE editor_navigation)
  add_test(NAME editor_navigation COMMAND editor_navigation_tests)

  file(GLOB SDK_BIN_DIRS "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
  list(SORT SDK_BIN_DIRS COMPARE NATURAL ORDER DESCENDING)
  foreach(SDK_BIN IN LISTS SDK_BIN_DIRS)
    list(APPEND DXC_HINTS "${SDK_BIN}/x64")
  endforeach()
  find_program(DXC_EXECUTABLE NAMES dxc HINTS ${DXC_HINTS} REQUIRED)
  set(SHADER_OUTPUTS "")
  # 共享头文件作为显式依赖，避免算法变体保留过期字节码。
  file(GLOB_RECURSE SHADER_INCLUDES CONFIGURE_DEPENDS "Shaders/Private/*.hlsli")
  function(compile_shader SOURCE NAME ENTRY PROFILE SUFFIX DXR)
    # 纯光栅变体在编译时移除软件 BVH 与硬件求交路径。
    set(RASTER 0)
    if(SUFFIX MATCHES "(^|_)2$")
      set(RASTER 1)
    endif()
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${NAME}_${SUFFIX}.cso")
    add_custom_command(OUTPUT "${OUTPUT}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
      COMMAND "${DXC_EXECUTABLE}" -E ${ENTRY} -T ${PROFILE} -D USE_DXR=${DXR} -D RASTER_ONLY=${RASTER} -HV 2021 -WX -O3
        -I "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Private/Common" -Fo "${OUTPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Private/${SOURCE}.hlsl"
      DEPENDS "Shaders/Private/${SOURCE}.hlsl" ${SHADER_INCLUDES} VERBATIM)
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${OUTPUT}" PARENT_SCOPE)
  endfunction()
  compile_shader(Deferred/GBuffer GBuffer VSMain vs_6_0 VS 0)
  compile_shader(Deferred/GBuffer GBuffer PSMain ps_6_0 PS 0)
  compile_shader(Shadows/RasterShadowDepth ShadowDepth VSMain vs_6_0 VS 0)
  compile_shader(Shadows/RasterShadowDepth ShadowDepth PSMain ps_6_0 PS 0)
  compile_shader(Materials/DisneyValidation DisneyValidation ValidationMain cs_6_5 test 0)
  # LTC 数值验证固定使用纯光栅定义，不引入软件或硬件求交依赖。
  compile_shader(Lighting/LtcValidation LtcValidation LtcValidationMain cs_6_5 test_2 0)
  foreach(BACKEND RANGE 0 2)
    set(HARDWARE_RAYS 0)
    if(BACKEND EQUAL 1)
      set(HARDWARE_RAYS 1)
    endif()
    compile_shader(Lighting/DirectLighting DirectLighting CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(ScreenSpaceGI/SSGI SSGI CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(PostProcess/Post Post CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    foreach(EFFECT SSAO SSR SSIL)
      compile_shader(ScreenSpaceEffects/${EFFECT} ${EFFECT} CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    endforeach()
    compile_shader(ScreenSpaceEffects/SSS SSS_Horizontal HorizontalMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(ScreenSpaceEffects/SSS SSS_Vertical VerticalMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(Sky/Sky Sky CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(Shadows/VSMGaussianBlur ShadowBlurH HorizontalMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    compile_shader(Shadows/VSMGaussianBlur ShadowBlurV VerticalMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
    if(BACKEND LESS 2)
      compile_shader(PathTracing/PathTrace PathTrace CSMain cs_6_5 ${BACKEND} ${HARDWARE_RAYS})
      foreach(ENTRY Candidate Temporal Spatial Evaluate)
        compile_shader(ReSTIRGI/ReSTIR ReSTIR ${ENTRY}Main cs_6_5 ${ENTRY}_${BACKEND} ${HARDWARE_RAYS})
      endforeach()
    endif()
    foreach(ENTRY Temporal Spatial)
      compile_shader(Denoiser/RadianceResolve RadianceResolve ${ENTRY}Main cs_6_5 ${ENTRY}_${BACKEND} ${HARDWARE_RAYS})
    endforeach()
  endforeach()
  add_custom_target(render_shaders ALL DEPENDS ${SHADER_OUTPUTS})

  add_library(directx12_rhi
    Source/Runtime/DirectX12RHI/Private/Dx12Device.cpp
    Source/Runtime/DirectX12RHI/Private/DescriptorArena.cpp
    Source/Runtime/DirectX12RHI/Private/Dx12Swapchain.cpp
    Source/Runtime/DirectX12RHI/Private/ResourcePool.cpp
    Source/Runtime/DirectX12RHI/Private/GraphProfiler.cpp
    Source/Runtime/DirectX12RHI/Private/GraphExecutor.cpp)
  target_include_directories(directx12_rhi PUBLIC Source/Runtime/DirectX12RHI/Public)
  target_link_libraries(directx12_rhi PUBLIC render_core d3d12 dxgi dxguid)
  add_library(renderer
    # 私有实现按职责定位；只暴露 Private 根目录，跨职责引用必须写明所属目录。
    Source/Runtime/Renderer/Private/Frame/FrameRenderer.cpp
    Source/Runtime/Renderer/Private/Frame/FrameHistory.cpp
    Source/Runtime/Renderer/Private/View/ViewState.cpp
    Source/Runtime/Renderer/Private/View/ViewHistory.cpp
    Source/Runtime/Renderer/Private/Resources/GraphResources.cpp
    Source/Runtime/Renderer/Private/Resources/RenderTarget.cpp
    Source/Runtime/Renderer/Private/Resources/FrameResources.cpp
    Source/Runtime/Renderer/Private/Pipeline/PipelineCache.cpp
    Source/Runtime/Renderer/Private/Pipeline/PassContext.cpp
    Source/Runtime/Renderer/Private/Pipeline/RenderPipeline.cpp
    Source/Runtime/Renderer/Private/Materials/MaterialShaderCompiler.cpp
    Source/Runtime/Renderer/Private/Scene/SceneGpu.cpp
    Source/Runtime/Renderer/Private/Shadows/ShadowRenderer.cpp
    Source/Runtime/Renderer/Private/Deferred/GBufferPass.cpp
    Source/Runtime/Renderer/Private/Deferred/DeferredRenderer.cpp
    Source/Runtime/Renderer/Private/Lighting/DirectLightingPass.cpp
    Source/Runtime/Renderer/Private/Lighting/LtcLuts.cpp
    Source/Runtime/Renderer/Private/PathTracing/PathTracingPass.cpp
    Source/Runtime/Renderer/Private/ScreenSpaceGI/SSGIPass.cpp
    Source/Runtime/Renderer/Private/ScreenSpaceEffects/ScreenSpaceEffectsPass.cpp
    Source/Runtime/Renderer/Private/ReSTIRGI/ReSTIRGIPass.cpp
    Source/Runtime/Renderer/Private/Denoiser/RadianceResolvePass.cpp
    Source/Runtime/Renderer/Private/PostProcess/PostProcessPass.cpp
    Source/Runtime/Renderer/Private/Capture/CaptureService.cpp)
  target_include_directories(renderer PUBLIC Source/Runtime/Renderer/Public PRIVATE Source/Runtime/Renderer/Private)
  target_compile_definitions(renderer PRIVATE RENDER_SHADER_DIRECTORY=L"${CMAKE_CURRENT_BINARY_DIR}/shaders")
  # 将固定版本的拟合表嵌入模块，发布程序不依赖源码目录中的运行时文件。
  foreach(LTC_TABLE MATRIX AMPLITUDE)
    if(LTC_TABLE STREQUAL "MATRIX")
      set(LTC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/LTC/ltc_1.dds")
    else()
      set(LTC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/LTC/ltc_2.dds")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LTC_FILE}")
    file(SIZE "${LTC_FILE}" LTC_${LTC_TABLE}_SIZE)
    file(READ "${LTC_FILE}" LTC_HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," LTC_${LTC_TABLE}_BYTES "${LTC_HEX}")
  endforeach()
  configure_file(tools/cmake/LtcTables.generated.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/LtcTables.generated.h" @ONLY)
  target_include_directories(renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_definitions(renderer PRIVATE
    RENDER_SHADER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}/shaders/Private"
    RENDER_DXC_EXECUTABLE=L"${DXC_EXECUTABLE}")
  target_link_libraries(renderer PUBLIC directx12_rhi render_scene PRIVATE windowscodecs ole32)
  add_dependencies(renderer render_shaders)
  # 公共扩展契约和纯 CPU 视图状态独立验证，不需要创建 GPU 设备。
  add_executable(render_pipeline_tests tests/RenderPipelineTests.cpp)
  target_link_libraries(render_pipeline_tests PRIVATE renderer)
  add_test(NAME render_pipeline COMMAND render_pipeline_tests)
  add_executable(view_state_tests tests/ViewStateTests.cpp)
  target_include_directories(view_state_tests PRIVATE Source/Runtime/Renderer/Private)
  target_link_libraries(view_state_tests PRIVATE renderer)
  add_test(NAME view_state COMMAND view_state_tests)
  add_library(engine_editor
    Source/Editor/EngineEditor/Private/EditorUi.cpp
    Source/Editor/EngineEditor/Private/EditorLocalization.cpp
    Source/Editor/EngineEditor/Private/EditorScriptUi.cpp
    Source/Editor/EngineEditor/Private/EditorContentBrowser.cpp
    Source/Editor/EngineEditor/Private/EditorContentFiles.cpp
    Source/Editor/EngineEditor/Private/EditorObjectPalette.cpp
    Source/Editor/EngineEditor/Private/EditorWorldUi.cpp
    Source/Editor/EngineEditor/Private/EditorProfilerUi.cpp
    Source/Editor/EngineEditor/Private/EditorGizmoUi.cpp
    Source/Editor/EngineEditor/Private/EditorHistoryUi.cpp
    Source/Editor/EngineEditor/Private/EditorViewportInput.cpp
    Source/Editor/EngineEditor/Private/EditorOverlayPass.cpp
    Source/Editor/EngineEditor/Private/Dx12Renderer.cpp)
  target_include_directories(engine_editor PUBLIC Source/Editor/EngineEditor/Public PRIVATE Source/Runtime/Renderer/Private)
  target_compile_definitions(engine_editor PRIVATE EDITOR_CONTENT_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}/Content")
  target_link_libraries(engine_editor PUBLIC renderer gameplay_scripts PRIVATE imgui imguizmo editor_gizmo editor_history editor_defaults editor_navigation asset_import comdlg32)
  # 浏览器文件操作独立于 ImGui，CPU 测试与正式编辑器复用同一创建实现。
  add_executable(editor_content_browser_tests Tests/EditorContentBrowserTests.cpp
    Source/Editor/EngineEditor/Private/EditorContentBrowser.cpp
    Source/Editor/EngineEditor/Private/EditorContentFiles.cpp)
  target_include_directories(editor_content_browser_tests PRIVATE
    Source/Editor/EngineEditor/Public Source/Runtime/Renderer/Public)
  target_compile_definitions(editor_content_browser_tests PRIVATE EDITOR_CONTENT_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}/Content")
  target_link_libraries(editor_content_browser_tests PRIVATE gameplay_scripts imgui comdlg32)
  add_test(NAME editor_content_browser COMMAND editor_content_browser_tests)
  # 时间线使用正式 ImGui 输入验证显式录制、逐帧回看、预算上限及真实区间，无需创建设备。
  add_executable(editor_profiler_tests Tests/EditorProfilerTests.cpp
    Source/Editor/EngineEditor/Private/EditorProfilerUi.cpp
    Source/Editor/EngineEditor/Private/EditorLocalization.cpp)
  target_include_directories(editor_profiler_tests PRIVATE Source/Editor/EngineEditor/Public)
  target_link_libraries(editor_profiler_tests PRIVATE render_core imgui)
  add_test(NAME editor_profiler COMMAND editor_profiler_tests)
  add_executable(EncoreEngine Source/Runtime/Launch/Private/Main.cpp)
  target_link_libraries(EncoreEngine PRIVATE engine_editor imgui)
  # 将正式内容复制到可执行文件旁，发行目录不依赖源码树。
  add_custom_command(TARGET EncoreEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Content" "$<TARGET_FILE_DIR:EncoreEngine>/Content"
    # 单独构建 EncoreEngine 目标时也同步入口，入口始终对应本次构建配置。
    COMMAND ${CMAKE_COMMAND} "-DENTRY_SOURCE=$<TARGET_FILE:EncoreEngine>"
      "-DENTRY_DESTINATION=${CMAKE_CURRENT_SOURCE_DIR}/EncoreEngine.exe"
      "-DENTRY_STAGING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/build/RootEntry"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PublishRootEntry.cmake"
    VERBATIM)
  # 无需重新链接的默认增量构建也检查入口，删除入口后再次构建即可自动补齐。
  add_custom_target(encore_root_entry ALL
    COMMAND ${CMAKE_COMMAND} "-DENTRY_SOURCE=$<TARGET_FILE:EncoreEngine>"
      "-DENTRY_DESTINATION=${CMAKE_CURRENT_SOURCE_DIR}/EncoreEngine.exe"
      "-DENTRY_STAGING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/build/RootEntry"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PublishRootEntry.cmake"
    DEPENDS EncoreEngine
    VERBATIM)
  # 在创建设备前拒绝超过着色器采样上限的输入。
  add_test(NAME cli_reject_excess_spp COMMAND EncoreEngine --spp 17 --frames 1 --width 64 --height 64 --headless)
  set_tests_properties(cli_reject_excess_spp PROPERTIES WILL_FAIL TRUE)
  # GPU 验证采用显式开关，保留无图形设备环境的 CPU 验证入口。
  option(ENCORE_ENABLE_GPU_TESTS "Build and register DirectX 12 integration tests" OFF)
  if(ENCORE_ENABLE_GPU_TESTS)
    # 真实 ImGui 拖动覆盖表面放置、取消、默认值、撤销和世界文档往返。
    add_executable(editor_object_placement_tests Tests/EditorObjectPlacementTests.cpp)
    target_link_libraries(editor_object_placement_tests PRIVATE engine_editor imgui imguizmo)
    target_compile_definitions(editor_object_placement_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME editor_object_placement COMMAND editor_object_placement_tests)
    set_tests_properties(editor_object_placement PROPERTIES LABELS GPU TIMEOUT 120)
    # 独立的浮点读回对照检查 LTC 积分，不能以降噪后的显示图像替代数值验证。
    add_executable(ltc_lighting_tests tests/LtcLightingTests.cpp)
    target_include_directories(ltc_lighting_tests PRIVATE Source/Runtime/Renderer/Private)
    target_compile_definitions(ltc_lighting_tests PRIVATE RENDER_SHADER_DIRECTORY=L"${CMAKE_CURRENT_BINARY_DIR}/shaders")
    target_link_libraries(ltc_lighting_tests PRIVATE renderer)
    add_test(NAME ltc_lighting COMMAND ltc_lighting_tests)
    set_tests_properties(ltc_lighting PROPERTIES LABELS GPU TIMEOUT 180)
    # 独立扩展模块只链接公共 Renderer 契约，私有帧入口仅对集成宿主可见。
    add_library(example_render_extension STATIC tests/modules/ExampleRenderExtension.cpp)
    target_link_libraries(example_render_extension PUBLIC renderer)
    target_include_directories(example_render_extension PUBLIC tests)
    add_executable(render_extension_integration_tests tests/RenderExtensionIntegrationTests.cpp)
    target_include_directories(render_extension_integration_tests PRIVATE Source/Runtime/Renderer/Private)
    target_link_libraries(render_extension_integration_tests PRIVATE example_render_extension)
    add_test(NAME render_extension_integration COMMAND render_extension_integration_tests)
    set_tests_properties(render_extension_integration PROPERTIES LABELS GPU TIMEOUT 120)
    # 时间戳和材质测试验证真实 GPU 行为。
    add_executable(graph_profiler_tests Tests/GraphProfilerTests.cpp)
    target_link_libraries(graph_profiler_tests PRIVATE directx12_rhi)
    add_test(NAME graph_profiler COMMAND graph_profiler_tests)
    set_tests_properties(graph_profiler PROPERTIES LABELS GPU TIMEOUT 120)
    add_executable(material_script_tests tests/MaterialScriptTests.cpp)
    target_include_directories(material_script_tests PRIVATE Source/Runtime/Renderer/Private)
    target_link_libraries(material_script_tests PRIVATE renderer)
    add_test(NAME material_scripts COMMAND material_script_tests)
    set_tests_properties(material_scripts PROPERTIES LABELS GPU TIMEOUT 180)
    add_executable(raster_pipeline_tests Tests/RasterPipelineTests.cpp)
    target_include_directories(raster_pipeline_tests PRIVATE Source/Runtime/Renderer/Private)
    target_compile_definitions(raster_pipeline_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(raster_pipeline_tests PRIVATE renderer asset_import)
    add_test(NAME raster_pipeline COMMAND raster_pipeline_tests)
    set_tests_properties(raster_pipeline PROPERTIES LABELS GPU TIMEOUT 180)
    add_executable(screen_space_effects_tests tests/ScreenSpaceEffectsTests.cpp)
    target_include_directories(screen_space_effects_tests PRIVATE Source/Runtime/Renderer/Private)
    target_link_libraries(screen_space_effects_tests PRIVATE renderer)
    add_test(NAME screen_space_effects COMMAND screen_space_effects_tests)
    set_tests_properties(screen_space_effects PROPERTIES LABELS GPU TIMEOUT 180)
    # 使用真实 ImGui 输入检查选择和控件边界，无需移动系统鼠标。
    add_executable(editor_gizmo_interaction_tests Tests/EditorGizmoInteractionTests.cpp)
    target_link_libraries(editor_gizmo_interaction_tests PRIVATE engine_editor imgui imguizmo)
    add_test(NAME editor_gizmo_interaction COMMAND editor_gizmo_interaction_tests)
    set_tests_properties(editor_gizmo_interaction PROPERTIES LABELS GPU TIMEOUT 120)
    # 正式宿主开启原生多窗口，验证拖出、缩放、独立队列呈现及销毁，布局仅写临时目录。
    add_executable(editor_platform_windows_tests Tests/EditorPlatformWindowsTests.cpp)
    target_link_libraries(editor_platform_windows_tests PRIVATE engine_editor imgui dwmapi)
    add_test(NAME editor_platform_windows COMMAND editor_platform_windows_tests)
    set_tests_properties(editor_platform_windows PROPERTIES LABELS GPU TIMEOUT 120)
    add_executable(disney_validation_tests Tests/DisneyValidationTests.cpp)
    target_compile_definitions(disney_validation_tests PRIVATE RENDER_SHADER_DIRECTORY=L"${CMAKE_CURRENT_BINARY_DIR}/shaders")
    target_link_libraries(disney_validation_tests PRIVATE engine_options d3d12 dxgi dxguid)
    add_dependencies(disney_validation_tests render_shaders)
    add_test(NAME disney_bsdf COMMAND disney_validation_tests)
    set_tests_properties(disney_bsdf PROPERTIES LABELS GPU TIMEOUT 120)
    add_executable(editor_import_integration_tests tests/EditorImportIntegrationTests.cpp)
    target_compile_definitions(editor_import_integration_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(editor_import_integration_tests PRIVATE engine_editor)
    add_test(NAME editor_import_integration COMMAND editor_import_integration_tests)
    set_tests_properties(editor_import_integration PROPERTIES LABELS GPU TIMEOUT 120)
    add_executable(renderer_integration_tests Tests/RendererIntegrationTests.cpp)
    target_include_directories(renderer_integration_tests PRIVATE Source/Runtime/Renderer/Private)
    target_link_libraries(renderer_integration_tests PRIVATE renderer asset_import editor_history)
    target_compile_definitions(renderer_integration_tests PRIVATE RENDER_SOURCE_DIRECTORY=L"${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME renderer_integration COMMAND renderer_integration_tests)
    add_test(NAME rdg_resources COMMAND renderer_integration_tests --rdg-only)
    set_tests_properties(rdg_resources PROPERTIES LABELS GPU TIMEOUT 120)
    set_tests_properties(renderer_integration PROPERTIES LABELS GPU TIMEOUT 240)
  endif()
endif()
