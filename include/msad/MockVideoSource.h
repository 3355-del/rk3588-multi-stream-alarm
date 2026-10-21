/*
 * MockVideoSource.h — 模拟视频源（模拟桩）
 *
 * 模拟行为：不访问任何真实硬件/文件，周期性渲染 SimScene 生成 YUV420 帧，
 * 帧率由外部调用节奏决定（流水线采集线程负责定时）。
 */
#ifndef MULTI_STREAM_MOCKVIDEOSOURCE_H
#define MULTI_STREAM_MOCKVIDEOSOURCE_H

#include <memory>
#include <string>

#include "msad/Hal.h"
#include "msad/SimScene.h"

namespace msad {

class MockVideoSource : public IVideoSource {
public:
    /* 传入共享的模拟场景（与 MockInferenceEngine 共用同一场景） */
    explicit MockVideoSource(std::shared_ptr<SimScene> scene, int fps = 30);

    bool open(const std::string& uri) override;
    bool readFrame(Frame& frame) override;
    void close() override;
    std::string name() const override;

private:
    std::shared_ptr<SimScene> scene_;   /* 共享模拟场景 */
    int fps_;
    bool opened_ = false;
    uint64_t frameCount_ = 0;
};

} // namespace msad

#endif // MULTI_STREAM_MOCKVIDEOSOURCE_H
