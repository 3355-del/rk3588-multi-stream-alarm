#pragma once
#include "msad/Hal.h"
#include <memory>
#include <atomic>
namespace msad {
// H.264/H.265 demuxed by FFmpeg, decoded by Rockchip MPP. Local clips repeat;
// network failures are reported to the pipeline rather than replaced by mock frames.
class RkMppSource : public IVideoSource {
public:
    RkMppSource();
    ~RkMppSource() override;
    bool open(const std::string& uri) override;
    bool readFrame(Frame& frame) override;
    void close() override;
    void requestStop() override { stopped_->store(true); }
    std::string name() const override { return "RkMppSource"; }
private:
    struct State;
    std::shared_ptr<State> state_;
    std::string uri_;
    uint64_t sequence_=0;
    std::shared_ptr<std::atomic<bool>> stopped_=std::make_shared<std::atomic<bool>>(false);
};
}
