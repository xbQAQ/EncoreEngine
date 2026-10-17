#include "editor/EditorObjectPalette.h"
#include "gameplay/World.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace render::editor {
namespace {
Vec3 Vertex(const Float4& vertex) { return {vertex.x, vertex.y, vertex.z}; }

gameplay::Transform PresetTransform(const config::Object& object) {
    gameplay::Transform result;
    if (!object.has_transform()) return result;
    const auto& transform = object.transform();
    result.position = ReadVector(transform.position());
    result.rotationDegrees = ReadVector(transform.rotation_degrees());
    // 配置缺失的缩放分量使用单位值，与世界文档实例化边界一致。
    const auto& scale = transform.scale();
    result.scale = {scale.has_x() ? scale.x() : 1, scale.has_y() ? scale.y() : 1, scale.has_z() ? scale.z() : 1};
    return result;
}

Aabb PresetBounds(const Scene& source, const gameplay::ObjectPreset& preset) {
    Aabb bounds;
    const std::size_t first = preset.firstTriangle, count = preset.triangleCount;
    if (!count || first > source.triangles.size() || count > source.triangles.size() - first) return bounds;
    // 预设范围只读取不可变源几何，避免放置预览生成临时 ECS 或渲染副本。
    for (std::size_t index = first; index < first + count; ++index)
        for (const auto& vertex : {source.triangles[index].a, source.triangles[index].b, source.triangles[index].c})
            bounds.Expand(Vertex(vertex));
    return bounds;
}

Vec3 TransformPoint(const gameplay::Transform& transform, Vec3 point) {
    const auto basis = MakeCameraBasis(transform.rotationDegrees.x, transform.rotationDegrees.y, transform.rotationDegrees.z);
    return transform.position + basis.right * (point.x * transform.scale.x) +
        basis.up * (point.y * transform.scale.y) + basis.forward * (point.z * transform.scale.z);
}

std::array<Vec3, 8> BoundCorners(const Aabb& bounds) {
    return {{{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z}, {bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
        {bounds.maximum.x, bounds.maximum.y, bounds.minimum.z}, {bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
        {bounds.minimum.x, bounds.minimum.y, bounds.maximum.z}, {bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
        {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}, {bounds.minimum.x, bounds.maximum.y, bounds.maximum.z}}};
}

std::optional<ImVec2> Project(Vec3 point, const ViewportRect& image, const gameplay::RenderCamera& camera) {
    const auto basis = MakeCameraBasis(camera.transform.rotationDegrees.x, camera.transform.rotationDegrees.y, camera.transform.rotationDegrees.z);
    const auto relative = point - camera.transform.position;
    const float depth = Dot(relative, basis.forward);
    if (depth < camera.camera.nearPlane || depth > camera.camera.farPlane) return {};
    const float tangent = std::tan(camera.camera.verticalFovDegrees * (3.14159265358979323846f / 360));
    return ImVec2{image.x + (1 + Dot(relative, basis.right) / (depth * tangent * image.width / image.height)) * image.width * .5f,
        image.y + (1 - Dot(relative, basis.up) / (depth * tangent)) * image.height * .5f};
}

bool Matches(const config::Object& object, const char* search) {
    std::string text = object.name() + " " + object.id();
    std::string query(search);
    // UTF-8 中文按原字节匹配，只有 ASCII 拉丁名称需要忽略大小写。
    const auto lower = [](unsigned char value) { return static_cast<char>(value < 128 ? std::tolower(value) : value); };
    std::transform(text.begin(), text.end(), text.begin(), lower);
    std::transform(query.begin(), query.end(), query.begin(), lower);
    return text.find(query) != std::string::npos;
}

void PresetIcon(const config::Object& object, ImVec2 center, ImU32 color) {
    auto* draw = ImGui::GetWindowDrawList();
    // 原生 DrawList 图标与既有变换工具栏一致，不依赖额外字体或外部贴图。
    if (object.has_light()) {
        const auto type = object.light().type();
        if (type == config::Light::RECTANGLE) draw->AddRect({center.x - 7, center.y - 5}, {center.x + 7, center.y + 5}, color, 0, 0, 1.5f);
        else if (type == config::Light::DISK) draw->AddEllipse(center, {8, 5}, color, 0, 20, 1.5f);
        else if (type == config::Light::SPOT) {
            draw->AddTriangle({center.x, center.y - 7}, {center.x - 7, center.y + 6}, {center.x + 7, center.y + 6}, color, 1.5f);
        } else {
            draw->AddCircle(center, 4, color, 16, 1.5f);
            for (int ray = 0; ray < 8; ++ray) {
                const float angle = ray * (3.14159265358979323846f / 4);
                const ImVec2 axis{std::cos(angle), std::sin(angle)};
                draw->AddLine({center.x + axis.x * 6, center.y + axis.y * 6},
                    {center.x + axis.x * 9, center.y + axis.y * 9}, color, 1);
            }
        }
    } else if (object.id().find("plane") != std::string::npos) {
        draw->AddQuad({center.x - 8, center.y + 2}, {center.x + 1, center.y - 5},
            {center.x + 8, center.y - 1}, {center.x - 1, center.y + 6}, color, 1.4f);
    } else {
        draw->AddRect({center.x - 6, center.y - 3}, {center.x + 4, center.y + 7}, color, 0, 0, 1.4f);
        draw->AddLine({center.x - 6, center.y - 3}, {center.x - 2, center.y - 7}, color, 1.4f);
        draw->AddLine({center.x - 2, center.y - 7}, {center.x + 8, center.y - 7}, color, 1.4f);
        draw->AddLine({center.x + 8, center.y - 7}, {center.x + 8, center.y + 3}, color, 1.4f);
        draw->AddLine({center.x + 8, center.y + 3}, {center.x + 4, center.y + 7}, color, 1.4f);
        draw->AddLine({center.x + 4, center.y - 3}, {center.x + 8, center.y - 7}, color, 1.4f);
    }
}
}

std::optional<ObjectPlacement> ResolveObjectPlacement(const Scene& source, const gameplay::World& world,
    const gameplay::ObjectPreset& preset, const Ray& ray, const gameplay::Transform& camera, float snapStep, SurfacePickCache* cache) {
    if (!IsFinite(ray.origin) || !IsFinite(ray.direction) || Length(ray.direction) < .0001f ||
        !std::isfinite(ray.tMin) || std::isnan(ray.tMax) || ray.tMax < ray.tMin) return {};
    ObjectPlacement result;
    result.transform = PresetTransform(preset.object);
    const auto configuredOffset = result.transform.position;
    const auto forward = MakeCameraBasis(camera.rotationDegrees.x, camera.rotationDegrees.y, camera.rotationDegrees.z).forward;
    // 只采用渲染器接受的真实表面；地面回退必须沿正向射线且位于完整裁剪区间。
    if (const auto hit = cache ? cache->Pick(source, world, ray) : PickSurface(source, world, ray)) {
        result.contact = hit->position;
        result.normal = hit->normal;
        result.anchor = PlacementAnchor::Surface;
    } else {
        const float groundDistance = std::abs(ray.direction.y) > .00001f ? -ray.origin.y / ray.direction.y : -1;
        if (groundDistance > 0 && groundDistance >= ray.tMin && groundDistance <= ray.tMax) {
            result.contact = ray.origin + ray.direction * groundDistance;
            result.normal = {0, 1, 0};
            result.anchor = PlacementAnchor::Ground;
        } else {
            const float denominator = Dot(ray.direction, forward);
            if (denominator <= .00001f) return {};
            const float distance = Dot(camera.position + forward * 3 - ray.origin, forward) / denominator;
            if (distance <= 0 || distance < ray.tMin || distance > ray.tMax) return {};
            result.contact = ray.origin + ray.direction * distance;
            result.normal = -forward;
            result.anchor = PlacementAnchor::Camera;
        }
    }
    const auto unsnapped = result.contact;
    if (std::isfinite(snapStep) && snapStep > 0) {
        result.contact = {std::round(result.contact.x / snapStep) * snapStep, std::round(result.contact.y / snapStep) * snapStep,
            std::round(result.contact.z / snapStep) * snapStep};
        // 吸附只改变切向坐标，投影回接触平面防止舍入把物体推入墙面。
        result.contact -= result.normal * Dot(result.contact - unsnapped, result.normal);
    }
    // 配置平移作为世界轴偏移叠加在吸附锚点上，预览与创建均只应用一次。
    result.transform.position = result.contact + configuredOffset;
    if (preset.object.has_light()) result.transform.position += result.normal * .15f;
    else if (result.anchor != PlacementAnchor::Camera && preset.triangleCount) {
        const auto bounds = PresetBounds(source, preset);
        if (!IsFinite(bounds.minimum) || !IsFinite(bounds.maximum)) return {};
        auto localPose = result.transform;
        localPose.position = configuredOffset;
        float minimum = std::numeric_limits<float>::infinity();
        // 保留预设朝向；立方体和平面都用变换后包围盒支撑，平面仍保持配置的 XZ 朝向。
        for (const auto corner : BoundCorners(bounds)) minimum = std::min(minimum, Dot(TransformPoint(localPose, corner), result.normal));
        result.transform.position += result.normal * std::max(0.0f, -minimum);
        const auto extent = bounds.maximum - bounds.minimum;
        // 零厚度预设略微离开宿主平面，避免放置后的两个共面表面发生深度闪烁。
        if (std::min({extent.x, extent.y, extent.z}) < .00001f) result.transform.position += result.normal * .001f;
    }
    if (!IsFinite(result.transform.position) || !IsFinite(result.contact)) return {};
    return result;
}

void DrawObjectPlacementPreview(const ObjectPlacement& placement, const gameplay::ObjectPreset& preset,
    const Scene& source, const ViewportRect& image, const gameplay::RenderCamera& camera) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = IM_COL32(100, 230, 165, 255);
    draw->PushClipRect({image.x, image.y}, {image.x + image.width, image.y + image.height}, true);
    // 预览只绘制细线和接触标记，绝不提前实例化或污染历史。
    if (const auto point = Project(placement.contact, image, camera)) {
        draw->AddCircle(*point, 5, color, 16, 1.5f);
        draw->AddLine({point->x - 9, point->y}, {point->x + 9, point->y}, color);
        draw->AddLine({point->x, point->y - 9}, {point->x, point->y + 9}, color);
    }
    const auto bounds = PresetBounds(source, preset);
    if (IsFinite(bounds.minimum) && IsFinite(bounds.maximum)) {
        const auto corners = BoundCorners(bounds);
        std::array<std::optional<ImVec2>, 8> points;
        for (std::size_t index = 0; index < points.size(); ++index) points[index] = Project(TransformPoint(placement.transform, corners[index]), image, camera);
        constexpr std::array<std::array<std::size_t, 2>, 12> edges{{{0, 1}, {1, 2}, {2, 3}, {3, 0},
            {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
        for (const auto& edge : edges) if (points[edge[0]] && points[edge[1]]) draw->AddLine(*points[edge[0]], *points[edge[1]], color, 1.5f);
    } else if (const auto point = Project(placement.transform.position, image, camera)) {
        draw->AddCircle(*point, 11, color, 24, 1.5f);
    }
    draw->PopClipRect();
}

void EditorObjectPalette::Reset() {
    // 世界替换清除错误，尚未释放的源拖动必须先取消再允许新世界接收。
    error_.clear();
    cancelled_ = gesture_;
}

void EditorObjectPalette::Draw(const gameplay::World& world, bool enabled) {
    const auto& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // 释放帧完成交付后结束手势；场景查询缓存由 EditorUi 统一维护。
        gesture_ = cancelled_ = false;
    }
    blocking_ = gesture_;
    // Escape 与应用失焦都取消整个按压；保留释放帧捕获避免落点变成普通单击。
    if (gesture_ && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || io.AppFocusLost)) {
        cancelled_ = true;
        const auto* payload = ImGui::GetDragDropPayload();
        if (payload && payload->IsDataType(objectPresetPayload)) ImGui::ClearDragDrop();
    }
    ImGui::SetNextWindowSizeConstraints({200, 140}, {FLT_MAX, FLT_MAX});
    if (ImGui::Begin("常用物体###ObjectPalette", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##PresetSearch", "搜索", search_.data(), search_.size());
        const auto& presets = world.ObjectPresets();
        unsigned lightCount = 0;
        for (const auto entity : world.Registry().view<const gameplay::Light>())
            if (world.Registry().get<gameplay::Light>(entity).enabled) ++lightCount;
        if (lightCount >= 16) ImGui::TextWrapped("启用光源已达上限（16）");
        if (!error_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .42f, .36f, 1));
            ImGui::TextWrapped("创建失败：%s", error_.c_str());
            ImGui::PopStyleColor();
        }
        // 分类保留配置顺序和稳定索引，搜索只影响当前行的可见性。
        for (int group = 0; group < 2; ++group) {
            ImGui::SeparatorText(group == 0 ? "基础几何" : "光源");
            for (std::uint32_t index = 0; index < presets.size(); ++index) {
                const auto& object = presets[index].object;
                if (object.has_camera() || object.has_light() != (group == 1) || !Matches(object, search_.data())) continue;
                ImGui::PushID(static_cast<int>(index));
                ImGui::BeginDisabled(!enabled);
                const auto corner = ImGui::GetCursorScreenPos();
                const float textWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - 30);
                const auto textSize = ImGui::CalcTextSize(object.name().c_str(), nullptr, false, textWidth);
                // 自定义长名称按内容区换行，图标和整行命中范围不会越过相邻面板。
                const float height = std::max(26.0f, textSize.y + 10);
                ImGui::Selectable("##PresetRow", false, ImGuiSelectableFlags_None, {0, height});
                PresetIcon(object, {corner.x + 13, corner.y + height * .5f}, ImGui::GetColorU32(ImGuiCol_Text));
                ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    {corner.x + 30, corner.y + (height - textSize.y) * .5f}, ImGui::GetColorU32(ImGuiCol_Text),
                    object.name().c_str(), nullptr, textWidth);
                if (!cancelled_ && ImGui::BeginDragDropSource()) {
                    if (!gesture_) error_.clear();
                    gesture_ = blocking_ = true;
                    ImGui::SetDragDropPayload(objectPresetPayload, &index, sizeof(index));
                    ImGui::TextUnformatted(object.name().c_str());
                    ImGui::EndDragDropSource();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}
}
