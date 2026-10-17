# EncoreEngine 验证记录

最新验证日期：2026-10-17。当前验收覆盖常用物体栏、配置化预设和视口拖放、显式 Profile 录制，以及 Protobuf 世界层级、对象资源引用、FBX/glTF/GLB、中文编辑器与内容浏览器、默认恢复、HLSL/Lua、RDG 资源管理、独立光栅与三种 GI/双求交后端、SSAO、SSR、SSIL、SSS、天空和原有材质、阴影、交互回归。本文区分 CPU 契约、GPU 数值、图像变化与人工截图检查，不将图像非空视为积分精度或性能证明。

历史功能验收的 Release 全量构建成功，CTest **35/35 通过**，用时 **148.40 秒**，其中 **13 项标记为 GPU**。真实设备回归覆盖时间戳、材质完整编译与失败保留、纯光栅、屏幕效果、物体拖放、编辑器输入与平台窗口、Disney 数值、场景重载、渲染器及 RDG 原生资源。该历史批次不能代替以下 Git 发布快照的实际结果，运行时性能也不能由测试总耗时推算。

## 2026-10-17 Git 初始化与引擎改名

- 引擎、CMake 工程及可执行目标统一为 `EncoreEngine`，GPU 测试开关为 `ENCORE_ENABLE_GPU_TESTS`；窗口标题、诊断前缀、编辑器停靠标识、拖放协议和 Lua 内部命名同步更新。用户布局使用 `%LOCALAPPDATA%/EncoreEngine/editor-layout-v3.ini`，旧版目录保留。
- 从待提交 Git 索引导出独立源码快照，在 `build/GitHubVerification` 使用 MSVC Release 完整构建成功；复用本机已下载的固定版本依赖源码，编译输出与对象文件独立生成。公开快照不含本地角色模型、派生世界和纹理，默认世界、通用导入夹具及测试可独立构建。
- 本次完整 CTest **34/35 通过**，总耗时 **105.03 秒**，其中 **13 项标记为 GPU**。唯一失败项为 `editor_platform_windows`：窗口归属检查通过，但桌面 `BitBlt` 返回失败，未取得原生窗口像素；同时记录到 D3D12 等待 fence 值为零的 warning。其余 CPU、GPU、导入、编辑器交互、材质编译、渲染器及 RDG 测试通过。
- 等其他测试进程结束后，单独复测 `editor_platform_windows`，耗时 **9.81 秒**，仍在同一桌面截图环节失败。本轮原生多窗口最终像素验证**未通过**，不能据历史结果声称本次全部测试通过。
- 根目录 `EncoreEngine.exe` 与独立 Release 构建的 SHA-256 一致。根入口以 `--headless --validation`、光栅/PCSS、320×240 内部尺寸分别加载默认世界与本地 `citlali.pb`，均完成四帧并退出码为 0，未输出 validation warning/error；非法 SPP 的诊断前缀已核对为 `EncoreEngine:`。默认世界的编辑器截图保留在本地 `build/encore-editor.png`，已人工检查实际视口和中文面板。
- 31 份 Markdown 文档的本地链接按 Git 路径大小写检查通过，213 个代码、配置及夹具文件与构建快照逐字节一致。项目维护文件的空白、UTF-8 和旧品牌残留检查通过；第三方许可证保持上游内容。一次性计划、诊断截图、日志和构建输出保留本地，角色资源按原始“禁止二次配布”说明排除。

下文历史批次中的可执行入口和命令已统一为新名称，各批次的执行范围仍以原记录为准。

## 2026-10-17 追加验收

- `object_presets` 验证七种正式配置、安装不创建可见实例、共享资源和稳定身份、创建最终姿态只应用一次、修改材质后保存重开、删除原实例后的资源保留、撤销重做、导入场景兼容及 16 盏灯上限。
- `editor_object_placement` 在真实隐藏窗口向生产 ImGui 行发送拖放输入，验证落点预览、一次创建、取消、图像外释放、选择与导航隔离、撤销重做、创建默认值及 900×720 窄布局。CPU 数学另覆盖裁剪、背面与透明遮罩、表面支撑、吸附、回退平面，以及拾取缓存失效和透明层叠。
- `editor_profiler` 验证开始/停止/清空、旧帧及未完成帧拒绝、逐帧回看和 600 帧上限；`raster_pipeline` 在 GPU 上确认默认不采集、开启后产生完成样本、停止后结果保持、重启后采样序号增长。
- 完整回归曾复现 `WaitIdle` 提前退休尚未提交上传资源的问题，修正实际提交 fence 限制后，原有读回测试恢复为 `17,29`，未放宽断言。导入零面积通道提升为文档时使用默认尺寸，同时保持已有正数作者尺寸及面光尺寸的严格校验；实际导入 fixture 与原有 Gameplay 断言均通过。
- 布局菜单交互测试在尺寸变化后先提交布局帧，再读取原生页签坐标；保留真实鼠标点击与菜单打开断言。

本轮经历完整回归失败后修复，最终以以上 35 项全部通过为准。根目录入口与 `build/Workbench/EncoreEngine.exe` 的 SHA-256 已核对一致。最终程序使用纯光栅、PCSS、480×480 内部尺寸、4 帧、`--headless --validation` 分别加载 Cornell 与正式茜特拉莉世界，均退出码为 0，未输出 validation warning/error。

已人工检查 [`object-palette-editor.png`](../captures/object-palette-editor.png) 与 [`object-palette-citlali-editor.png`](../captures/object-palette-citlali-editor.png)：中文物体栏、工具页签、视口与详情没有重叠；物体栏可滚动，默认 Profile 显示未录制，实际角色与贴图可见。四帧 SSGI 仍有有限采样噪声，截图不作为收敛质量证明。功能说明见 [物体放置](ObjectPlacement.md)、[性能录制](Profiling.md)，未完成的历史需求见 [实现状态](ImplementationStatus.md)。

## 2026-10-16 历史验收

此前 Release 全量构建成功，CTest 24/24 通过，用时 245.29 秒，其中九项标记为 GPU。下列追加说明及后续详细章节保留其对应的历史验证范围。

追加的根目录入口与场景选择验证：Release 全量及增量构建均成功；移除本轮生成的根目录入口后，再次默认增量构建自动补齐，SHA-256 与构建目录程序一致。`world_document`、`editor_content_browser`、`editor_gizmo_interaction`、`editor_import_integration` 四项定向回归全部通过，用时 45.58 秒；本次未重复运行此前完整的 24 项。新增真实 ImGui 交互覆盖打开项目场景菜单、选择材质实验室、一次性请求及取消不加载。根目录 `EncoreEngine.exe` 在 D3D12 validation 下实际载入材质实验室并完成四帧，截图 `captures/scene-selector-editor.png` 已检查中文按钮和排版。

内容创建追加验证：`editor_content_browser` 检查真实文件夹和空文件创建、Unicode 名称、HLSL/Lua 模板载入、Lua 编译、同名不覆盖、Content 目录边界、非法名称与保留设备名，并通过真实 ImGui 输入覆盖工具栏与右键创建、重名后修正和取消。与 `lua_scripts`、`editor_history`、`editor_gizmo_interaction` 合计四项定向回归通过。实际构建中遇到运行中的根目录 EXE 后，发布脚本保留旧映像并更新入口；原进程仍响应，新入口 SHA-256 与构建目录一致，D3D12 validation 下完成一帧启动验证。本次没有重新执行全部 GPU 算法测试。

## 1. 环境与复现

| 项目 | 实际环境 |
| --- | --- |
| 平台 | Windows x64 |
| 编译器 | Visual Studio 2022，`E:/VisualStudio2022`，C++20 |
| CMake | `D:/CMake/bin/cmake.exe`，VS 17 2022 generator，x64 |
| Windows SDK / DXC | 10.0.26100.0 |
| GPU | NVIDIA GeForce RTX 5070 |
| 求交 | DXR 1.1 inline RayQuery、Compute 软件 BVH |
| 最终配置 | Release，GPU tests 开启，D3D12 validation |

```powershell
& D:\CMake\bin\cmake.exe -S . -B build -G "Visual Studio 17 2022" -A x64 -DENCORE_ENABLE_GPU_TESTS=ON
& D:\CMake\bin\cmake.exe --build build --config Release --parallel
& D:\CMake\bin\ctest.exe --test-dir build -C Release --output-on-failure
```

本次程序构建输出为 `build/Workbench/EncoreEngine.exe`，配置使用 `-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=D:/Render/build/Workbench`；构建会同步根目录 `EncoreEngine.exe` 作为日常入口。该构建目录与旧 `build/Release/EncoreEngine.exe` 隔离；旧进程不会自动获得本次功能。正式内容已经复制到 Workbench/Content，根目录入口直接使用工程 Content。

当前 CMake 的 shader 入口和变体统一使用 DXC `-WX -O3`，同时包含 Disney 与 LTC 数值验证入口。纯光栅变体使用 `RASTER_ONLY=1`，编译移除 BVH/TLAS 查询；屏幕效果与天空各生成软件、DXR 与纯光栅变体。Compute profile 为 CS 6.5，光栅为 VS/PS 6.0。

## 本轮新增验证

- `world_document`：官方文本/二进制往返、层级/稳定身份、场景切换、对象材质变体和外部 RGBA 载荷、另存相对路径、坏引用/截断/预算及失败原子性。
- `fbx_import`：真实 ufbx 解析、层级与材质分面、外部/嵌入图像、镜像、Unicode 路径、同图透明度共享及拒绝条件；原 glTF 九组回归继续通过。
- `editor_content_browser`、`editor_defaults`、`editor_history`：材质与脚本分配、世界打开请求、六组默认基线和二十二组作者事务回放。
- `lua_scripts`：实际 Lua 解释执行、ECS 位姿写入、错误隔离、预算、暂停与失效实体；`material_scripts` 检查成功/失败编译、旧程序保留、异步更新与真实 GBuffer 像素。
- `graph_profiler`：原生时间戳、完成 fence 与实际 GPU 复制负载；CPU/GPU 耗时独立测量。
- `raster_pipeline`：独立光栅、PCF/PCSS/VSM、SSGI、体积后处理、真实导入纹理编辑和不存在 DXR 构建 Pass。
- `screen_space_effects`：五项效果确实改变像素，零强度严格无效，SSS 材质遮罩、SSIL 不重复 SSGI、两后端 PT/ReSTIR 的天空照明以及完整材质重载。
- `editor_gizmo_interaction`：中文字体/名称、固定详情、默认恢复之后首帧撤销，以及视口材质拖放与撤销。

DXR 接触回归曾在天花板/后墙公共边发现少量外部亮度。诊断表明两面交距差为 `1.75e-8`，重建点落在公共边会漏掉零距离遮挡；为凸重心权重加入 `2^-20` 内部余量后，两后端、三种 GI、三个外部光源的原始 HDR 断言全部满足 `mean=0, max=0`，未放宽断言或容差。以下部分保留原有测试的详细覆盖说明。

真实角色资产另通过 `citlali_scene_tool` 生成并读回：四个 FBX 合计 72,611 个模型三角形，加开放地面两片；11 份唯一图像、104 MiB 原始 RGBA 和 13,941,743 字节二进制世界。纯光栅/PCSS 与 DXR 路径追踪均完成实际画面捕获；编辑器宽窗口及 900×720 窄窗口均通过人工检查，中文标签、内容列表与视口无重叠。生成、来源和截图见 [`CitlaliScene.md`](CitlaliScene.md)。

## 2. CPU 与命令行

| CTest | 测试组 | 主要断言 |
| --- | --- | --- |
| `core_algorithms` | 9 | Math/AABB、Cornell/BVH、Octree、RDG 调度/hazard/生命周期/校验、reservoir 统计 |
| `gameplay_ecs` | 11 | 生命周期、快照隔离、Velocity 原子性、mesh parts、材质、五类灯光、独立 emitter 所有权 |
| `scene_extraction` | 12 | 位姿/材质/可见性、镜像属性、面光几何、共享网格副本独立属性/归属、非法重叠和数量限制 |
| `shadow_projection` | 5 | 方向光拟合、点光六面、透视裁剪、容量/失败原子性、PCSS/VSM 数值边界 |
| `resource_model` | 5 | 显式所有权、失效/跨所有者/类型错误句柄、材质唯一数据源、纹理 view、World 移动、Cornell 模式和真实导入 |
| `asset_import` | 9 | glTF/GLB、节点层级/逆转置、稀疏属性、PNG/data URI、扩展纹理、灯光框架、Unicode 路径和拒绝条件 |
| `editor_gizmo` | 7 | 实际场景矩阵往返、带符号缩放、中心枢轴、相机投影、最近可见表面、共享副本、单面和 alpha MASK 拾取 |
| `editor_history` | 22 | 连续事务合并及抵消、稀疏组件/材质/设置恢复、脚本与身份、历史分支/128 步上限、非拥有资源与导航保留 |
| `editor_navigation` | 8 | 飞行基向量/世界升降、速度限幅、带 roll 平移、窄视口变换网格取景、环绕/推进、灯光范围与非法输入 |
| `cli_reject_excess_spp` | 预期失败 | `--spp 17` 在创建设备前明确拒绝；CTest 用 `WILL_FAIL` 检查 |

当前配置包含 22 项未标记 GPU 的测试和 13 项 GPU 测试；关闭 GPU tests 后保留前者，包括物体预设、世界、FBX、Lua、默认值、内容浏览器和 Profile UI 等。AssetImport 使用真实 cgltf、ufbx 和 WIC；合成 fixture 覆盖确定的边界，用户提供的五份 FBX 另有实际导入验证，未在本机启动 Blender 重新导出。

## 3. Disney GPU 数值验证

`disney_bsdf` 使用独立 D3D12 dispatch/readback，24 个材质/视角 case、每个 case 64 个 batch、每个 batch 256 次试验，共 393216 次散射试验。检查：

- 值和 PDF 有限且非负，公开 `BsdfPdf` 与抽样返回密度一致。
- 反射互易性及适用的透射 eta² 互易性；相对容差 0.003，近零比较下限 0.001。
- 与独立 uniform-sphere 积分比较，并用 batch 统计波动确定容差。
- 理想镜面、理想折射、TIR、空事件及离散概率。
- IOR=4 的理想玻璃使用独立 CPU Fresnel/eta² 解析期望验证，覆盖导入器、编辑器和 shader 的共同上限。
- 平滑法线与法线贴图组合不会越过实际三角形的几何半球。

delta 分支用解析值校验，不用均匀方向积分逼近 Dirac 分布。最尖锐 GTR1 clearcoat 不做有限均匀方向集的积分一致性断言，但仍检查有限性、PDF 和互易性。加性 Disney 模型及单次 GGX 不保证严格白炉能量守恒；该测试不将已知模型近似隐藏为通过标准。详见 [`Materials/DisneySurface.md`](Materials/DisneySurface.md)。

## 4. Renderer GPU 集成

`renderer_integration` 在真实设备和 RDG 上检查：

- 2 个求交后端 × 3 种 GI × 4 种阴影，共 24 个组合有可见场景，并执行正确的 atlas/VSM Pass。
- 单个点光的重复直接照明完全确定；PCF/PCSS/VSM 的实际显示图像两两不同，避免用随机噪声误判接入。
- 方向、点、矩形、圆盘、聚光分别执行关闭、红色、蓝色和位姿变化，对接收面的贡献与无灯控制比较；面光另与不变位姿的采样噪声比较。
- 真实导入场景在两种后端都可渲染；切换 base texture、normal map、normal strength 会改变像素。
- 导入闭合盒子的粗糙 BTDF 编辑，其图像变化大于同材质重复渲染的采样波动。
- 仅发光网格、没有解析灯的场景中，PT/ReSTIR/SSGI 在两个后端均能将红/绿发光贡献传到接收面；关闭 emission 后接收面变黑。该回归在旧代码中确认 ReSTIR/SSGI 为零，再修复为通过。
- 相机变化重置历史而不上传几何；可见性变化上传并重建 AS；材质/灯光变化按对应数据上传。
- 最终 overlay 在场景渲染后执行；连续 capture 恢复状态，交换链可 resize 到 800x600 和 320x240。
- Cornell 金属盒子以 16384 SPP 的 PT 图像为参考，并使用独立 4096 SPP 的 PT 控制组估计有限采样噪声；软件 BVH 与 DXR 的 ReSTIR 都检查前表面误差、底部异常亮点和后期收敛。
- 1100 三角形/属性的共享网格副本超过初始容量，两个求交后端均可实际绘制；撤销/重做后的确定性像素分别精确恢复原图与副本图。旧实现在此确认出现上传容量不足，再修复为通过。
- 背墙外、地板下、侧墙外光源 × 三种 GI × 两后端共 18 组接触遮挡案例，直接读回 `Resolve.Spatial` raw HDR；所有像素需非负且小于 `1e-8`。光源移入房间必须产生亮度，移回墙外首帧必须清除亮历史；GBuffer 精确位置还须保持在源几何范围内。旧代码确认复现接缝漏光与负 Y 接收点后才修复，见 [`ContactVisibility.md`](ContactVisibility.md)。

测试使用 160x120 的内部渲染尺寸。一般图像差异是 ACES 后经 sRGB 解码的显示线性 RGB 平均绝对差，证明参数进入实际着色，不是场景 HDR 误差或物理精度。接触遮挡测试单独读取浮点 HDR，不经过显示转换。SSGI 的 marcher 不使用场景射线，两种 backend selection 验证的是共同宿主与直接光可见性路径。

金属盒子回归使用固定内部尺寸 160x120、路径深度 4、SPP=16、关闭 Bloom；ReSTIR 每像素每帧生成 32 个新候选。前表面最终 MAE：PT 控制组 0.005030，ReSTIR 软件后端 0.005925，DXR 0.005129；底部最大通道误差的 P95 分别为 0.02189、0.03031、0.03242。ReSTIR 后期帧间差分别为 0.001355 和 0.001403，均通过与独立 PT 噪声及早期图像比较的断言。这些质量误差在 ACES 输出经 sRGB 解码后的显示线性域统计，不是 raw HDR 或无偏性证明。

发光网格回归中，PT、ReSTIR 与 SSGI 的接收面都出现对应的红/绿贡献，关闭 emission 后为零。SSGI 的屏幕空间结果明显偏低，符合其信息缺失限制；此案例不是三种估计器精度等价的证明。

## 5. 编辑器交互与导入

`editor_gizmo_interaction` 在隐藏的原生窗口内向生产 ImGui/ImGuizmo 代码注入鼠标、按键及文本事件，不移动操作系统光标。检查完整 Gizmo 拖动的一步撤销、Ctrl+Y/Shift+Z、保留旁观实体模拟位移与后续相机导航、Q/W 工具、RMB+W/E 飞行、F 取景、手柄上的 Alt 环绕、无活动相机时 Ctrl+D/Delete 的撤销/重做，以及场景替换时取消交互。实际 World 页 Name 输入激活后验证原生 Ctrl+Z 只撤销最后一次文字输入，结束编辑后全局 Ctrl+Z 才撤销完整属性事务。数学与表面拾取边界由 CPU 测试覆盖，操作及边界见 [`EditorControls.md`](EditorControls.md)。

`editor_import_integration` 使用真实 EngineEditor facade、ImGui 和 GPU，覆盖 Cornell 初始场景、首次导入、拒绝不支持资产后保留可绘制场景、再次导入以及最终 UI 截图。重复导入验证持久 descriptor 的回收和重建；字体与 viewport descriptor 在场景重载后仍可用。

本地批量输出位于 `build/captures/editor-import/`，仓库保留的 [`editor-after-reload.png`](../captures/editor-after-reload.png) 显示重载后的场景与编辑器。CPU 解析/转换失败在 GPU 场景销毁前报告；native 分配失败保持原始异常诊断，不宣称可以原子恢复。

## 6. 实际画面

本次 Cornell ReSTIR 修复截图：[`contact-fixed-restir.png`](../captures/contact-fixed-restir.png)；新版 UI：[`editor-ue-controls.png`](../captures/editor-ue-controls.png)。使用最终 Release 程序、软件 BVH、光追阴影、400x300、256 帧、SPP=16、validation 运行，进程退出码为 0。人工检查可见红绿墙、顶灯、两个盒子和 Gizmo 工具栏；没有选择实体时不绘制操作轴，UI 在后处理之后合成。保留原 PBR 材质，金属盒子底部的白色地板反射与侧面的绿墙反射仍存在，独立 PT 也能观察到；这些反射与接缝处真实遮挡错误分开验证，详见 [`ContactVisibility.md`](ContactVisibility.md)。有限采样噪声仍可见。

此 viewport 显示均值为 0.391539，非黑像素 100200/120000；UI 显示均值为 0.290827，非黑像素 966280/1076544（1344x801）。本次独立程序运行未输出 validation warning/error。

此前导入 fixture 的 PT/DXR/VSM 使用 400x300、32 帧、SPP=4 运行，进程退出码为 0；[`completion-import-vsm.png`](../captures/completion-import-vsm.png) 与 [`completion-import-editor.png`](../captures/completion-import-editor.png) 可见纹理地面和盒子。导入 viewport 均值为 0.0476246，非黑像素 8675/120000，背景无环境光为黑色。最新导入路径另外由本次 CPU 与 GPU 集成测试覆盖。

```powershell
.\build\Release\EncoreEngine.exe --gi restir --backend software --shadow rays `
    --frames 256 --spp 16 --width 400 --height 300 --headless --validation `
    --capture captures/contact-fixed-restir.png --capture-ui captures/editor-ue-controls.png

.\build\Release\EncoreEngine.exe --scene tests/fixtures/textured_multilight.gltf `
    --gi pathtracing --backend dxr --shadow vsm --frames 32 --spp 4 `
    --width 400 --height 300 --headless --validation `
    --capture captures/completion-import-vsm.png
```

Capture 的 mean 和 nonblack 是显示空间统计，用于发现空输出；非黑阈值是显示亮度大于 0.01。不能据此声称照度正确、后端逐像素相同或实时性能达标。

## 7. 验证边界

尚未完成固定射线集的 DXR/软件 BVH 命中逐项对照、全部材质/灯光的 raw HDR 高样本参考、显存峰值与逐 Pass 长期基准、长时间动态场景测试，以及 alpha MASK 在所有管线/atlas 边缘的固定像素对照。GPU 时间戳本身已通过原生负载验证。没有在本机启动 Blender 生成资产，实际 Blender 导出流程按 [`BlenderImport.md`](BlenderImport.md) 的格式与限制执行。

PCF/PCSS/VSM 有有限分辨率和投影覆盖，VSM 仍可能漏光；PT 选择这些滤波模式也会产生有偏可见性，`--shadow rays` 用于求交参考。玻璃阴影为二值遮挡，没有彩色透射阴影。Disney 提供完整 2012 表面参数和 2015 透射/薄表面扩展，subsurface 是局部近似而不是空间 BSSRDF。任意 Blender 节点网络不能直接等价导入。

ReSTIR 的 secondary Lo 为漫反射近似；粗糙度小于 0.35、含 clearcoat、含透射或薄表面的 primary 使用完整相机路径回退，因此不宣称任意光滑材质都已实现 ReSTIR 路径重用。ReSTIR Beauty 使用可重置的静态 HDR 均值，reservoir M 不代表独立样本数；SSGI 仍受屏幕信息限制。当前没有 Lumen、Nanite、NRD、完整 TAA、动画/蒙皮资产或通用嵌套折射介质求解。

资源由 World 显式拥有，类型化句柄不保活；未加入引用计数、GC 或自动卸载。当前 MeshResource 保存烘焙的多 surface 场景网格；编辑器副本共享资源并在 CPU 渲染提取时展开，没有 GPU 硬件实例化、源模型导入去重或几何编辑后的自动 GPU 刷新，详见 [`Resources.md`](Resources.md)。

最终测试输出与日志位于 `build/Testing/Temporary/LastTest.log`；架构、阴影和 ReSTIR 原理分别见 [`Architecture.md`](Architecture.md)、[`Shadows.md`](Shadows.md) 与 [`ReSTIR-GI-Learning.md`](ReSTIR-GI-Learning.md)。
