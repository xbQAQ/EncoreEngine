#pragma once

#include "gameplay/Components.h"

namespace gameplay { class World; }
namespace render { struct Scene; }

namespace render::editor {
enum class NavigationMode {
    None, // No pointer drag; a wheel event still dollies the camera.
    Fly, // Right-button mouse look with camera-relative WASD and world-vertical Q/E.
    Pan, // Middle-button drag, including Alt+middle, moves camera and focus together.
    Orbit, // Alt+left drag rotates the camera around the retained focus pivot.
    Dolly, // Alt+right drag changes distance to the retained focus pivot.
};

struct NavigationInput {
    NavigationMode mode{NavigationMode::None}; // Captured gesture chosen by the UI input owner.
    float mouseDeltaX{}; // Horizontal pointer travel in displayed-image pixels.
    float mouseDeltaY{}; // Vertical pointer travel in displayed-image pixels, positive downward.
    float forward{}; // Signed W/S movement axis, positive along the view direction.
    float right{}; // Signed D/A movement axis, positive along the camera right axis.
    float up{}; // Signed E/Q movement axis, positive along world +Y.
    float deltaSeconds{}; // Frame duration for keyboard movement; long stalls are bounded.
    float wheel{}; // Positive wheel travel speeds up fly mode or dollies toward the focus.
    float viewportHeight{1}; // Positive displayed-image height used for distance-scaled panning.
};

// Input capture and scene dirty notifications stay in the UI; navigation owns only finite camera mathematics.
class EditorNavigation {
public:
    // Returns only pose changes, so adjusting fly speed alone does not reset renderer accumulation.
    bool Update(gameplay::Transform& transform, const gameplay::Camera& camera, const NavigationInput& input);
    // Frame the visible selection and retain its orbit pivot; malformed or hidden targets leave all state untouched.
    bool Focus(gameplay::Transform& transform, const gameplay::Camera& camera, const Scene& source,
        const gameplay::World& world, gameplay::Entity entity, float aspect);

    float Speed() const { return speed_; }
    Vec3 Pivot() const { return pivot_; }
    float FocusDistance() const { return focusDistance_; }

private:
    float speed_{2}; // Persistent world-meter fly speed, adjusted only by the wheel during fly mode.
    Vec3 pivot_{}; // Retained world-space target shared by orbit, pan and dolly gestures.
    float focusDistance_{5}; // Last valid pivot distance, also setting the pan scale before explicit focus.
    bool hasPivot_{}; // A fresh controller places its default pivot ahead of the current camera.
};
}
