#include "render/ShadowViews.h"
#include "Shadows/ShadowFiltering.h"
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b, float epsilon = 0.0001f) { return std::abs(a - b) < epsilon; }
render::Aabb Bounds() { return {{-2, -1, -3}, {4, 5, 6}}; }
render::GpuLight Light(unsigned type) {
    render::GpuLight light;
    light.positionType = {0, 8, 0, static_cast<float>(type)};
    light.directionRange = {0, -1, 0, 100};
    light.tangentWidth = {1, 0, 0, 2};
    light.bitangentHeight = {0, 0, 1, 2};
    light.parameters = {0.1f, 0.8f, 0.9f, 0};
    light.shadow.z = 7;
    return light;
}
template<class F> void Rejects(F&& function) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "Invalid shadow inputs must fail explicitly");
}

void DirectionalFit() {
    std::vector<render::GpuLight> lights{Light(0)};
    lights[0].directionRange = {0.3f, -0.8f, 0.5f, 0};
    const auto views = render::BuildShadowViews(lights, Bounds());
    Check(views.size() == 1 && views[0].forwardProjection.w == 0, "Directional uses one orthographic view");
    for (unsigned i = 0; i < 8; ++i) {
        const render::Vec3 corner{i & 1 ? 4.0f : -2.0f, i & 2 ? 5.0f : -1.0f, i & 4 ? 6.0f : -3.0f};
        Check(render::ProjectShadowPoint(views[0], corner).inside, "All directional bounds corners fit");
    }
    Check(lights[0].shadow.x == 0 && lights[0].shadow.y == 1 && lights[0].shadow.z == 7, "View assignment preserves emitter metadata");
}

void PointFacesAndPacking() {
    std::vector<render::GpuLight> lights{Light(0), Light(1), Light(4)};
    const auto views = render::BuildShadowViews(lights, Bounds(), 32, 3);
    Check(views.size() == 8 && lights[1].shadow.x == 1 && lights[1].shadow.y == 6, "Point allocates contiguous six faces");
    for (unsigned i = 0; i < views.size(); ++i) {
        Check(views[i].atlas.x == (i % 3) * 32 && views[i].atlas.y == (i / 3) * 32, "Atlas tiles have stable row-major offsets");
        if (i < 1 || i > 6) continue;
        const auto& view = views[i];
        const render::Vec3 center{view.originNear.x + view.forwardProjection.x * 2,
            view.originNear.y + view.forwardProjection.y * 2, view.originNear.z + view.forwardProjection.z * 2};
        const auto projected = render::ProjectShadowPoint(view, center);
        Check(projected.inside && Near(projected.u, 0.5f) && Near(projected.v, 0.5f), "Every point face center projects to its tile center");
        Check(Near(view.upExtent.w, 1), "Point uses ninety degree face projections");
    }
}

void PerspectiveAndClip() {
    std::vector<render::GpuLight> lights{Light(2), Light(3), Light(4)};
    const auto views = render::BuildShadowViews(lights, Bounds());
    Check(views.size() == 3, "Area and spot each use one perspective view");
    Check(Near(views[2].upExtent.w, 0.75f), "Spot FOV follows the outer cosine");
    const auto& spot = views[2];
    Check(!render::ProjectShadowPoint(spot, {0, 9, 0}).inside, "Behind-light receiver is outside");
    Check(!render::ProjectShadowPoint(spot, {0, -200, 0}).inside, "Beyond-range receiver is outside");
    const render::Vec3 edge{1.5f, 6, 0};
    Check(render::ProjectShadowPoint(spot, edge).inside, "Spot cone boundary is retained");
    Check(!render::ProjectShadowPoint(spot, {1.6f, 6, 0}).inside, "Outside cone is rejected");
    // A valid glTF ninety-degree half-cone uses the finite 178-degree raster projection limit.
    lights = {Light(4)};
    lights[0].parameters.y = 0;
    const auto hemisphere = render::BuildShadowViews(lights, Bounds());
    Check(hemisphere[0].upExtent.w > 50 && hemisphere[0].upExtent.w < 58, "Hemisphere spot projection remains finite");
}

void CapacityAndAtomicity() {
    std::vector<render::GpuLight> lights(16, Light(1));
    Check(render::BuildShadowViews(lights, Bounds()).size() == 96, "Sixteen point lights fit exactly ninety-six views");
    lights.push_back(Light(0));
    Rejects([&] { render::BuildShadowViews(lights, Bounds()); });
    lights = {Light(1), Light(4)};
    lights[0].shadow.x = 71;
    lights[1].parameters.y = -0.2f;
    Rejects([&] { render::BuildShadowViews(lights, Bounds()); });
    Check(lights[0].shadow.x == 71, "Failure does not partially assign earlier lights");
    Rejects([&] { render::BuildShadowViews(lights, Bounds(), 0); });
    Rejects([&] { render::BuildShadowViews(lights, Bounds(), 256, 1); });
    Rejects([&] { render::BuildShadowViews(lights, {}); });
    lights = {Light(0)};
    lights[0].directionRange = {};
    Rejects([&] { render::BuildShadowViews(lights, Bounds()); });
    lights = {Light(2)};
    lights[0].tangentWidth.w = 0;
    Rejects([&] { render::BuildShadowViews(lights, Bounds()); });
    lights = {Light(1)};
    lights[0].parameters.x = std::numeric_limits<float>::quiet_NaN();
    Rejects([&] { render::BuildShadowViews(lights, Bounds()); });
}

void FilterBounds() {
    using namespace render::shadow;
    Check(VarianceVisibility(0.5f, 0.25f, 0.4f, 1e-5f, 0.2f) == 1, "Receiver before moment mean is lit");
    Check(VarianceVisibility(0.5f, 0.25f, 0.9f, 1e-5f, 0.2f) < 0.001f, "Variance floor does not erase an opaque blocker");
    const float soft = VarianceVisibility(0.5f, 0.3f, 0.7f, 1e-5f, 0);
    Check(soft > 0 && soft < 1, "Chebyshev produces a bounded soft result");
    Check(VarianceVisibility(0.5f, 0.3f, 0.7f, 1e-5f, 0.5f) < soft, "Bleed reduction tightens the bound");
    Check(Near(PcssPenumbra(0.2f, 8, 4, false), 0.2f), "PCSS perspective uses similar-triangle separation");
    Check(PcssPenumbra(0.2f, 4, 4, false) == 0, "Contact shadows have zero geometric penumbra");
    Check(Near(PcssPenumbra(0.01f, 8, 4, true), std::tan(0.01f) * 4), "Directional PCSS uses angular source size");
}
}

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"directional fit", DirectionalFit}, {"point faces and packing", PointFacesAndPacking},
        {"perspective clipping", PerspectiveAndClip}, {"capacity and atomicity", CapacityAndAtomicity}, {"filter bounds", FilterBounds}};
    try {
        for (const auto& [name, test] : tests) { test(); std::cout << "PASS " << name << '\n'; }
        std::cout << tests.size() << '/' << tests.size() << " shadow tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
