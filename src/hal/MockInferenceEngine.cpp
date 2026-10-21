/*
 * MockInferenceEngine.cpp — 模拟推理引擎实现
 */
#include "msad/MockInferenceEngine.h"

#include <cmath>

#include "msad/Logger.h"

namespace msad {

MockInferenceEngine::MockInferenceEngine(std::shared_ptr<SimScene> scene)
    : scene_(std::move(scene)) {}

bool MockInferenceEngine::init() {
    Logger::info("MockInferenceEngine",
                 "mock init (rknn_api + YOLOv8n INT8 would be loaded on RK3588)");
    return true;
}

bool MockInferenceEngine::infer(const Frame& frame, std::vector<Detection>& dets) {
    if (!scene_) return false;
    if (!frame.validDimensions() || frame.data.size() < frame.yuvSize()) return false;
    dets = frame.mockDetections;
    return true;
}

} // namespace msad
