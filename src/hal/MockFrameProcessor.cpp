/*
 * MockFrameProcessor.cpp — 模拟图像预处理实现
 */
#include "msad/MockFrameProcessor.h"

#include "msad/Logger.h"

namespace msad {

bool MockFrameProcessor::init() {
    Logger::info("MockFrameProcessor", "mock init (RGA would be used on RK3588)");
    return true;
}

bool MockFrameProcessor::process(Frame in, Frame& out) {
    /* 模拟预处理：转移帧所有权，避免复制像素。真实实现：RGA 缩放+格式转换（YUV420→RGB888），
     * 可走 DMA-BUF 零拷贝。这里保留同样的输入输出语义。 */
    out = std::move(in);
    return true;
}

} // namespace msad
