/*
 * MockAiInfer.h — 模拟 AI 推理引擎（模拟桩）
 *
 * 模拟行为：不做真实图像检测；从共享模拟场景读取目标真实位置，
 * 生成带轻微噪声的检测框 + 置信度，模拟"推理输出"。
 *
 * 移植 RK3588：实现 RknnInfer : IAiInfer，
 *   内部 rknn_api 加载 YOLOv8n INT8 .rknn → 前向 → 后处理 → 手写 NMS。
 */
#ifndef MODULE_MOCKAIINFER_H
#define MODULE_MOCKAIINFER_H

#include <memory>

#include "IAiInfer.h"
#include "msad/SimScene.h"

namespace msad {

class MockAiInfer : public IAiInfer {
public:
    explicit MockAiInfer(std::shared_ptr<SimScene> scene);

    bool init() override;
    bool infer(const Frame& frame, std::vector<Detection>& dets) override;

private:
    std::shared_ptr<SimScene> scene_;
};

} // namespace msad

#endif // MODULE_MOCKAIINFER_H
