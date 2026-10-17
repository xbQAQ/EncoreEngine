# EncoreEngine 架构说明

本文记录当前工程采用的模块布局和 GPU 帧边界。目标是让 CPU 世界状态、DirectX 12 资源和算法 Pass 保持清晰依赖，便于在不改变上层 ECS 和渲染设置的情况下替换求交、GI 或后处理实现。模块按 `Public`/`Private` 分开；公共头文件只暴露调用者需要的类型和契约，D3D12 细节留在 RHI/Renderer 私有实现中。

## 1. 目录布局

```text
Source/
  Runtime/
    Core/          平台无关基础类型和数学
    Reflection/    独立元数据注册表、动态调用和 Lua 值桥接
    RenderCore/    Scene、BVH、Reservoir、RenderGraph、GpuTypes
    Gameplay/     EnTT World、Components、RenderSnapshot
    AssetImport/  cgltf/ufbx 解析、WIC 图像、静态 FBX/glTF/GLB 转换
    DirectX12RHI/ Dx12Device、DescriptorArena、资源池、GraphExecutor、Swapchain
    Renderer/      SceneExtraction、SceneGpu、FrameRenderer、GI/Post Pass 组合
    Launch/        Win32 入口和命令行
  Editor/
    EngineEditor/
      Public/    Dx12Renderer、EditorUi、EditorGizmo、EditorNavigation、EditorHistory
      Private/   Editor UI 适配层、导航/历史实现、EditorOverlayPass、宿主 Impl
shaders/Private/
  Common/Common.hlsli
  Materials/Disney.hlsli
  Materials/MaterialSampling.hlsli
  Shadows/RasterShadowDepth.hlsl
  Shadows/VSMGaussianBlur.hlsl
  Deferred/GBuffer.hlsl
  Lighting/DirectLighting.hlsl
  PathTracing/PathTrace.hlsl
  ScreenSpaceGI/SSGI.hlsl
  ReSTIRGI/ReSTIR.hlsl
  Denoiser/RadianceResolve.hlsl
  PostProcess/Post.hlsl
docs/
  Architecture.md
  ReSTIR-GI-Learning.md
  Validation.md
  BlenderImport.md
  EditorControls.md
  EditorGizmo.md
  Resources.md
  ContactVisibility.md
```

每个 C++ 模块分别设置 `Public` include root 和 `Private` 实现目录；暂时没有公开接口的 Launch 也保留这一目录约定。

### Renderer 私有目录

目录按所有权和功能职责组织，同一类型的声明和实现放在一起。跨目录包含从 `Private` 根目录写出所属目录，例如 `Resources/FrameResources.h`；不为每个子目录新增包含路径，也不在根目录保留转发头。

| 目录 | 文件与职责 |
| --- | --- |
| `Frame/` | `FrameRenderer` 负责帧调度和提交，`FrameHistory` 在帧边界注册历史提取 |
| `View/` | `ViewState` 保存 CPU 相机与采样状态，`ViewHistory` 管理跨帧 GPU 存储 |
| `Resources/` | `GraphResources`、`RenderTarget` 和 `FrameResources` 声明图内资源及算法资源组 |
| `Pipeline/` | `RenderPipeline` 提供阶段扩展，`RenderPasses` 声明内置入口，`PassContext` 借用帧资源，`PipelineCache` 管理 PSO |
| `Materials/` | `MaterialShaderCompiler` 编译材质源码和着色器变体，向管线缓存提供结果 |
| `Scene/` | 场景提取、GPU 上传及加速结构 |
| `Capture/` | 独立读回流程，不参与算法或帧资源所有权 |
| 各算法目录 | `Deferred`、`Lighting`、`Shadows`、`PathTracing`、`ScreenSpaceGI`、`ScreenSpaceEffects`、`ReSTIRGI`、`Denoiser`、`PostProcess` |

公共头继续位于 `Public/render/`，按稳定契约暴露，不为私有实现的每个子目录建立公共镜像。`RenderCore` 的通用 RDG 与 Renderer 的具体帧资源仍分别维护。

### 参考与取舍

参考成熟引擎的职责边界，而非要求所有工程采用同一目录树：

| 参考项目 | 实际组织 | 本项目采用的原则 |
| --- | --- | --- |
| [Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-directory-structure) | Source 按 Runtime、Editor、Developer、Programs 分开，模块区分 Public/Private | 保持运行时与编辑器依赖方向，公共头只承载外部契约 |
| [Godot](https://docs.godotengine.org/en/stable/engine_details/architecture/godot_architecture_diagram.html) | 场景层 `scene/`、服务层 `servers/`、底层 `drivers/` 与 `platform/` | 世界状态、渲染消费和设备执行各自拥有生命周期 |
| [Filament](https://github.com/google/filament/blob/main/filament/CMakeLists.txt) | 公共 `include/filament/`，私有 `src/details/`、`src/fg/`、`src/components/`，部分核心文件直接位于 src | 公共 API 与实现分离，图基础设施集中维护，功能目录保持内聚 |

这些项目并不要求每个实现文件都建立子目录；此处的五组目录是根据当前 Renderer 已有类型和职责做出的本地划分。当前只维护 DirectX 12 后端，沿用 `DirectX12RHI` 的设备、资源池、图执行器和交换链；本轮不引入跨平台 RHI 接口或其他后端。目录整理不改变渲染阶段、资源寿命或现有算法行为。

Renderer 的算法文件分别注册自己的 Pass：

| 算法层 | 私有实现 | 主要职责 |
| --- | --- | --- |
| Deferred | [`GBufferPass.cpp`](../Source/Runtime/Renderer/Private/Deferred/GBufferPass.cpp) | 光栅 position/normal/base/depth |
| Shadows | [`ShadowRenderer.cpp`](../Source/Runtime/Renderer/Private/Shadows/ShadowRenderer.cpp) | atlas 深度/矩生成与 VSM 分离模糊 |
| Lighting | [`DirectLightingPass.cpp`](../Source/Runtime/Renderer/Private/Lighting/DirectLightingPass.cpp) | primary emission 与直接光 |
| PathTracing | [`PathTracingPass.cpp`](../Source/Runtime/Renderer/Private/PathTracing/PathTracingPass.cpp) | 完整相机路径和渐进积分 |
| ScreenSpaceGI | [`SSGIPass.cpp`](../Source/Runtime/Renderer/Private/ScreenSpaceGI/SSGIPass.cpp) | 屏幕空间射线和 GI 历史 |
| ReSTIRGI | [`ReSTIRGIPass.cpp`](../Source/Runtime/Renderer/Private/ReSTIRGI/ReSTIRGIPass.cpp) | candidates、temporal、spatial、evaluate |
| Denoiser | [`RadianceResolvePass.cpp`](../Source/Runtime/Renderer/Private/Denoiser/RadianceResolvePass.cpp) | HDR temporal/spatial reconstruction |
| PostProcess | [`PostProcessPass.cpp`](../Source/Runtime/Renderer/Private/PostProcess/PostProcessPass.cpp) | 介质、曝光、Bloom、ACES 和显示编码 |

[`Frame/FrameRenderer.cpp`](../Source/Runtime/Renderer/Private/Frame/FrameRenderer.cpp) 通过 `DeferredRender` 组合固定阶段并执行图；`Frame/FrameHistory.cpp` 注册历史资源的所有权提取，`View/ViewState.cpp` 维护纯 CPU 视图历史。各 Pass 在自己的实现文件中设置算法参数；`Pipeline/PipelineCache.cpp` 管理 PSO，`Pipeline/PassContext.cpp` 提供 RDG 上传和录制接口，`Scene/SceneGpu.cpp` 负责场景资源与加速结构，`Capture/CaptureService.cpp` 提供独立读回流程。

## 2. 依赖方向

下表的“依赖”表示左侧模块使用右侧模块的公开接口；算法与世界状态不反向依赖 Editor：

| 模块 | 直接依赖与边界 |
| --- | --- |
| Core | C++ 标准库；提供数学与基础类型 |
| Reflection | Core、EnTT meta；Lua 桥接目标额外依赖 Lua，不依赖 Gameplay 或 GPU |
| RenderCore | Core；提供平台无关场景、求交结构和 RenderGraph |
| Gameplay | Core、Reflection、RenderCore 数据及 EnTT；脚本目标组合 Lua，不依赖 DirectX12RHI |
| AssetImport | RenderCore、固定版 cgltf v1.15/ufbx v0.23.1、Windows WIC/COM；不依赖 GPU、ECS 或 Editor |
| DirectX12RHI | RenderCore、D3D12/DXGI；不依赖 Gameplay registry |
| Renderer | RenderCore、Gameplay、DirectX12RHI；消费快照并组合渲染 Pass |
| EngineEditor | Gameplay、Renderer、AssetImport、DirectX12RHI 设备/描述符及 ImGui；包含宿主 `Dx12Renderer::Impl` |
| Launch | 组合运行时与编辑器；不实现 GI 算法 |

`Core` 不包含 D3D12、ImGui 或 EnTT。`Gameplay` 拥有 ECS、世界文档和 CPU Resource，提取无 GPU 的 `RenderSnapshot`，不创建 GPU 资源或提交命令。`DirectX12RHI` 不读取 EnTT registry，也不决定 GI 算法；它实现设备能力、命令队列、描述符、资源生命周期与 RDG 执行。`Renderer` 消费不可变快照，负责场景上传、BVH/BLAS 重建、GBuffer、GI、resolve 和 post pass，完全不依赖 Editor。`EngineEditor` 修改 `World.Registry()` 中的组件后调用 `World.MarkDirty()` 并将 `RenderSettings.reset` 置位；其 `Dx12Renderer::Impl` 负责把 World、Renderer、RHI、Swapchain、EditorUi 和 CaptureService 组合起来，ImGui GPU 绘制仅发生在最终 Editor Pass 的 callback 中。`Launch` 只负责窗口、命令行配置和 `Dx12Renderer` 对象组装。

构建目标保持以下方向：`renderer` 链接 `directx12_rhi` 和 `render_scene`，后者依赖 Gameplay/RenderCore；`engine_editor` 链接 `renderer`、`asset_import` 与 ImGui，并经运行时目标获得 Gameplay 和 DirectX12RHI 接口。Renderer 不链接 Editor 或 AssetImport，也不包含它们的头文件；它接收已经拥有全部数据的 `Scene`。`EncoreEngine` 链接 `engine_editor` 与 ImGui，公共 `render/Dx12Renderer.h` 属于 EngineEditor。Core、RenderCore、Gameplay 和导入转换均可在无窗口/无 GPU 的测试中验证，WIC 导入测试仍要求 Windows。

反射注册表使用独立 EnTT meta 上下文。`engine_reflection` 提供类型、字段和函数元数据，`engine_lua_reflection` 提供通用 Lua 值桥接；Gameplay 的 `WorldReflection` 注册引擎类型并适配组件快照事务。`ScriptSystem` 在首次编译时冻结元数据，每个实例独占 Lua 虚拟机，通过 `world` API 查询和提交世界状态。扩展方式见 [`Reflection.md`](Reflection.md)。

## 3. Thin PImpl 和对象所有权

[`Dx12Renderer.h`](../Source/Editor/EngineEditor/Public/render/Dx12Renderer.h) 的宿主入口使用 Thin PImpl：公共头保留 Win32 窗口入口、启动参数和 `Frame/Capture/CaptureEditor/Resize`，私有 `Impl` 组合设备、资源池、场景、Pass、历史和 Editor。PImpl 的析构在 [`Private/Dx12Renderer.cpp`](../Source/Editor/EngineEditor/Private/Dx12Renderer.cpp) 中执行。这个边界隐藏 D3D12 对象和渲染细节；Gameplay、RenderCore 和 Renderer 不需要包含该宿主头。

PImpl 应只负责构造、委托和生命周期，具体 GPU 工作由有独立职责的类承担：

| 对象 | 所有者 | 责任 |
| --- | --- | --- |
| `gameplay::World` | `Dx12Renderer::Impl` | ECS 实体、组件、revision、模拟和快照提取 |
| `dx12::Dx12Device` | `Dx12Renderer::Impl` | Adapter、queue、command list、fence、DXR 能力 |
| `dx12::DescriptorArena` | `Dx12Renderer::Impl` | CPU/visible CBV-SRV-UAV heaps、RTV/DSV 和帧表 |
| `ResourcePool`/`Dx12Swapchain` | `Dx12Renderer::Impl` | GPU 资源分配、上传暂存与 presentation surface |
| `RenderGraph`/`GraphExecutor` | 每帧/初始化局部作用域 | Pass 依赖、资源状态、barrier、拓扑和执行 |
| `SceneGpu` | `FrameRenderer` | ECS 场景提取结果、几何/材质资源及加速结构 |
| `ShadowRenderer` | `FrameRenderer` | 原始深度矩 atlas、深度目标、横纵模糊目标和 shadow Pass |
| `ViewState` | `FrameRenderer` | 仅 CPU 的当前/已提交相机、帧序号和积累计数 |
| `FrameResources`/`ViewHistory` | `FrameRenderer` | 按 GBuffer、Lighting、ReSTIR、Denoiser、Effects 分组声明图资源，以及跨帧历史 |
| `GraphTexture`/`GraphBuffer`/`RenderTarget` | 对应资源组 | 单个图资源的描述和稳定逻辑代理；RenderTarget 只允许颜色或深度附件 |
| `RenderPipeline` | 宿主或 `FrameRenderer` | 固定阶段入口及外部模块的前后 Pass、递归 Subpass 注册 |
| `PipelineCache`/`PassContext` | `FrameRenderer` / 帧局部 | Pipeline 复用、Pass 录制所需的非拥有资源访问 |
| `FrameRenderer` | `Dx12Renderer::Impl` | 每帧快照、资源、Pass 组合、history reset |
| `EditorUi`/`CaptureService` | `Dx12Renderer::Impl` | UI 生命周期/组件编辑，以及独立 capture 图读回 |
| `EditorNavigation`/`EditorHistory` | `EditorUi` | 纯 CPU 相机导航数学，以及有界的作者操作撤销/重做 |
| `EditorObjectPalette` | `EditorUi` | 常用物体搜索、拖放源、放置预览和输入捕获；实体与共享资源由 World 创建 |

Editor 的 ImGui font upload 由 `Dx12Renderer::Impl` 注册命名 `Editor.UploadFonts` Pass 调用 `EditorUi::CreateGpuObjects()`；`EditorUi` 构造函数只初始化 context/backend。最终 `EditorUi::AddOverlayPass()` 注册 `Editor.ImGui`，由最后的 overlay callback 调用 `Draw()`；调用者先声明并绑定 backbuffer RTV 和 viewport SRV，Editor 不自行切换资源状态。

常用物体的模板来自正式 Protobuf 内容。`WorldObjectPresets` 在 Renderer 初次烘焙前安装共享源范围，拖放只生成实例组件；`EditorObjectPalette` 处理落点与预览，视口把创建交给已有历史和默认值系统。这样对象库不反向依赖 ImGui，也不把几何构建重新塞入宿主，详见 [`ObjectPlacement.md`](ObjectPlacement.md)。

## 4. CPU 世界到 GPU 场景

每帧的边界顺序为：

1. `World.Update(delta)` 在 `RenderSettings.simulate` 为真时推进带 `Velocity` 的实体；成功的实际移动会提升 world revision。
2. `World.ExtractRenderSnapshot()` 复制可见 `MeshRenderer`、active `Camera`、enabled `Light` 和可编辑材质表，并按实体 ID 稳定排序。快照不持有 registry 指针，不含 GPU 资源；旧快照不会被后续材质编辑修改。
3. `SceneExtraction` 将 snapshot 转换为 Renderer 所需的几何、材质和灯光；普通网格按 scale、X/Y/Z rotation、translation 顺序变换参考三角形，并应用 material/visible。平滑法线使用逆缩放和旋转，镜像编辑同步反转绕序、UV/法线顶点关联及切线手性。首个实例使用原始 primitive 槽位；不同实体共享相同非空 MeshResource 句柄与完整源范围时，后续实例追加独立变换的三角形和逐三角形属性，并重定向 `b.w`。部分范围重叠和重复实体仍被拒绝。没有实例占用的源范围写为远处退化三角形，矩形和圆盘灯追加可见发光几何。
4. `SceneGpu` 对新 revision 先用 `SameRenderSceneInputs` 比较最近成功接受的实例、材质和灯光输入。只有相机或非渲染作者信息变化时，校验全部相机后接受新版本，跳过场景复制、bounds、shadow views 和 CPU BVH；仍向帧入口报告版本变化以重置历史。首次同步及非相机输入变化继续完整提取和构建 CPU BVH，再比较三角形、属性、材质、灯光和阴影视图字节，通过 RDG 上传变化数据；几何/属性变化触发上传和 DXR BLAS/TLAS 重建，当前没有 AS refit/UPDATE。比较覆盖实例资源/范围/覆盖模式和全部灯光/材质字段，不比较业务结构体填充字节。校验失败不推进已接受 revision 或输入缓存，`SceneBuildCount` 记录完整重建成功次数。参考 `Scene` 保持源 primitive 顺序，BVH 只重排渲染副本；位置中的属性间接索引使重排不破坏平滑法线和 UV。同步在提交新 CPU 场景、开始命令录制之前检查三角形和属性容量，必要时按倍增扩容三角形、BVH、属性及相关 DXR 分配。旧分配由 ResourcePool 保留至 Reset；撤销减少活动数量，重做复用已有容量。每个算法入口独立准备常量快照，通过 `GraphExecutor::UploadParameters` 声明上传生产者；只有保留的 Pass 执行时才分配、Map 和写入上传缓冲，不再共享一份每帧可变 GPU 常量。
5. `FrameRenderer::Draw` 在 snapshot revision 变化时设置 reset；Editor 的采样、相机和光源操作也显式设置 reset。分辨率、GI/backend 或未来 shader layout 热重载同样必须使旧 GI、reservoir、position/normal 和 HDR history 失效。

灯光统一打包为 `GpuLight`，支持 directional、point、rectangle、disk、spot，最多十六个启用灯光。矩形/圆盘保留独立宽高、方向和切线框架；发光网格与解析采样共用相同 pose，圆盘采用 64 边形近似。Cornell 预置灯继续拥有 material 3 和原始发光四边形，其 MeshRenderer Transform 不重复应用；普通面光生成独立发光材质和几何。`Scene.cornell` 显式标识此特例，不从文件路径或材质数量推断。

`ExtractRenderScene` 的可选 `triangleOwners` 输出与未重排的结果三角形逐项对应；普通实例映射到各自的版本化实体，发光几何映射到解析灯，无主退化槽位为空实体。只有完整提取成功才替换调用者的映射。该映射用于 CPU 拾取，不进入 GPU 数据契约；渲染器仍使用 `c.w` 中的解析灯标记。

GPU 初始三角形容量包含源几何和十六个 64-sector disk 的余量，属性容量按参考数据建立，后续按各自需求增长。扩容会等待 GPU 空闲再替换资源引用及保留的 TLAS 描述符；结构化 SRV 的增长受几何倍增次数限制。三角形计数需容纳 DXR 的三倍顶点数，属性的一基 float 索引不得超过可精确表示的 `2^24`。这是当前场景资源池的容量管理，不引入引用计数、垃圾回收或自动卸载政策，详见 [`Resources.md`](Resources.md)。

AssetImport 在进入 ECS 前完成静态资产转换：cgltf 解析/验证容器和 accessor，WIC 解码 PNG/JPEG，节点世界变换烘焙位置和法线，材质转换为 GPU 参数，punctual lights 保留名称、世界坐标框架和锥角。`Scene` 拥有 packed texels、texture views、triangle attributes、material names、mesh parts 和 lights；`World::CreateScene` 在替换旧世界前校验 primitive 范围和覆盖。导入限制及 glTF 到 Disney 的映射区别见 [`BlenderImport.md`](BlenderImport.md)。

### CPU/GPU 数据契约

| 数据 | 大小 | 含义 |
|---|---|---|
| `GpuTriangle` | 48 字节 | 三个位置；a.w 材质，b.w 属性索引加一，c.w 解析灯索引加一（零为普通几何） |
| `GpuTriangleAttributes` | 96 字节 | 平滑法线、UV0、世界切线和手性 |
| `GpuMaterial` | 176 字节 | Disney 2012 参数、透射/薄表面扩展、贴图视图和辅助参数 |
| `GpuTextureView` | 48 字节 | RGBA8 packed texel offset、尺寸、wrap 和 UV transform |
| `GpuLight` | 112 字节 | 五类灯光参数、发射框架、阴影视图范围与发光材质关联 |
| `GpuShadowView` | 80 字节 | 线性深度投影、atlas tile 和 owning light |
| `FrameConstants` | 384 字节 | 相机、采样、灯光/阴影/资产计数；上传分配仍按 512 字节对齐 |
| Reservoir | 96 字节/像素 | secondary 样本及 M、W、权重和接收点信息 |

SRV 表为 `t0..t31`，保留原有 GI 输入，并在 t16..t23 绑定灯光、阴影矩、属性、纹理视图、texels、阴影视图、GBuffer 切线和 UV。GBuffer UV 的 z 保存当前绘制 primitive 索引加一、w 保存正面标记，供材质独立的 emitter 身份及几何法线重建使用。CPU `static_assert` 与 HLSL 结构共同维护 ABI；变更这些布局必须重编译全部 shader permutation。

光栅 GBuffer 和两种求交后端共用 `TrianglePosition`，用重心权重重建并限制命中位置在源三角形的坐标范围内。仅光栅覆盖边界的非正权重采用极小的内部代表点，保留共享边覆盖；软件三角形与 DXR 的近距离区间均从零开始，避免在已有几何法线起点偏移后再次跳过相邻遮挡面。该修复不改变公共 GPU 数据布局；有限精度近似、尺度边界及真实 HDR 回归见 [`ContactVisibility.md`](ContactVisibility.md)。

## 5. RDG 帧层次

图拥有的纹理/缓冲声明、存活区间分配复用、外部历史提取和真实 GPU 测试见 [`RenderDependencyGraph.md`](RenderDependencyGraph.md)。

Renderer 将所有 GPU 工作注册为图 Pass，RHI 的 `GraphExecutor` 只执行编译后的计划：

`DeferredRender()` 固定调用 Prepare、Shadows、BasePass、DirectLighting、GlobalIllumination、Denoise、ScreenEffects、PostProcess、History、Overlay 阶段。各阶段保留稳定入口，功能开关只决定内部是否注册工作。外部模块经公共 `RenderPipeline` 注册阶段之前/之后的 Pass 和递归 Subpass；黑板传递命名图资源，`PassParameters` 同时产生依赖及实际绑定。扩展不能在注册回调中录制 GPU 命令，原生工作必须放进 RDG 执行回调。正式接口、独立模块示例和卸载边界见 [`RenderPipelineExtensions.md`](RenderPipelineExtensions.md)。

```text
Scene upload / initial clear
  -> DXR.BuildBLAS / DXR.BuildTLAS (when needed)
  -> Shadow.RasterDepth (PCF / PCSS / VSM)
       -> Shadow.BlurHorizontal -> Shadow.BlurVertical (VSM only)
  -> Raster.GBuffer
  -> GI branch:
       GI.PathTracing
       or Lighting.Direct -> GI.SSGI
       or Lighting.Direct -> ReSTIR.Candidates -> Temporal -> Spatial -> Evaluate
  -> Resolve.Temporal -> Resolve.Spatial
  -> Sky / SSAO / SSR / SSIL / SSS (enabled effects only)
  -> Post.VolumeBloomACES
  -> Editor.ImGui
  -> backbuffer PRESENT

Lighting.GI -> History.GI
GBuffer -> History.Position + History.Normal
Resolve.Temporal -> History.Resolved
ReSTIR.Spatial -> History.Reservoir (ReSTIR mode only)
```

上图表达数据依赖和分支。历史通过 `QueueExtraction` 延长至图末端并交换默认堆资源所有权；诊断快照才使用 copy 提取。每个 Pass 声明读写资源和所需 D3D12 状态；图编译器推导值依赖、冲突顺序、UAV ordering、资源存活和最终状态。最终覆盖层只添加纯排序边，不会因此保活无用 Pass。Present 是图外的 swapchain 动作，但 backbuffer 必须由导出声明恢复到 `PRESENT`。CPU 配置、descriptor 创建和 shader/pipeline cache 不属于 GPU Pass；上传、AS build、dispatch、draw 和 capture readback 均有图记录。

阴影层为方向光建立正交视图、点光建立六面视图，矩形/圆盘/聚光建立透视视图；最多 96 个 atlas views。RasterDepth 写入线性深度一、二阶矩及独立硬件深度。PCF 手工比较原始矩中的深度，PCSS 在未模糊深度上搜索 blocker，再计算滤波半径；VSM 独立横纵模糊并应用方差下限和漏光约束。所有滤波限制在当前 tile，不能跨 tile 采样。三种 GI 的 NEE 均使用选定阴影模式；Path Tracing 选择 `rays` 时提供真实求交可见性参考，选择 shadow map 时属于有偏的滤波近似。

## 6. GI 与 resolve 边界

`PathTracingPass` 输出完整相机路径 radiance 和 progressive sample state；`SSGIPass` 只读取屏幕空间 GBuffer/Direct/历史资源，不能提供屏幕外或被遮挡的真实 GI；`ReSTIRGIPass` 输出 96 字节 reservoir 与 raw indirect GI，DXR inline RayQuery 和 Compute software BVH 只替换 `TraceScene` 求交后端，不改变 target/PDF 公式。

`Materials/Disney.hlsli` 独立实现表面 BSDF 的评估、混合 PDF 和采样：Burley diffuse/retroreflection、局部 subsurface approximation、sheen、各向异性 GGX、GTR1 clearcoat、精确 dielectric Fresnel、粗糙/平滑反射与透射，以及 2015 thin/diffuse-transmission 扩展。delta 分支携带离散概率；表面参数完整不代表 ReSTIR 的 secondary 重连已覆盖任意镜面路径，也不代表实现体积 BSSRDF。材质采样层读取 UV/切线、法线贴图、扩展纹理和 alpha MASK；两种求交后端与光栅消费相同 alpha 测试。

`RadianceResolvePass` 是独立的轻量重建层：

- 用 position、normal、material 和重投影历史验证当前/上一帧兼容性。
- ReSTIR 静止视图累计 HDR 均值，World/camera revision 与参数 reset 立即使均值失效；reservoir M 只限制复用历史，不限制辐射积累。
- SSGI 对兼容 3x3 邻域计算均值、方差和 min/max，将短历史限制在均值 ±1.75 sigma 与邻域范围内。
- 用小型 3x3 cross-bilateral 权重合成 spatial radiance，再交给 PostProcess 的曝光、25 tap bloom、ACES 和 sRGB 编码。

`History.Resolved` 保存 temporal 输出，空间滤波结果仅流向后处理；这使重建历史与 Path Tracing/SSGI 的 `History.GI` 保持独立，且不会把空间滤波反复反馈进时间积累。混合模式只在 resolve 中把 Direct 与 GI 合成一次，Path Tracing 的完整路径 radiance 不再叠加 Direct。

该层不是 NRD 或完整 TAA，光栅 GBuffer 没有 camera jitter 或动态对象 motion-vector resolve。SSGI 短历史权重最多 0.97、confidence 最多 32；ReSTIR HDR 均值单独保存累计 sample budget。空间滤波不把邻居计为额外时间样本。Path Tracing progressive 输出以及 ReSTIR `M/W/normal` 调试视图绕过两轮滤波；ReSTIR 的尖锐反射（roughness < 0.35）、clearcoat 和透射 primary 使用完整相机路径回退，进行像素内采样并绕过漫反射空间滤波。

## 7. Editor 到 Runtime 的交互

正式世界文档采用官方 Protobuf，层级为 World → Level → Scene → Object。World 拥有 Resource 目录，Object 引用 Mesh、Material 及 Texture；Scene 只组织对象。`render::Scene` 是 Renderer 的扁平传输结构，其材质表不承担作者关联关系。文档与外置图像说明见 [`WorldDocuments.md`](WorldDocuments.md)，实际角色展示资产见 [`CitlaliScene.md`](CitlaliScene.md)。

内容浏览器、详情、世界文件操作、材质 HLSL 编辑、Lua Gameplay 和 Profile 分别由 Editor 独立模块组合。工具窗口可缩放、重排停靠或拖出为原生平台窗口。HLSL 在后台编译，完整 PSO 候选在渲染线程原子发布；Lua 在 UI 前更新以显示当前位姿。详见 [`Scripting.md`](Scripting.md)。性能采集默认关闭，由编辑器显式开始/停止录制，最多保留 600 个完成帧供回看。逐 Pass GPU 计时来自原生 timestamp，完成 fence 后才交给性能面板；CPU/GPU 各自使用实际起止时间绘制时间线，不将累计耗时冒充事件位置。采集范围及暂停时的执行路径见 [`Profiling.md`](Profiling.md)。

新增屏幕效果与天空通过独立 RDG 模块注册，使用图内瞬态目标，最终 UI 始终在后处理之后叠加。各算法和计能边界见 [`ScreenSpaceEffects.md`](ScreenSpaceEffects.md)。

视口 Gizmo 通过独立的 `EditorGizmo` 数学/拾取模块与 `EditorGizmoUi` ImGuizmo 适配层写回 ECS Transform，最终像素仍由 Editor overlay 合成。网格围绕源 AABB 中心编辑，CPU 拾取遵守背面与 alpha MASK 接受规则，并使用提取后的实体归属识别共享网格副本，详情见 [`EditorGizmo.md`](EditorGizmo.md)。

CPU 资产使用 `engine::Resource` 公共基类，TextureResource/MeshResource/MaterialResource 由 World 的 ResourceStore 显式拥有，非拥有句柄不执行引用计数或 GC。`CookResourceScene()` 在导入边界产生 Renderer 的不可变传输副本，材质快照从资源中的唯一编辑参数复制，详情见 [`Resources.md`](Resources.md)。

Editor UI 只编辑 Renderer/Gameplay 的公开数据：GI mode、backend、samples、bounce、ReSTIR candidates/history/radius、shadow/medium/post 参数，以及 ECS entity 的 Name/Transform/MeshRenderer/Camera/Light/Velocity。所有直接 component edits 都调用 `World.MarkDirty()` 并设置 `RenderSettings.reset`。相机 viewport 使用 active ECS Camera 的完整 `rotationDegrees`：右手坐标下按 X/Y/Z Euler 顺序旋转，包含 roll，正 pitch 向下，与网格 Transform 约定一致。

`EditorViewportInput` 负责视口焦点、鼠标手势捕获和键盘路由，`EditorNavigation` 只处理纯 CPU 数学与持续飞行速度/焦点。RMB 飞行、世界 +Y/-Y 升降、MMB/两键平移、Alt 环绕/平移/推进及 F 取景共用 `MakeCameraBasis`，不通过修改 FOV 导航。F 根据可见源网格经 ECS 变换后的边界或灯光发光范围取景，并保留轨道焦点；相机 roll、scale 和投影参数保持不变。操作对应关系及未实现的 UE 功能见 [`EditorControls.md`](EditorControls.md)。

`EditorHistoryUi` 将检查器的一次连续交互或一次 Gizmo 拖动记录为作者操作事务，`EditorHistory` 只保留实际变化的组件、材质参数 lane 和渲染设置，最多 128 步。创建、复制、删除、属性编辑及 Gizmo 变换可以撤销/重做；回放恢复版本化实体身份和相关选择，先检查身份冲突，再写回世界并重置 GI 历史。Gizmo 事务只捕获实际目标 Transform，不回滚其他实体的模拟位移。文字输入保留 ImGui 原生局部撤销；相机导航、自动模拟、Play/Pause 和临时 reset 标记不生成作者操作历史。成功替换场景时清空历史，历史中的资源句柄不延长资源寿命。

Editor 的 `BeginFrame -> Build -> EndFrame` 只生成 ImGui draw data，不改变场景资源状态或调用 `Present`。字体由独立 `Editor.UploadFonts` Pass 调用 `CreateGpuObjects()` 创建；最终 Editor Pass 通过 `Draw()` 合成到已声明的 backbuffer。渲染视图使用固定 descriptor slot 1，font slot 0；viewport 图像是已编码的 UNORM 显示结果，UI 不再次做 HDR tone mapping。

启用多视口时，主图提交并等待完成后，宿主再执行独立的 `Editor.PlatformWindows` 图，显式读取 Display。ImGui DX12 平台后端拥有各自交换链、direct queue 与 fence，并在平台呈现结束时等待，之后下一主帧才能覆写共享图像/描述符。平台交换链是外部资源边界，不能假设主图的 barrier 会自动覆盖另一条队列。离屏测试关闭多视口并使用内存布局，避免覆盖用户的个人窗口布局。

## 8. 可测试边界

- Core/RenderCore：Math、BVH、reservoir weighted sampling、RenderGraph 拓扑/barrier/初始化错误。
- Gameplay：World create/destroy、revision、Velocity 更新、快照排序、组件过滤、源 mesh parts、材质快照及五类导入灯光。
- AssetImport：节点层级/逆转置/镜像、索引与默认属性、GLB/data URI、WIC 图像、纹理变换和通道打包、命名 punctual lights、格式拒绝。
- DirectX12RHI：adapter/DXR capability、descriptor range、resource pool、GraphExecutor 状态映射；无 GPU 时报告 unavailable，不伪造成功。
- Renderer：SceneExtraction 的 transform/material/light 变化、共享源范围的独立三角形/属性/归属、非法重叠拒绝、生成面光几何、payload 校验、ShadowViews 投影、历史 reset、Pass 组合和 shader permutation；GPU 测试还覆盖实例增长后的几何/属性容量与撤销/重做图像恢复。
- GPU 数值：Disney 采样 PDF/吞吐一致性、参数响应和有限性；这些断言不替代所有材质的全角度积分或严格能量证明。
- EngineEditor：导航数学、变换后的 F 取景、带 roll 的平移/轨道旋转、稀疏事务合并及抵消、实体身份冲突预检、128 步历史、资源句柄保留、原生文字撤销、真实 UI 的复制/删除/导航与场景替换；ImGui draw 的最终像素由 GPU smoke/capture 验证。

验证命令和实际结果见 [`docs/Validation.md`](Validation.md)。静态源代码检查或单次 PNG capture 不能代替 GPU 正确性验证；报告中的截图与 Debug Layer 检查也不等价于 HDR 参考精度、性能基准或逐射线后端一致性。
