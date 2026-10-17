#include "gameplay/WorldReflection.h"
#include <cmath>

namespace gameplay {
namespace {
using engine::reflection::TypeKind;
void Require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
void Finite(float value) { Require(std::isfinite(value), "组件包含非有限数值"); }
void Finite(const Vec3& value) { Require(render::IsFinite(value), "组件向量包含非有限数值"); }
}

WorldReflection::WorldReflection() {
    // 数学值、全局数学函数和强类型枚举由同一套注册接口描述。
    types_.Type<Vec3>("Vec3").Constructor<>().Constructor<float, float, float>()
        .Field<&Vec3::x>("x", "水平坐标或红色分量").Field<&Vec3::y>("y", "垂直坐标或绿色分量")
        .Field<&Vec3::z>("z", "深度坐标或蓝色分量");
    types_.Function<&render::Dot>("Dot", "两个向量的点积");
    types_.Function<&render::Cross>("Cross", "两个向量的叉积");
    types_.Function<&render::Length>("Length", "向量长度");
    types_.Function<&render::Normalize>("Normalize", "安全归一化向量");
    types_.Function<&render::IsFinite>("IsFinite", "检查向量是否有限");
    types_.Type<Entity>("Entity", TypeKind::Enumeration).Value<static_cast<Entity>(entt::null)>("Null");
    types_.Type<LightType>("LightType", TypeKind::Enumeration)
        .Value<LightType::Directional>("Directional", "平行光").Value<LightType::Point>("Point", "点光")
        .Value<LightType::Rectangle>("Rectangle", "矩形面光").Value<LightType::Disk>("Disk", "圆盘面光")
        .Value<LightType::Spot>("Spot", "聚光灯");
    types_.Type<Name>("Name").Constructor<>().Field<&Name::value>("value", "实体显示名称");
    types_.Type<Transform>("Transform").Constructor<>()
        .Field<&Transform::position>("position", "世界位置，单位为米")
        .Field<&Transform::rotationDegrees>("rotationDegrees", "欧拉角，单位为度")
        .Field<&Transform::scale>("scale", "局部轴缩放");
    types_.Type<Camera>("Camera").Constructor<>()
        .Field<&Camera::verticalFovDegrees>("verticalFovDegrees", "垂直视场角")
        .Field<&Camera::nearPlane>("nearPlane", "近裁剪面")
        .Field<&Camera::farPlane>("farPlane", "远裁剪面").Field<&Camera::active>("active", "是否参与视图选择");
    types_.Type<Velocity>("Velocity").Constructor<>().Field<&Velocity::linear>("linear", "世界空间线速度");
    types_.Type<Light>("Light").Constructor<>()
        .Field<&Light::type>("type").Field<&Light::color>("color").Field<&Light::intensity>("intensity")
        .Field<&Light::width>("width").Field<&Light::height>("height").Field<&Light::range>("range")
        .Field<&Light::enabled>("enabled").Field<&Light::sourceRadius>("sourceRadius")
        .Field<&Light::outerAngleDegrees>("outerAngleDegrees").Field<&Light::innerAngleDegrees>("innerAngleDegrees")
        .Field<&Light::direction>("direction").Field<&Light::tangent>("tangent").Field<&Light::bitangent>("bitangent")
        .ReadOnly<&Light::emitterMaterial>("emitterMaterial", "由作者资源配置决定的发光材质槽");
    types_.Type<LuaScript>("LuaScript").Constructor<>()
        .ReadOnly<&LuaScript::source>("source", "编辑器管理的源代码")
        .ReadOnly<&LuaScript::path>("path", "编辑器管理的来源路径")
        .Field<&LuaScript::enabled>("enabled", "脚本是否启用");
    types_.Type<ObjectIdentity>("ObjectIdentity")
        .ReadOnly<&ObjectIdentity::levelId>("levelId").ReadOnly<&ObjectIdentity::sceneId>("sceneId")
        .ReadOnly<&ObjectIdentity::objectId>("objectId");
    types_.Type<engine::ResourceHandle<render::MeshResource>>("MeshHandle")
        .ReadOnly<&engine::ResourceHandle<render::MeshResource>::value>("value", "非拥有型网格资源标识");
    types_.Type<MeshRenderer>("MeshRenderer").Constructor<>()
        .ReadOnly<&MeshRenderer::firstTriangle>("firstTriangle").ReadOnly<&MeshRenderer::triangleCount>("triangleCount")
        .Field<&MeshRenderer::materialIndex>("materialIndex").Field<&MeshRenderer::visible>("visible")
        // 脚本可显式选择保留各表面材质或应用统一材质覆盖。
        .Field<&MeshRenderer::useSurfaceMaterials>("useSurfaceMaterials")
        .ReadOnly<&MeshRenderer::lightOwned>("lightOwned").ReadOnly<&MeshRenderer::meshResource>("meshResource");
    // 非拥有型 World 服务不开放构造及结构变更方法，元数据仍可完整描述其查询接口。
    types_.Type<World>("World", TypeKind::Class, "拥有实体、组件和资源的世界")
        .Property<nullptr, &World::Revision>("revision", "世界版本")
        .Property<nullptr, &World::ActiveSceneId>("activeSceneId", "活动场景标识")
        .Property<nullptr, &World::ActiveLevelId>("activeLevelId", "活动关卡标识")
        .Method<&World::CameraEntity>("CameraEntity", "获取主相机实体")
        .Method<&World::LightEntity>("LightEntity", "获取主光源实体");

    // 提交前复核完整快照，同时防止通过新建快照绕过只读字段。
    RegisterComponent<Name>("Name", [](const Name& value, const Name&, const World&) {
        Require(value.value.size() <= 4096 && value.value.find('\0') == std::string::npos, "实体名称过长或包含空字符");
    });
    RegisterComponent<Transform>("Transform", [](const Transform& value, const Transform&, const World&) {
        Finite(value.position); Finite(value.rotationDegrees); Finite(value.scale);
        Require(std::abs(value.scale.x) >= 0.0001f && std::abs(value.scale.y) >= 0.0001f &&
            std::abs(value.scale.z) >= 0.0001f, "组件缩放不能接近零");
    });
    RegisterComponent<Velocity>("Velocity", [](const Velocity& value, const Velocity&, const World&) { Finite(value.linear); });
    RegisterComponent<Camera>("Camera", [](const Camera& value, const Camera&, const World&) {
        Finite(value.verticalFovDegrees); Finite(value.nearPlane); Finite(value.farPlane);
        Require(value.verticalFovDegrees > 0 && value.verticalFovDegrees < 180 && value.nearPlane > 0 &&
            value.farPlane > value.nearPlane, "相机视场角或裁剪面范围无效");
    });
    RegisterComponent<Light>("Light", [](const Light& value, const Light& old, const World&) {
        Finite(value.color); Finite(value.direction); Finite(value.tangent); Finite(value.bitangent);
        for (const float number : {value.intensity, value.width, value.height, value.range, value.sourceRadius,
                                  value.outerAngleDegrees, value.innerAngleDegrees}) Finite(number);
        Require(static_cast<unsigned>(value.type) <= static_cast<unsigned>(LightType::Spot), "未知光源类型");
        Require(value.color.x >= 0 && value.color.y >= 0 && value.color.z >= 0 && value.intensity >= 0 &&
            value.width > 0 && value.height > 0 && value.range > 0 && value.sourceRadius >= 0,
            "光源颜色、强度、尺寸或影响范围无效");
        Require(value.outerAngleDegrees > 0 && value.outerAngleDegrees < 90 && value.innerAngleDegrees >= 0 &&
            value.innerAngleDegrees <= value.outerAngleDegrees, "聚光灯锥角范围无效");
        Require(value.emitterMaterial == old.emitterMaterial, "脚本不能替换作者配置的发光资源身份");
    });
    RegisterComponent<LuaScript>("LuaScript", [](const LuaScript& value, const LuaScript& old, const World&) {
        Require(value.source == old.source && value.path == old.path, "脚本源码和来源路径只能在编辑器中修改");
    });
    RegisterComponent<ObjectIdentity>("ObjectIdentity", {}, false);
    RegisterComponent<MeshRenderer>("MeshRenderer", [](const MeshRenderer& value, const MeshRenderer& old, const World& world) {
        Require(value.firstTriangle == old.firstTriangle && value.triangleCount == old.triangleCount &&
            value.meshResource == old.meshResource && value.lightOwned == old.lightOwned, "脚本不能修改不可变网格身份和范围");
        Require(value.materialIndex < world.Materials().size(), "网格材质槽超出世界资源范围");
    });
}

const ComponentBinding& WorldReflection::Component(std::string_view name) const {
    const auto type = types_.Resolve(name);
    if (!type) throw std::invalid_argument("未知组件类型：" + std::string(name));
    return Component(type.id());
}
const ComponentBinding& WorldReflection::Component(entt::id_type id) const {
    const auto found = components_.find(id);
    if (found == components_.end()) throw std::invalid_argument("该反射类型没有注册为世界组件");
    return found->second;
}

entt::meta_any WorldReflection::Read(const World& world, Entity entity, std::string_view name, const ComponentWrites& writes) const {
    const auto& binding = Component(name);
    if (!world.Registry().valid(entity) || !binding.has(world, entity)) throw std::invalid_argument("实体无效或缺少组件");
    const auto found = writes.find({entity, binding.type.id()});
    return found == writes.end() ? binding.read(world, entity) : binding.copy(found->second);
}
void WorldReflection::Stage(const World& world, Entity entity, std::string_view name, const entt::meta_any& value, ComponentWrites& writes) const {
    const auto& binding = Component(name);
    if (!binding.writable) throw std::invalid_argument("该世界组件只读");
    binding.validate(world, entity, value);
    writes.insert_or_assign({entity, binding.type.id()}, binding.copy(value));
}
void WorldReflection::Commit(World& world, ComponentWrites& writes) const {
    // 所有可能失败的校验先执行，之后每个组件交换都满足无异常提交约束。
    for (const auto& [key, value] : writes) Component(key.second).validate(world, key.first, value);
    bool changed = false;
    for (auto& [key, value] : writes) changed |= Component(key.second).commit(world, key.first, value);
    writes.clear();
    if (changed) world.MarkDirty();
}
}
