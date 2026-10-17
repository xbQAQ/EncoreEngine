# EncoreEngine

项目使用 Git 管理，仓库地址为 [xbQAQ/EncoreEngine](https://github.com/xbQAQ/EncoreEngine)。

这是一个面向 Windows x64 的 DirectX 12 / RDG 渲染器实验工程，提供独立纯光栅管线，以及 Path Tracing、SSGI 和 ReSTIR GI；光追求交可选 DXR 1.1 inline RayQuery 或 Compute 软件 BVH。支持 FBX/glTF/GLB 静态导入、多光源、PCF/PCSS/VSM、Disney 材质、SSAO、SSR、SSIL、屏幕空间 SSS 和解析天空。中文编辑器提供内容浏览器、对象详情、默认恢复、撤销、材质 HLSL 热编译、Lua Gameplay 和逐 Pass CPU/GPU Profile。

场景使用官方 Protobuf 文档组织为 World → Level → Scene → Object；Mesh、Material、Texture 通过对象引用绑定，共享 Resource 目录由 World 拥有。文档说明见 [`WorldDocuments.md`](docs/WorldDocuments.md)，脚本接口见 [`Scripting.md`](docs/Scripting.md)，图资源生命周期见 [`RenderDependencyGraph.md`](docs/RenderDependencyGraph.md)，新增效果及近似边界见 [`ScreenSpaceEffects.md`](docs/ScreenSpaceEffects.md)。实际验证结果见下文链接的验证记录。

原始需求的逐项实现状态、基础实现与未完成差额见 [`ImplementationStatus.md`](docs/ImplementationStatus.md)，历史设计与旧测试批次不代表所有需求已完成。

模块边界、依赖方向、Thin PImpl 和 RDG 执行链见 [`docs/Architecture.md`](docs/Architecture.md)。ReSTIR GI 的推导、reservoir 布局、Pass 数据流、历史失效、调试指标和已知限制见 [`docs/ReSTIR-GI-Learning.md`](docs/ReSTIR-GI-Learning.md)。构建、CPU 测试和 RTX 5070 六模式验证记录见 [`docs/Validation.md`](docs/Validation.md)；接触漏光修复与保留的 PBR 反射见 [`docs/ContactVisibility.md`](docs/ContactVisibility.md)。公共 GPU 契约见 [`Source/Runtime/RenderCore/Public/render/GpuTypes.h`](Source/Runtime/RenderCore/Public/render/GpuTypes.h) 和 [`docs/superpowers/specs/2026-10-15-gi-interface.md`](docs/superpowers/specs/2026-10-15-gi-interface.md)。

## 环境要求

- Windows 10/11 x64。
- Visual Studio 2022（MSVC，C++20）与对应 Windows SDK。
- CMake 3.24 或更新版本。
- DirectX Shader Compiler（`dxc.exe`），用于 HLSL 编译；CMake 自动查找 Windows SDK `bin` 目录，或从 `PATH`/`-DDXC_EXECUTABLE=...` 指定。Compute 变体使用 Shader Model 6.5。
- DirectX 12 GPU，硬件后端还需 DXR 1.1；已验证设备为 RTX 5070，范围见验证报告。软件 BVH 不要求 DXR，但仍需当前 shader profile 的设备支持。

CMake 通过 FetchContent 获取固定版本 EnTT `v3.15.0`、Dear ImGui `v1.91.9b-docking`、cgltf `v1.15`、ufbx `v0.23.1`、Protobuf `v21.12` 和 Lua `5.4.8`。PNG/JPEG 通过系统 WIC 解码。Lumen、Nanite、完整 RTXDI SDK 和外部降噪器不属于当前实现依赖。

## 配置与构建

在 Developer PowerShell for VS 2022 中获取源码并构建：

```powershell
git clone https://github.com/xbQAQ/EncoreEngine.git
cd EncoreEngine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

发布构建：

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

构建后会自动在工程根目录生成或更新 `EncoreEngine.exe`，对应最近一次构建的 Debug/Release 配置。双击即可启动，也可执行 `.\EncoreEngine.exe`；默认增量构建会补齐被删除的根目录入口。
编辑器运行期间重新构建时，会保留运行中的旧映像到 `build/RootEntry`，根目录入口更新为新版本；当前窗口不受中断，重新启动后使用新功能。

Windows 默认关闭 `ENCORE_ENABLE_GPU_TESTS`，注册 Core、ECS、资源、场景提取、阴影投影、资产导入、编辑器数学/历史和 CLI 参数检查等 CPU/CLI 测试。启用真实 GPU 集成测试：

```powershell
cmake -S . -B build -DENCORE_ENABLE_GPU_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

启用后增加 Disney BSDF 数值验证、编辑器导入/真实 UI 交互回归和 Renderer 集成验证。Renderer 测试覆盖后端/GI、阴影、导入场景、共享网格实例扩容与撤销/重做、ECS revision、capture 和交换链 resize；本轮实际注册数量与执行结果以 [`docs/Validation.md`](docs/Validation.md) 为准。交换链析构会先 `WaitIdle()`，确保 Present 完成后才释放 backbuffer。切回仅 CPU/CLI 测试时，重新配置 `-DENCORE_ENABLE_GPU_TESTS=OFF`。

如果 CMake 找不到 `dxc.exe` 或 Windows SDK，应先在 VS 开发者环境中确认工具路径；不要把本机绝对路径提交到工程配置。GPU 验证需记录实际 adapter、feature level、Shader Model、DXR tier、Debug Layer 状态和输出截图路径。

## 运行参数

当前命令行由 `Source/Runtime/Launch/Private/Main.cpp` 解析：

```text
EncoreEngine.exe --pipeline raster|raytracing|dxr
                  --gi pathtracing|ssgi|restir --backend software|dxr
                  --frames N --spp N --width 640 --height 480
                  --capture captures/x.png --capture-ui captures/editor.png
                  --scene path/to/world.textproto --shadow rays|pcf|pcss|vsm
                  --headless --validation
```

`--frames 0` 持续运行至窗口关闭，`--headless` 使用隐藏窗口执行同一设备/交换链路径，`--validation` 请求 D3D12 验证。`--capture` 只保存渲染视图，`--capture-ui` 保存编辑器 backbuffer。`--spp` 为 1..16，尺寸范围为 64x64 至 3840x2160。参数错误会显式失败；当前没有 `--help` 参数。

不带管线参数时编辑器默认纯光栅；`--pipeline dxr` 选择硬件路径追踪。纯光栅程序在编译时移除软件 BVH 和 DXR 查询，阴影使用 shadow map。`--scene` 可打开 `.textproto`/`.pb` 世界或 `.fbx`/`.gltf`/`.glb` 模型。左上角常驻“选择场景”按钮列出 `Content/Worlds` 中的项目配置，仓库包含 `default.textproto`（Cornell Box）和 `material-lab.textproto`（材质实验室）；本地另行生成的 `citlali.pb`（茜特拉莉）也会自动列出。“浏览场景文件”可打开外部配置。世界面板仍可保存世界和切换文档中的场景；内容目录在构建后复制到程序旁。

PowerShell 示例（先完成 Release 构建）：

```powershell
.\EncoreEngine.exe --gi restir --backend software --frames 120 --width 640 --height 480 --capture captures/restir-software.png
.\EncoreEngine.exe --gi restir --backend dxr --frames 120 --width 640 --height 480 --capture captures/restir-dxr.png
.\EncoreEngine.exe --gi ssgi --backend software --frames 120 --width 640 --height 480 --capture captures/ssgi.png
.\EncoreEngine.exe --gi pathtracing --backend dxr --frames 256 --spp 4 --width 640 --height 480 --capture captures/pathtracing-dxr.png
.\EncoreEngine.exe --scene tests/fixtures/textured_multilight.gltf --gi restir --backend software --shadow pcss
```

`--backend dxr` 在设备不支持 DXR 1.1 时明确报错；此时使用 `--backend software`。SSGI 不依赖 DXR，但其屏幕空间限制仍然存在。

省略 `--frames` 可交互运行。渲染面板可切换管线、GI/求交后端、ReSTIR 复用和调试视图，并调整曝光、泛光、阴影、介质与屏幕效果。光追模式显示 SPP，光栅模式仅在开启 SSGI 时显示“间接光采样”，纯直接光不提供 SPP 控件。世界窗口管理场景和实体，详情窗口显示实时 Transform、材质、Camera、Light、Velocity 与脚本；“导入模型”将静态 FBX/glTF/GLB 追加到当前场景，每个文件是可整体操作的模型对象，内部保留各表面材质与纹理。组件或材质修改会提升 World revision 并重置 GI 历史。

各工具窗口可拖动页签自由停靠、缩放或拖出主窗口，布局保存在 `%LOCALAPPDATA%/EncoreEngine/editor-layout-v3.ini`，旧版布局文件保留。性能窗口默认不采集，点击开始/停止录制后可逐帧查看真实 CPU/GPU 时间线，支持缩放、滚动和搜索；每次最多录制 600 帧，见 [`Profiling.md`](docs/Profiling.md)。项目文件列表占用实际剩余高度。窗口与操作说明见 [`EditorControls.md`](docs/EditorControls.md)。固定渲染阶段及外部模块的 Pass/Subpass 扩展接口见 [`RenderPipelineExtensions.md`](docs/RenderPipelineExtensions.md)。

左侧“常用物体”提供立方体、平面和五类光源，可搜索后拖到视口，预览落点并在松开时创建。新对象立即选中，支持撤销、恢复创建默认值和保存重开；模板也使用 Protobuf 配置，详见 [`ObjectPlacement.md`](docs/ObjectPlacement.md)。

内容浏览器的“项目文件”页签支持通过“+ 新建”或列表右键菜单创建文件夹、空文件、HLSL 材质脚本和 Lua 游戏脚本。名称支持中文，新条目创建在当前目录，成功后刷新并选中；脚本模板包含现有编译器要求的入口，并自动补全省略的 `.hlsl`/`.lua` 扩展名。重名、非法名称或写入失败会保留对话框供修改，取消不创建内容。文件操作限定在项目 `Content` 内，不覆盖已有条目。

视口采用 UE 常见核心操作：`Q/W/E/R` 切换选择/移动/旋转/本地缩放，`Space` 循环变换工具，`F` 对所选实体取景；`RMB + WASD/Q/E` 飞行，`MMB` 或左右键同时按住拖动平移，`Alt + LMB/MMB/RMB` 分别环绕/平移/推进。右键飞行时滚轮调整持续移动速度，其他情况下滚轮朝焦点推进或后退。`Ctrl+D` 复制所选实体，`Delete` 删除；Camera 实体受保护。完整操作、焦点规则和支持边界见 [`EditorControls.md`](docs/EditorControls.md)，Gizmo 坐标与拾取实现见 [`EditorGizmo.md`](docs/EditorGizmo.md)。

编辑器提供最多 128 步撤销/重做：`Ctrl+Z` 撤销，`Ctrl+Y` 或 `Ctrl+Shift+Z` 重做，也可使用 `Edit` 菜单。一次连续属性拖拽或 Gizmo 拖动合并为一次编辑；文字输入保留原生局部撤销。成功导入新场景会清空编辑历史，相机导航、F 取景和自动模拟不记入撤销。历史只保存作者操作的变化值，不复制或持有场景资源。

CPU 资产采用 Godot 式资源层级：TextureResource、MeshResource、MaterialResource 继承 Resource，由 World 显式拥有。网格副本共享不可变 MeshResource，在提取时展开为独立变换的三角形、属性和拾取归属；不会复制源资产。GPU 几何容量按需增长，旧分配保留至资源池重置。句柄不延长资源生命周期，当前没有 RefCounted、引用计数或 GC，详见 [`Resources.md`](docs/Resources.md)。

支持方向、点、矩形、圆盘、聚光灯，最多 16 个启用灯光，超限明确报错。矩形/圆盘可旋转并独立调整宽高，可见发光几何与解析采样使用同一框架。纯光栅面光源采用 LTC 与固定节点的 Disney 材质修正，直接光不依赖帧随机种子或 SPP；积分、阴影近似和数值验证见 [`LTC.md`](docs/LTC.md)。PCF 手工比较阴影深度，PCSS 搜索 blocker 并估计半影，VSM 使用分离模糊矩和方差/漏光约束。三种 GI 均使用所选阴影模式；Path Tracing 选择 `--shadow rays` 才能作为射线可见性的参考，选择 shadow map 会引入滤波近似。

Blender 可导出 FBX 或 glTF 2.0，不能直接打开 `.blend`。FBX 骨骼和形态键只烘焙文件默认姿态，不提供动画播放。导入内容、导出步骤、纹理/材质映射和明确拒绝的格式见 [`docs/BlenderImport.md`](docs/BlenderImport.md)。

## 验证与后续对照

构建、DXC `-WX` shader permutation、CPU/CLI/数值测试、GPU 截图与 Debug Layer 的实际执行记录见 [`docs/Validation.md`](docs/Validation.md)。源码已注册测试不等同于验证已执行；报告区分静态检查、CPU 断言、GPU 数值结果和图像读回。

后续应以固定 Cornell Box 和手动曝光做定量对照：Path Tracing 作为慢速参考，先比较 DXR/软件命中和可见性，再比较 raw HDR GI。补充长时间相机/模拟运动、minimize/restore、低粗糙镜面、屏幕边缘和屏幕外遮挡；报告平均/P95 `M`、`W`、历史拒绝原因、非有限数计数和 GPU Pass 时间。

## 目录提示

```text
Source/Runtime/Core/        平台无关基础类型与数学
Source/Runtime/RenderCore/  场景、BVH、reservoir、RenderGraph 与 GPU 契约
Source/Runtime/Gameplay/    EnTT ECS、World、组件与 RenderSnapshot
Source/Runtime/AssetImport/ cgltf、ufbx、WIC、材质映射和静态模型转换
Source/Runtime/DirectX12RHI/ D3D12 设备、描述符、资源池和 RDG 执行器
Source/Runtime/Renderer/    场景提取、分组图资源、固定阶段与独立算法 Pass
Source/Runtime/Launch/      Win32 入口与命令行
Source/Editor/EngineEditor/Public/  Dx12Renderer/EditorUi 公共宿主接口
Source/Editor/EngineEditor/Private/ Editor PImpl、ImGui overlay 与 ECS 检查器
Shaders/Private/            公共、Materials、Shadows、Deferred、Lighting、GI、Resolve、Post HLSL
docs/Architecture.md        模块边界和 RDG 数据流
docs/ReSTIR-GI-Learning.md  ReSTIR GI 学习、推导与验证说明
docs/Validation.md          构建、测试、GPU 验证与已知覆盖范围
docs/BlenderImport.md       Blender 导出、资产支持边界和导入验证
docs/EditorControls.md      视口操作、编辑快捷键、撤销/重做和支持边界
docs/EditorGizmo.md         Gizmo 坐标契约、中心枢轴和表面拾取
docs/Resources.md           CPU 资源所有权、共享实例和 GPU 容量增长
docs/WorldDocuments.md      官方 Protobuf 世界层级、对象引用和读写
docs/FbxImport.md           FBX 默认姿态、分面、纹理与拒绝边界
docs/CitlaliScene.md        实际角色世界、正式生成工具和资源载荷
docs/Scripting.md           HLSL 材质热编译与 Lua Gameplay 接口
docs/ScreenSpaceEffects.md  SSAO、SSR、SSIL、SSS、解析天空的实现边界
docs/superpowers/specs/     已批准的接口与设计规格
tests/                      CPU/ECS/RDG 测试入口
```

## 已知限制

- ReSTIR GI 是有限候选、有限历史和相关复用的有偏实用估计器。
- Disney 表面模型实现 Burley diffuse/retroreflection、局部 subsurface 近似、sheen、各向异性 GGX、GTR1 clearcoat，以及粗糙/平滑反射与透射、薄表面 diffuse transmission；局部 subsurface 项不是体积 BSSRDF，参数实现也不保证任意材质下严格能量守恒。
- 当前 ReSTIR secondary outgoing radiance 仍有 diffuse 近似，完整表面 BSDF 不等于所有 GI 路径都能无偏复用；尖锐镜面焦散与复杂 delta 重连不在覆盖范围内。
- ReSTIR 的 roughness < 0.35、含 clearcoat 或透射 primary 使用独立 `TracePath` 回退，尖锐反射/透射路径没有参与通用 reservoir 重连；回退输出完整相机 radiance，Resolve 不重复叠加直接光。
- SSGI 缺少屏幕外和被遮挡屏幕信息，历史复用不能恢复从未可见的场景信息。
- 当前 SSGI 是有限步数屏幕射线与历史辐射钳制，没有 Hi-Z、离屏 GI fallback 或 NRD；后处理曝光是 16 点稀疏测光，Bloom 是 25 tap 滤波，没有完整直方图适应和独立 TAA 系统。
- ReSTIR 候选使用完整 Disney BSDF mixture/VNDF，普通像素每帧新提案为 SPP × Candidates，空间复用固定 6 邻居；历史采用静态世界位置的相机重投影，尚无动态对象 motion vector。
- `RadianceResolve` 对静止 ReSTIR 累计 HDR 均值，SSGI 保留短窗口方差裁剪；小型空间滤波跳过完整 BSDF 路径像素。该层不是 NRD 或完整 TAA，光栅 GBuffer 仍无 jitter/motion-vector resolve。
- 几何变更通过 ECS 快照重新提取，重建软件 BVH，并在 DXR 可用时重建 BLAS/TLAS；没有 AS refit，启用灯光最多 16 个，阴影 atlas 最多 96 个视图。圆盘发光面使用与采样一致的 64 边形近似。
- 软件 BVH 当前采用中位数划分；八叉树只有独立的静态三角形范围查询，尚未接入渲染对象管理。距离、对象视锥、八叉树层级和 Hi-Z 遮挡剔除尚未实现，不能将 RDG 无用 Pass 裁剪视为场景剔除。
- 编辑器当前为单选透视视口，只实现 UE 常见核心操作子集；尚无正交视图、框选/多选、顶点吸附、临时枢轴编辑或 Alt 拖动 Gizmo 复制。共享网格实例在渲染提取时展开，不是 GPU 硬件实例化；容量增长不引入新的资源 GC 政策。
- glTF 导入仅支持已列明的静态、PNG/JPEG 和 UV0 数据；动画、蒙皮、压缩几何、任意 Blender 节点图、alpha BLEND 等会明确拒绝。Disney 参数化与 glTF specular/sheen 的任意颜色和独立 sheen roughness 不完全等价，详见导入文档。
- PCF/PCSS/VSM 具有有限 atlas 分辨率、深度偏移、视锥覆盖和滤波近似，VSM 仍可能漏光；它们不替代 Path Tracing 的精确几何可见性。Lumen、Nanite 尚未实现。
- DXR 与 software BVH 的一致性依赖相同的几何、epsilon、背面和透明策略；未经固定射线对照不能声称完全一致。
- 四张 96 字节/像素的全分辨率 reservoir 在 640x480 下合计 112.5 MiB；高分辨率运行应考虑显存与带宽。
- 性能面板分别显示 CPU 录制、图编译、屏障提交和 GPU timestamp；GPU 结果在对应 fence 完成后读取。PNG 为 sRGB 显示输出，定量 HDR 误差需要独立读回。
- SSR/SSAO/SSIL 受屏幕信息和固定步数限制；SSS 是材质遮罩控制的屏幕扩散，天空是解析环境，均不替代体积 BSSRDF 或完整物理大气。新增开关默认关闭，需在渲染面板中启用。
- 本 README 不把单次截图当作 HDR 正确性或性能证明；已执行的构建、测试和 RTX 5070 GPU 结果见 [`docs/Validation.md`](docs/Validation.md)。

## 茜特拉莉展示场景

角色资源的原始说明禁止二次配布，因此 Git 仓库不包含原始模型、`Content/Worlds/citlali.pb` 和 `Content/Models/Citlali`。生成工具、导入代码与通用示例仍随仓库提供；本地已有资源保留。取得相应资源与使用授权后，可按 [`CitlaliScene.md`](docs/CitlaliScene.md) 生成展示世界。

本地验证场景将茜特菈琳、夏日 FBX 主体及书、杯、武器导入同一个 `citlali.pb`，共 5 个可整体编辑的模型，保留内部 31 个材质表面与 12 张完整分辨率图像。生成后使用内容浏览器的“项目文件”进入 Worlds 并打开 `citlali.pb`，或执行：

```powershell
.\EncoreEngine.exe --scene .\Content\Worlds\citlali.pb
```

配置包含正面相机、三盏展示面光和开放地面；纹理位于 `Content/Models/Citlali`，需与世界一同保留。模型为文件默认姿态的静态烘焙，详细来源、生成命令和边界见 [`CitlaliScene.md`](docs/CitlaliScene.md)。
