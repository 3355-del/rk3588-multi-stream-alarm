/*
 * MockAiInfer.cpp — 模拟 AI 推理引擎实现
 *
 * 模拟策略：从共享 SimScene 读取目标真实位置，输出带轻微噪声的检测框；
 * 置信度按类别计算（0.75~0.98），模拟真实模型的输出分布。
 */
#include "MockAiInfer.h"

#include "msad/Logger.h"

namespace msad {

MockAiInfer::MockAiInfer(std::shared_ptr<SimScene> scene)
    : scene_(std::move(scene)) {}

bool MockAiInfer::init() {
    Logger::info("MockAiInfer",
                 "mock init (rknn_api + YOLOv8n INT8 would be loaded on RK3588)");
    return true;
}

bool MockAiInfer::infer(const Frame& frame, std::vector<Detection>& dets) {
    if (!scene_) return false;
    if (!frame.validDimensions() || frame.data.size() < frame.yuvSize()) return false;
    dets = frame.mockDetections;
    return true;
}

} // namespace msad
