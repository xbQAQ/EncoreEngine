# CPU 资源对象

引擎采用类似 Godot 的资源类型层级：`TextureResource`、`MeshResource` 和 `MaterialResource` 都继承 `engine::Resource`。当前只确定资源的数据职责和显式所有者；不引入 `RefCounted`、引用计数、垃圾回收、自动卸载、异步加载或热重载。

## 类型与所有权

| 类型 | 职责 |
| --- | --- |
| `Resource` | 提供进程内 ID、名称、具体类型查询和虚析构；不关联 ECS 实体生命周期。 |
| `ResourceHandle<T>` | 非拥有的类型化 ID；复制句柄不延长资源寿命。 |
| `ResourceStore` | 使用 `unique_ptr<Resource>` 明确拥有资源；负责创建、类型校验查询、显式删除和清空。 |
| `TextureResource` | 拥有一张解码后的 RGBA8 图像及尺寸；同一图像的多个使用位置共享资源。 |
| `MeshResource` | 拥有一个烘焙后的多 surface 网格，包括全部三角形、顶点属性和命名 surface 范围。 |
| `MaterialResource` | 拥有唯一可编辑的 `GpuMaterial` 参数，通过当前纹理槽查询图像句柄和 sampling view。 |

`World` 是当前场景 CPU 资源的明确所有者。删除一个 ECS 实体不会自动删除它引用的资源。替换场景或销毁 `World` 时，其所有资源随所有者销毁；这只是当前容器的生命周期，不代表已经选择了未来的内存回收策略。

资源 ID 在进程内递增，不是持久化资产 GUID。句柄必须交给对应 `ResourceStore` 查询；错误类型、其他所有者以及已清空或删除的句柄均返回空指针。移动 `World` 会转移所有权，原资源 ID 与对象地址保持有效。

## 导入到渲染

1. `ImportGltf` 和 `BuildCornellScene` 仍输出拥有完整数据的 `Scene` 传输对象。
2. `World::CreateScene` 校验输入，创建资源并建立 ECS。一个烘焙后的 `MeshResource` 可以包含多个命名 surface；`MeshRenderer` 通过类型化句柄引用它，并继续保存稳定的三角形范围。
3. `World::CookResourceScene` 在场景安装边界将资源转换为 Renderer 的参考 `Scene`。三角形顺序和 attribute ID 保持不变；图像重新打包为 GPU texel 数组，各 sampling view 的 sampler、UV transform 和材质索引保持对应。
4. 每帧 `ExtractRenderSnapshot` 只复制组件和当前材质参数。Renderer 从不可变参考几何和快照构建绘制数据；共享网格实例在这里展开为独立的三角形、属性及拾取归属，继续使用现有 GPU ABI 与 BVH。

几何和图像目前只提供只读访问，不存在资源编辑后自动刷新 GPU 的机制。重新安装场景时才重新烘焙这些数据，因此不需要每帧复制完整几何或图像。

## 共享网格实例

编辑器 `Ctrl+D` 复制网格实体的组件及非拥有 `MeshResource` 句柄，副本与原实体共享不可变源三角形、属性和 surface 范围。复制不会创建新的 MeshResource、图像或材质资源；实体 Transform、可见性和材质槽引用可以分别编辑。材质参数仍由对应 MaterialResource 共享，修改同一材质会影响所有引用它的实体。

`SceneExtraction` 允许不同非空实体以相同非空资源句柄共享完全相同的源范围。第一个实例保留源 primitive 槽位，后续实例追加独立变换的三角形；副本的逐三角形属性另行追加并更新 `b.w`，避免后一个实例覆盖前一个实例的法线、UV 或切线。镜像变换同步调整绕序、UV/法线顶点关联和切线手性。部分范围重叠、不同资源身份占用同一范围和重复实体身份仍被拒绝；没有实例占用的源范围保持为退化三角形。

可选 `triangleOwners` 向量与未排序的提取三角形一一对应，使副本和生成发光面能够拾取到准确的实体。CPU 所有权映射不进入 shader ABI；BVH 可以重排渲染副本，源 MeshResource 和参考 Scene 保持不变。这是 CPU 展开后的渲染实例支持，没有增加 GPU 硬件实例化或 AS refit。

撤销/重做保存组件和材质/设置变化值，其中的网格引用仍为非拥有句柄。删除副本、撤销复制或从历史中淘汰操作，都不自动卸载共享资源。成功替换场景会清空编辑历史，避免旧场景的资源引用跨过所有者生命周期；操作说明见 [`EditorControls.md`](EditorControls.md)。

## GPU 容量与寿命

SceneGpu 的初始三角形容量包含源数据和最多十六个圆盘灯的几何余量，属性缓冲区按参考数据建立。共享实例或发光面使计数超出容量时，在提交新提取结果及开始本帧命令录制之前扩容。三角形/BVH 与属性容量分别按需倍增；三角形容量增长时同步重建相应 BLAS、TLAS、scratch 和 instance 分配。扩容会等待 GPU 空闲，TLAS 继续使用保留的描述符槽位，其他 SRV 的增长受扩容次数限制。

撤销或删除只减少活动几何数量，不缩小已分配容量，后续重做可以复用它。ResourcePool 继续拥有旧缓冲区、加速结构和上传暂存，直到场景替换时 Reset；倍增避免每次添加实体都分配新容量，但扩容阶段和保留的旧分配仍会增加显存及上传内存使用。当前没有增加资源池回收器或新的 CPU/GPU GC、自动卸载政策。

计数在分配前检查：GPU 三角形容量必须能容纳 `uint32` 的三倍顶点计数和 BVH 索引，属性的一基 float 索引最多为可逐整数精确表示的 `2^24`。这些是数据表示边界，不是设备显存足够的保证；实际 GPU 覆盖和运行结果见 [`Validation.md`](Validation.md)。

## 材质编辑兼容

`World::Materials()` 返回参数投影视图，支持 `operator[]`、`size()`、`empty()` 和 range-for。它直接引用 `MaterialResource::Parameters()`，不再维护另一份可能失去同步的可编辑参数数组。它不是连续存储的 `std::vector<GpuMaterial>`，不提供 `data()` 或结构性增删接口。

修改材质参数后仍需调用 `World::MarkDirty()`，让依赖历史数据的渲染器重置累积。快照中的材质是值拷贝，旧快照不会随之后的编辑而改变。

图像与 sampling view 分开：`TextureResource` 拥有像素，`TextureBinding` 保存指向图像的句柄和每次使用的 `GpuTextureView`。材质按当前 `Parameters().textures/textures2` 查询 binding，因此共享图像不会丢失各自的 wrap、UV offset、scale 或 rotation。
