/*
 * MockDecoder.h — 模拟视频解码器（模拟桩）
 *
 * 模拟行为：不访问真实文件/网络，按调用节奏（外部线程定时调用）
 * 渲染共享模拟场景生成 YUV420 帧，模拟"解码出图像"。
 *
 * 移植 RK3588：实现 RkMppDecoder : IDecoder，
 *   内部用 MPP 解码 H.264/H.265（本地文件/RTSP 均支持）。
 */
#ifndef MODULE_MOCKDECODER_H
#define MODULE_MOCKDECODER_H

#include <memory>
#include <string>

#include "IDecoder.h"
#include "msad/SimScene.h"

namespace msad {

class MockDecoder : public IDecoder {
public:
    /* 传入共享模拟场景（与 MockAiInfer 共用，保证画面与检测结果自洽） */
    explicit MockDecoder(std::shared_ptr<SimScene> scene, int fps = 25);

    bool open(const std::string& uri) override;
    bool decodeFrame(Frame& frame) override;
    void close() override;
    std::string name() const override;

private:
    std::shared_ptr<SimScene> scene_;
    int fps_;
    bool opened_ = false;
    std::string uri_;
    uint64_t frameIndex_ = 0;
};

} // namespace msad

#endif // MODULE_MOCKDECODER_H
