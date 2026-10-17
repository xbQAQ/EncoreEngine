// 视图契约可独立于 GPU 帧入口验证，私有路径显式标明职责。
#include "View/ViewState.h"
#include "render/RenderSettings.h"
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool Near(float left, float right) {
    return std::abs(left - right) < 0.00001f;
}
bool Same(const render::Float4& left, const render::Float4& right) {
    return Near(left.x, right.x) && Near(left.y, right.y) && Near(left.z, right.z) && Near(left.w, right.w);
}
bool Same(const render::Uint4& left, const render::Uint4& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}

gameplay::RenderSnapshot CameraSnapshot() {
    // 使用非默认位置、投影和宽高比，让漏填字段不能被零值或默认值掩盖。
    gameplay::RenderSnapshot snapshot;
    gameplay::RenderCamera camera;
    camera.transform.position = {2, 3, -4};
    camera.camera.verticalFovDegrees = 90;
    camera.camera.nearPlane = 0.25f;
    camera.camera.farPlane = 320;
    snapshot.cameras.push_back(camera);
    return snapshot;
}

void FirstView() {
    render::ViewState view;
    view.Begin(CameraSnapshot(), 640, 320, true);
    const auto values = view.Parameters();
    // 首帧没有已提交历史，必须以当前相机作为前帧基底。
    Require(Same(values.cameraPosition, {2, 3, -4, 0}), "Camera position reaches the first view snapshot");
    Require(Same(values.cameraForward, {0, 0, 1, 1}) && Same(values.cameraRight, {1, 0, 0, 2}) &&
        Same(values.cameraUp, {0, 1, 0, 0}), "Identity view keeps axes, FOV and aspect ratio");
    Require(Same(values.previousPosition, values.cameraPosition) && Same(values.previousForward, values.cameraForward) &&
        Same(values.previousRight, values.cameraRight) && Same(values.previousUp, values.cameraUp),
        "The first view reprojects from its own camera basis");
    Require(Same(values.resolution, {640, 320, 1.0f / 640, 1.0f / 320}) && Same(values.projection, {0.25f, 320, 0, 0}),
        "Viewport and clipping planes reach the view snapshot");
    Require(values.settings.x == 0 && values.timing.z == 0 && view.FrameIndex() == 0 && view.Accumulated() == 0,
        "Preparing a view does not advance frame or sample history");
    Require(Same(values.controls, {}) && Same(values.lighting, {}) && Same(values.counts, {}) && Same(values.flags, {}) &&
        Same(values.screenFlags, {}) && Same(values.sky, {}), "ViewState does not populate algorithm or scene parameters");
}

void CommittedCameraHistory() {
    render::ViewState view;
    auto snapshot = CameraSnapshot();
    render::RenderSettings settings;
    settings.samples = 4;
    view.Begin(snapshot, 640, 320, true);
    const auto first = view.Parameters();
    view.Complete(settings);

    // 移动、旋转和视口变化只更新当前相机，前帧仍来自上次成功提交。
    snapshot.cameras[0].transform.position = {11, -2, 7};
    snapshot.cameras[0].transform.rotationDegrees.z = 90;
    snapshot.cameras[0].camera.verticalFovDegrees = 60;
    view.Begin(snapshot, 300, 300, false);
    const auto moved = view.Parameters();
    Require(Same(moved.cameraPosition, {11, -2, 7, 0}) && Same(moved.previousPosition, first.cameraPosition),
        "Moving the camera preserves the last committed position for reprojection");
    Require(Same(moved.previousForward, first.cameraForward) && Same(moved.previousRight, first.cameraRight) &&
        Same(moved.previousUp, first.cameraUp), "Changing FOV, aspect and roll preserves the committed camera basis");
    Require(Near(moved.cameraRight.x, 0) && Near(moved.cameraRight.y, 1) && Near(moved.cameraRight.w, 1) &&
        Near(moved.cameraUp.x, -1), "Roll and current viewport affect the new basis");
    Require(moved.settings.x == 1 && moved.timing.z == 4, "Completed frames advance the seed and expose sample history");

    // 重复准备但未完成提交，不能覆盖上一帧的位置、FOV 或相机基底。
    snapshot.cameras[0].transform.position = {22, 5, 9};
    view.Begin(snapshot, 300, 300, false);
    Require(Same(view.Parameters().previousPosition, first.cameraPosition), "Begin alone cannot commit camera history");
    view.Complete(settings);
    snapshot.cameras[0].transform.position = {33, 6, 10};
    view.Begin(snapshot, 300, 300, false);
    Require(Same(view.Parameters().previousPosition, {22, 5, 9, 0}), "Complete publishes the most recently prepared camera");
}

void SampleHistoryReset() {
    render::ViewState view;
    const auto snapshot = CameraSnapshot();
    render::RenderSettings settings;
    settings.samples = 4;
    view.Begin(snapshot, 640, 320, settings.reset);
    view.Complete(settings);
    Require(view.Accumulated() == 4 && view.FrameIndex() == 1 && !settings.reset,
        "Completing a reset frame records its samples and clears the reset request");

    // 不同帧采样预算按实际值累积，重置只在新快照中拒绝旧样本。
    settings.samples = 3;
    view.Begin(snapshot, 640, 320, false);
    Require(view.Parameters().timing.z == 4, "A continuing view exposes prior completed samples");
    view.Complete(settings);
    Require(view.Accumulated() == 7 && view.FrameIndex() == 2, "Continuing frames accumulate their own sample budget");
    settings.samples = 2;
    settings.reset = true;
    view.Begin(snapshot, 640, 320, true);
    Require(view.Parameters().timing.z == 0 && view.Accumulated() == 7 && view.FrameIndex() == 2,
        "Reset suppresses shader history before altering committed CPU counters");
    view.Complete(settings);
    Require(view.Accumulated() == 2 && view.FrameIndex() == 3 && !settings.reset,
        "A completed reset starts a new sample total without restarting the random frame sequence");
    view.Begin(snapshot, 640, 320, false);
    Require(view.Parameters().timing.z == 2, "The next view exposes the new post-reset sample total");
}

void MissingCameraFallback() {
    render::ViewState view;
    view.Begin({}, 320, 200, true);
    const auto values = view.Parameters();
    // 编辑器尚未放置相机时仍应得到有效投影，不能将空快照变成全零或非有限视图。
    Require(Same(values.cameraPosition, {0, 1.4f, -4.6f, 0}) && Same(values.cameraRight, {1, 0, 0, 1.6f}),
        "A missing scene camera uses the renderer fallback pose and current viewport");
    Require(std::isfinite(values.cameraForward.w) && values.cameraForward.w > 0 &&
        values.projection.x > 0 && values.projection.y > values.projection.x,
        "Fallback camera projection has a finite field of view and valid clipping planes");
    Require(Same(values.previousPosition, values.cameraPosition), "The first fallback view has no uninitialized camera history");
}

void InvalidViewportPreservesView() {
    render::ViewState view;
    view.Begin(CameraSnapshot(), 640, 320, false);
    const auto before = view.Parameters();
    // 无效视口必须在修改状态前拒绝，否则失败的准备会污染随后成功提交的相机历史。
    for (const auto extent : {std::pair{0u, 320u}, std::pair{640u, 0u}}) {
        bool rejected = false;
        try { view.Begin({}, extent.first, extent.second, true); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "A zero-sized viewport is rejected before producing reciprocal dimensions");
        const auto after = view.Parameters();
        Require(Same(before.cameraPosition, after.cameraPosition) && Same(before.cameraForward, after.cameraForward) &&
            Same(before.resolution, after.resolution) && Same(before.projection, after.projection),
            "A rejected viewport leaves the prepared CPU view intact");
        Require(view.FrameIndex() == 0 && view.Accumulated() == 0, "Rejected preparation cannot advance sample history");
    }
}
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"first CPU view", FirstView}, {"committed camera history", CommittedCameraHistory},
        {"sample history reset", SampleHistoryReset}, {"missing camera fallback", MissingCameraFallback},
        {"invalid viewport preserves view", InvalidViewportPreservesView},
    };
    int failed = 0;
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failed) << '/' << tests.size() << " view state tests passed\n";
    return failed ? 1 : 0;
}
