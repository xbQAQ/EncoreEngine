#include "editor/EditorUi.h"
#include "gameplay/World.h"
#include "render/RenderSettings.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace render::editor {
namespace {
gameplay::Entity NavigationCamera(const gameplay::World& world) {
    auto result = gameplay::Entity{entt::null};
    // Use the same deterministic active-camera policy as renderer uniforms and gizmo projection.
    for (auto [entity, transform, camera] : world.Registry().view<const gameplay::Transform, const gameplay::Camera>().each()) {
        (void)transform;
        if (camera.active && (result == entt::null || entt::to_integral(entity) < entt::to_integral(result))) result = entity;
    }
    return result;
}
}

void EditorUi::NavigateViewport(RenderSettings& settings, gameplay::World& world, const Scene& source,
    const ViewportRect& image, float aspect, float delta, bool imageHovered, bool focused, bool captured) {
    const auto& io = ImGui::GetIO();
    if (!focused || captured || io.WantTextInput || io.AppFocusLost) {
        navigationMode_ = NavigationMode::None;
        navigationButton_ = -1;
        cameraDragging_ = false;
        return;
    }
    // A captured navigation gesture retains its mode outside the image, but always ends on owner-button release.
    if (navigationButton_ >= 0 && !ImGui::IsMouseDown(navigationButton_)) {
        navigationButton_ = -1;
        navigationMode_ = NavigationMode::None;
    }
    const bool inputAvailable = !ImGui::IsAnyItemActive() || ImGui::GetActiveID() == ImGui::GetCurrentWindow()->MoveId;
    if (navigationMode_ == NavigationMode::None && imageHovered && inputAvailable) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            navigationMode_ = NavigationMode::Pan;
            navigationButton_ = ImGuiMouseButton_Middle;
        } else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            navigationMode_ = NavigationMode::Orbit;
            navigationButton_ = ImGuiMouseButton_Left;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            navigationMode_ = io.KeyAlt ? NavigationMode::Dolly : NavigationMode::Fly;
            navigationButton_ = ImGuiMouseButton_Right;
        }
    }
    cameraDragging_ = navigationMode_ != NavigationMode::None;
    if (!cameraDragging_ && inputAvailable && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        // Escape is viewport-local; duplicate, delete and undo are dispatched for the whole editor before UI mutation.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { selection_ = entt::null; CancelGizmo(); }
    }

    const auto cameraEntity = NavigationCamera(world);
    if (cameraEntity == entt::null) return;
    auto& transform = world.Registry().get<gameplay::Transform>(cameraEntity);
    const auto& camera = world.Registry().get<gameplay::Camera>(cameraEntity);
    bool changed = false;
    if (!cameraDragging_ && inputAvailable && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_F, false)) changed = navigation_.Focus(transform, camera, source, world, selection_, aspect);

    NavigationInput input;
    input.mode = navigationMode_;
    if (navigationMode_ == NavigationMode::Fly && ImGui::IsMouseDown(ImGuiMouseButton_Left)) input.mode = NavigationMode::Pan;
    input.viewportHeight = image.height;
    input.deltaSeconds = delta;
    // Initial presses carry cursor travel from another panel; start moving only after the capture frame.
    const bool starting = navigationButton_ >= 0 && ImGui::IsMouseClicked(navigationButton_);
    input.mouseDeltaX = starting ? 0 : io.MouseDelta.x;
    input.mouseDeltaY = starting ? 0 : io.MouseDelta.y;
    input.wheel = (imageHovered || cameraDragging_) ? io.MouseWheel : 0;
    if (navigationMode_ == NavigationMode::Fly && !io.KeyCtrl && !io.KeyAlt) {
        input.forward = (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f);
        input.right = (ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f);
        input.up = (ImGui::IsKeyDown(ImGuiKey_E) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0f : 0.0f);
    }
    if (navigation_.Update(transform, camera, input) || changed) {
        world.MarkDirty();
        settings.reset = true;
    }
}
}
