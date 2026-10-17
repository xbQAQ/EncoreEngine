# Blender 静态场景导入

引擎读取 FBX 和 glTF 2.0 的 `.gltf`、`.glb`，不直接读取 `.blend`。FBX 使用固定版本 ufbx，骨骼和形态键只烘焙默认静态姿态，完整规则见 [FBX 导入](FbxImport.md)。本页其余部分描述 cgltf v1.15 的 glTF 路径；PNG/JPEG 解码共用 Windows WIC。导入结果完全拥有几何、材质、图像和灯光数据，不依赖 Blender 进程，也不保留解析器内存指针。

## Blender 导出

1. 在 Blender 中保存 `.blend` 源文件。对动画、骨骼和形态键，先制作需要的静态副本，并应用形变；实现实例应先转为实际网格。
2. 使用 Principled BSDF 材质。通过 Image Texture 和 UV Map 提供纹理；程序化节点、混合节点和非 Principled 着色网络应先烘焙为 PNG/JPEG 贴图。
3. 使用第一个 UV 集。确保需要纹理的网格具有 UV；可导出平滑法线和切线。未提供法线时使用几何法线，未提供切线时根据 UV 计算切线。
4. 选择 `File > Export > glTF 2.0`。推荐选择 `glTF Binary (.glb)`，或保持 `.gltf`、`.bin` 和贴图的相对位置。导出静态网格、法线、UV、材质，以及需要的 punctual lights。
5. 关闭 Draco、Meshopt、动画、蒙皮和 morph target 导出。纹理使用 PNG 或 JPEG；不要使用 KTX2/BasisU 或 WebP 作为唯一图像来源。
6. 玻璃使用 `OPAQUE` 覆盖模式并导出 `KHR_materials_transmission`。裁切使用 `MASK`。`BLEND` 覆盖混合会明确拒绝，因为它需要额外的覆盖/排序渲染流程；它与几何透射不是同一功能。

glTF 为右手、Y-up。导入器消费导出器写入的坐标约定，不再额外交换坐标轴。节点的矩阵或 TRS 层级会烘焙到世界位置；法线使用逆转置，镜像变换会反转三角形绕序并修正切线手性。

## 在引擎中打开

编辑器的“导入模型”入口可以选择 `.fbx`、`.gltf` 或 `.glb`，每次导入追加到当前场景，保留已有模型、相机和编辑结果。每个文件生成一个以文件名命名的完整 ECS 模型对象，可整体选择、移动、旋转、缩放、复制、隐藏和删除。内部 mesh primitives 保留各自的逐三角形材质和纹理，不再平铺为独立模型；随后 GPU BVH 的排序不改变属性索引。

详情中的“使用模型表面材质”默认开启，“材质”列表选择当前要编辑的原始表面材质槽。关闭该选项或从内容浏览器应用材质，可覆盖整个模型；原始表面绑定仍会保存，重新开启后恢复。世界文档将表面范围、材质、贴图、脚本和覆盖状态保存在模型对象内，保存重开后保持完整模型结构。旧单材质世界无需迁移。

命令行示例：

```powershell
.\EncoreEngine.exe --scene .\tests\fixtures\textured_multilight.gltf
```

命令行 `--scene` 直接打开模型作为初始世界，按资产包围盒建立默认相机，没有源灯光时创建默认矩形灯。编辑器内后续导入保留当前相机，不重复创建默认灯光；实际携带的 glTF 灯光会一起追加。失败的导入保留当前世界并报告具体条件，用户可以修正导出选项后重试。文件内部节点层级仍烘焙为静态几何，本次模型分组不增加独立骨骼或子节点编辑。

## 支持范围

| 数据 | 行为 |
|---|---|
| 容器与缓冲 | glTF 2.0 JSON、GLB、外部相对缓冲 URI、base64 data URI；Unicode 文件路径 |
| 几何 | 索引或无索引 triangle list；节点层级、矩阵/TRS、实例节点的世界空间副本 |
| 顶点属性 | POSITION、NORMAL、TEXCOORD_0、TANGENT；cgltf 浮点解包支持稀疏属性和归一化整数属性 |
| 图像 | 外部 URI、base64 data URI、GLB bufferView 中的 PNG/JPEG；RGBA8 自有像素 |
| 采样 | Repeat、Clamp、Mirrored Repeat；KHR_texture_transform 的平移、缩放、旋转与 UV0 |
| 基础材质 | 线性 baseColor 因子、metallic/roughness、emission、法线强度、double-sided、OPAQUE/MASK |
| 扩展因子 | clearcoat、transmission、IOR、specular、sheen、anisotropy、emissive strength、volume attenuation、无色 diffuse transmission |
| 扩展纹理 | transmission R；clearcoat R/roughness G；sheen RGB/roughness A；specular RGB/factor A |
| 灯光 | KHR_lights_punctual 的 directional、point、spot；名称、继承位姿、线性色彩、强度、range 和锥角；最多 16 个启用灯光 |

位置流保持 48 字节：`a.w` 是材质索引，`b.w` 是属性索引加一，零使用几何回退；`c.w` 是运行时解析灯索引加一，普通导入网格为零。96 字节属性记录保存三个法线、三个 UV 和世界切线。RGBA8 像素不在 CPU 上统一做 Gamma 转换：RGB 颜色贴图与线性数据贴图由材质采样分别解释，并在双线性插值前解码颜色 texel。

## 材质映射边界

运行时使用 Disney 2012 参数化，不宣称与 Blender 任意节点网络或 glTF 每个扩展的 BRDF 完全相同。

- clearcoat roughness 映射为 `1 - roughness` 的 clearcoat gloss；glTF specular factor 和颜色亮度映射为 `0.5 * factor * luminance(colorFactor)` 的 Disney specular level。
- glTF specular/sheen 的 RGB 因子投影到运行时的标量强度和 tint。任意彩色 Fresnel 或独立 sheen 色相不保证精确还原；Disney 2012 sheen 使用固定形状，因此独立 sheen roughness 因子不改变该 lobe。
- 独立扩展纹理会打包为上述 RG/RGBA 通道。它们必须使用相同 UV 变换和 wrap 模式；分辨率不同时按最大宽高进行最近邻重采样。图片源像素保持 RGBA8，不保证该重采样与原始多纹理过滤完全等价。
- 核心 occlusion 贴图不注入照明结果：本引擎通过场景可见性和 GI 计算遮蔽。采样使用运行时统一过滤策略，不导入 glTF 的 min/mag filter 或预生成 mip 链。
- 体积扩展提供薄表面标识、衰减颜色和距离，不是完整的对象体积网格/厚度贴图积分器。

明确拒绝的输入包括：不支持的 required extension、Draco/Meshopt、蒙皮、动画、morph targets、EXT_mesh_gpu_instancing、非 triangle-list 拓扑、稀疏索引、顶点颜色、UV1 纹理、材质 variants、specular-glossiness、unlit、iridescence、dispersion、alpha BLEND、anisotropy 方向纹理、独立 clearcoat normal、volume thickness 纹理，以及有色或贴图 diffuse transmission。错误信息会给出烘焙、静态化或导出替代方案。单张图像每边最多 16384 像素；单文件最多 4 GiB；材质 IOR 支持 1 到 4。

Blender area light 不是 KHR_lights_punctual 类型，不会通过该标准导入为矩形或圆盘灯。导入后可在引擎中创建矩形/圆盘 ECS 灯；其解析采样与可见发光网格使用相同坐标框架。

## 验证资产

`tests/fixtures/textured_multilight.gltf` 是测试程序生成的自包含资产，不声称由本机运行 Blender 导出。它包含带贴图地面、闭合盒子、两个 mesh primitives、clearcoat 材质和三个 punctual lights。`unsupported_required.gltf` 用于验证不支持的 required extension 会失败。

```powershell
cmake --build build --config Release --target asset_import_tests gameplay_tests scene_extraction_tests
ctest --test-dir build -C Release --output-on-failure -R "asset_import|gameplay_ecs|scene_extraction"
.\build\Release\asset_import_tests.exe --write-fixtures .\tests\fixtures
```

导入测试覆盖节点层级、逆转置、镜像绕序、无索引默认属性、稀疏 POSITION、GLB 内嵌图像、data URI、真实 PNG 像素、纹理变换、扩展通道打包、父级缩放下的灯光框架与错误路径。ECS/提取测试另外覆盖材质编辑快照、五种灯光类型、生成面光网格、灯光数量边界及无效数据拒绝。
