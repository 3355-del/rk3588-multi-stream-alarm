/*
 * MockDecoder.cpp — 模拟视频解码器实现
 */
#include "MockDecoder.h"

#include "msad/Logger.h"

namespace msad {

MockDecoder::MockDecoder(std::shared_ptr<SimScene> scene, int fps)
    : scene_(std::move(scene)), fps_(fps) {}

bool MockDecoder::open(const std::string& uri) {
    if (!scene_ || fps_ < 1 || fps_ > 240) {
        Logger::error("MockDecoder", "scene is null");
        return false;
    }
    /* 模拟打开解码器：仅记录 URI（真实板卡上这里是 MPP 打开文件/RTSP） */
    uri_ = uri;
    opened_ = true;
    frameIndex_ = 0;
    Logger::info("MockDecoder", "mock open uri=" + uri + " (no real demux/decode)");
    return true;
}

bool MockDecoder::decodeFrame(Frame& frame) {
    if (!opened_) return false;

    /* 按配置 FPS 推进模拟时间。 */
    scene_->update(1.0f / static_cast<float>(fps_));
    /* 渲染当前场景为 YUV420 帧（模拟解码输出） */
    scene_->render(frame);
    frame.streamId = 0;                    /* 由上层覆盖为真实路号 */
    frame.frameIndex = ++frameIndex_;
    frame.timestampMs = frame.frameIndex * 1000 / static_cast<uint64_t>(fps_);
    return true;
}

void MockDecoder::close() {
    if (!opened_) return;
    opened_ = false;
    Logger::info("MockDecoder", "mock close uri=" + uri_);
}

std::string MockDecoder::name() const {
    return "MockDecoder";
}

} // namespace msad
