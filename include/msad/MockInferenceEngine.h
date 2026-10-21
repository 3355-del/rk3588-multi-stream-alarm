/*
 * MockInferenceEngine.h — 模拟推理引擎（模拟桩）
 *
 * 模拟行为：从共享 SimScene 读取目标真实位置，生成带轻微噪声的检测框，
 * 模拟"看到目标并输出检测结果"。不加载任何真实模型、不访问 NPU。
 *
 * 移植 RK3588：实现 RknnInferenceEngine : IInferenceEngine，
 *   内部 rknn_api 加载 YOLOv8n INT8 .rknn → 前向 → 解码输出 → 调用 NMS。
 */
#ifndef MULTI_STREAM_MOCKINFERENCEENGINE_H
#define MULTI_STREAM_MOCKINFERENCEENGINE_H

#include <memory>

#include "msad/Hal.h"
#include "msad/SimScene.h"

namespace msad {

class MockInferenceEngine : public IInferenceEngine {
public:
    explicit MockInferenceEngine(std::shared_ptr<SimScene> scene);

    bool init() override;
    bool infer(const Frame& frame, std::vector<Detection>& dets) override;

private:
    std::shared_ptr<SimScene> scene_;   /* 与 MockVideoSource 共享的场景 */
};

} // namespace msad

#endif // MULTI_STREAM_MOCKINFERENCEENGINE_H
