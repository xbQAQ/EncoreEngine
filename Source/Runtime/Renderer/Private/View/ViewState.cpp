// 相机和采样推进属于视图职责，与 GPU 历史分配分开维护。
#include "View/ViewState.h"
#include "render/RenderSettings.h"
#include <cmath>
#include <stdexcept>

namespace render {
void ViewState::Begin(const gameplay::RenderSnapshot& snapshot, unsigned width, unsigned height, bool reset) {
    // 相机准备只计算 CPU 数据，保持与编辑器导航共用的完整 XYZ 姿态约定。
    if (!width || !height) throw std::invalid_argument("ViewState requires a nonzero viewport extent");
    constexpr float radians = 3.14159265359f / 180.0f;
    const gameplay::RenderCamera fallback{entt::null, {{0, 1.4f, -4.6f}, {}, {1, 1, 1}}, {}};
    const auto& camera = snapshot.cameras.empty() ? fallback : snapshot.cameras.front();
    const auto position = camera.transform.position;
    const auto [forward, right, up] = MakeCameraBasis(camera.transform.rotationDegrees.x,
        camera.transform.rotationDegrees.y, camera.transform.rotationDegrees.z);
    current_.position = {position.x, position.y, position.z, 0};
    current_.forward = {forward.x, forward.y, forward.z, std::tan(0.5f * camera.camera.verticalFovDegrees * radians)};
    current_.right = {right.x, right.y, right.z, static_cast<float>(width) / height};
    current_.up = {up.x, up.y, up.z, 0};
    current_.resolution = {static_cast<float>(width), static_cast<float>(height), 1.0f / width, 1.0f / height};
    current_.projection = {camera.camera.nearPlane, camera.camera.farPlane, 0, 0};
    reset_ = reset;
}

FrameConstants ViewState::Parameters() const {
    // 保留现有 HLSL ABI，但只生成视图字段；算法字段由具体 Pass 的设置函数填写。
    FrameConstants parameters{};
    const auto& previous = frameIndex_ ? previous_ : current_;
    parameters.cameraPosition = current_.position;
    parameters.cameraForward = current_.forward;
    parameters.cameraRight = current_.right;
    parameters.cameraUp = current_.up;
    parameters.previousPosition = previous.position;
    parameters.previousForward = previous.forward;
    parameters.previousRight = previous.right;
    parameters.previousUp = previous.up;
    parameters.resolution = current_.resolution;
    parameters.projection = current_.projection;
    parameters.settings.x = frameIndex_;
    parameters.timing.z = reset_ ? 0.0f : static_cast<float>(accumulated_);
    return parameters;
}

void ViewState::Complete(RenderSettings& settings) {
    // 仅成功完成的帧推进历史与样本序列，保持原来的重置采样计数语义。
    previous_ = current_;
    accumulated_ = settings.reset ? settings.samples : accumulated_ + settings.samples;
    settings.reset = false;
    ++frameIndex_;
}
}
