/*
 * MockFrameProcessor.h — 模拟图像预处理（模拟桩）
 *
 * 模拟行为：透传帧（真实 RK3588 上用 RGA 做缩放/格式转换/归一化）。
 * 保留 process() 签名与语义，便于后续替换为 RGA 硬件实现。
 */
#ifndef MULTI_STREAM_MOCKFRAMEPROCESSOR_H
#define MULTI_STREAM_MOCKFRAMEPROCESSOR_H

#include "msad/Hal.h"

namespace msad {

class MockFrameProcessor : public IFrameProcessor {
public:
    bool init() override;
    bool process(Frame in, Frame& out) override;
};

} // namespace msad

#endif // MULTI_STREAM_MOCKFRAMEPROCESSOR_H
