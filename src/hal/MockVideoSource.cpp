/*
 * MockVideoSource.cpp — 模拟视频源实现
 */
#include "msad/MockVideoSource.h"

#include "msad/Logger.h"

namespace msad {

MockVideoSource::MockVideoSource(std::shared_ptr<SimScene> scene, int fps)
    : scene_(std::move(scene)), fps_(fps) {}

bool MockVideoSource::open(const std::string& uri) {
    if (!scene_ || fps_ < 1 || fps_ > 240) {
        Logger::error("MockVideoSource", "scene is null");
        return false;
    }
    /* 模拟"打开设备"：演示真实板卡上 open 的语义（校验、分配资源） */
    opened_ = true;
    frameCount_ = 0;
    Logger::info("MockVideoSource", "mock open uri=" + uri +
                 " (no real hardware)");
    return true;
}

bool MockVideoSource::readFrame(Frame& frame) {
    if (!opened_) return false;

    /* 按配置 FPS 推进模拟时间。 */
    scene_->update(1.0f / static_cast<float>(fps_));
    /* 渲染当前场景到帧（YUV420P） */
    scene_->render(frame);
    frame.streamId = 0;                  /* 由流水线覆盖为真实路号 */
    frame.frameIndex = ++frameCount_;
    frame.timestampMs = frame.frameIndex * 1000 / static_cast<uint64_t>(fps_);
    return true;
}

void MockVideoSource::close() {
    if (!opened_) return;
    opened_ = false;
    Logger::info("MockVideoSource", "mock close");
}

std::string MockVideoSource::name() const {
    return "MockVideoSource";
}

} // namespace msad
