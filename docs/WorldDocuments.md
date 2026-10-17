# 世界文档

世界作者数据由 [`Schemas/World.proto`](../Schemas/World.proto) 定义，使用官方 Protobuf C++ 消息、二进制编码器和 `TextFormat` 解析器。文本和二进制采用同一结构、引用规则及验证入口。

## 层级与所有权

世界拥有多个关卡，关卡拥有多个场景，场景拥有对象。每个层级使用稳定字符串标识；对象标识不等于运行时 EnTT 句柄，场景切换后可以重新创建实体而保留作者身份。

材质、图像与网格保存在世界共享资源目录。对象通过 `mesh_id`、`material_id` 和 `texture_bindings.texture_id` 建立关联，场景没有材质或纹理所有权列表。渲染用 `render::Scene` 只是烘焙后的传输表，不是作者数据所有者。

同一基础材质可被多个对象共享。对象需要不同纹理或独立参数时，可以提供 `texture_bindings` 和 `material_override`；烘焙器生成独立的 GPU 材质槽。覆盖参数按字段继承基础材质。对象材质变体也能通过 `material_script_source`、`material_script_path` 保存独立 Surface 源码。

完整模型仍只占一个 Object。`surface_materials` 按相对对象范围的 `first_triangle` 和 `triangle_count` 连续覆盖模型，各段独立引用共享材质并保存纹理、参数和脚本覆盖。`use_surface_materials` 默认开启；关闭后使用对象顶层材质覆盖整个模型，原始表面绑定继续保存以便恢复。未写入表面列表的旧单材质文档保持原有行为。

## 发布示例

[`default.textproto`](../Content/Worlds/default.textproto) 包含 Cornell 房间和材质实验室两个场景；[`material-lab.textproto`](../Content/Worlds/material-lab.textproto) 是独立可打开的第二份世界文档。Cornell 的几何尺寸、绕序、盒体姿态、材质、摄像机和灯光均在配置中定义。

网格支持通用四边形、以底面中心为原点的盒体，以及导入模型的嵌入三角形载荷。嵌入载荷保留平滑法线、UV、切线和源范围，读取时不需要原始 glTF 文件。`source_path` 只记录来源，不使 RenderCore 反向依赖 AssetImport。

[`basic.textproto`](../Content/Objects/basic.textproto) 使用同一协议提供常用物体模板。宿主打开世界或导入模型后安装库资源，模板对象本身不进入活动场景；用户拖放后才生成当前 Scene 下的普通 Object。导出保留共享网格/材质及稳定身份，创建的对象不依赖运行时预设索引，详见 [`ObjectPlacement.md`](ObjectPlacement.md)。

## 读写接口

```cpp
auto document = render::LoadWorldDocument(path);
world.CreateDocument(document, "cornell");
world.SwitchScene("material-lab");
render::SaveWorldDocument(world.ExportDocument(), destination);
```

`.pb` 使用官方二进制编码；其他后缀使用 UTF-8 Protobuf 文本，文本读取接受 UTF-8 BOM。默认资源路径优先选择可执行文件旁的 `Content/Worlds/default.textproto`，开发环境回退到构建系统注入的资源目录。

`CreateDocument` 先在临时 World 中构建全部资源和 ECS 组件，成功后才替换当前世界。`SwitchScene` 先导出当前编辑，再加载目标场景，因此不丢失未保存的变换、可见性、材质关联、灯光、摄像机、速度或 Lua 源码。旧导入入口 `CreateScene`、`CreateCornell` 的世界也可导出为包含嵌入几何的完整文档。

`AppendScene` 将已解析的模型追加到活动场景，为输入资源和对象分配独立命名空间并重定位表面、纹理与发光体引用。重复导入保留为独立实例，已有场景、相机和编辑结果继续保留；失败不会部分追加对象或资源。编辑器“导入模型”调用该路径，打开世界文档仍替换当前世界。

`SaveWorldDocument` 先验证，再写入同目录临时文件并原子替换目标。验证或写入失败不会交付半个正式文档。编辑器在替换 World 后应清理选择与历史，并调用 `EditorDefaults::Capture` 建立新的导入基线。

## 验证与边界

- 世界文档最多六十四 MiB，单份 Lua 或 HLSL 源码最多一 MiB。
- 所有已写入浮点分量必须有限；变换不可使用零缩放，变换后坐标也必须可表示。
- 关卡、场景、对象及各资源目录中的标识不可为空或重复；对象资源引用、网格范围和纹理采样槽必须有效。
- 模型表面范围必须无空洞、无重叠地覆盖完整对象，所有表面材质与纹理引用均需有效。
- 图像载荷必须与宽高完全对应；纹理槽为零到七，U/V 包裹方式支持重复、镜像和夹取。
- 摄像机必须满足有效视场角及近远平面关系；光源必须使用有效能量、尺寸和正交发射坐标系，每场景最多十六个启用光源。
- 路径和字符串必须是有效 UTF-8，不能含空字符；源路径不能含 Windows 文件名禁止字符。

面光源可通过 `emitter_object_id` 指向同场景网格对象，每个发光体只能归属一个面光源。配置场景会隐藏该对象的稳定源三角形槽，由解析光源按自身姿态与尺寸生成唯一可见发光几何，从而保持光源采样和 MIS 标签一致。旧 `BuildCornellScene` 入口保留历史渲染兼容约定，几何与材质数据仍来自文档。

`world_document_tests` 覆盖官方文本和二进制往返、失败保存、引用及数值校验、共享材质的对象纹理隔离、相同像素资源身份、脚本保存、旧导入提升、场景切换和发光体唯一性，并验证完整模型表面、整体覆盖后恢复、删除原实例后保存副本、追加与重复导入，以及非法表面范围的拒绝。
