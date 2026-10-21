#pragma once
#include "msad/Hal.h"
#include "msad/Logger.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <algorithm>

namespace msad {
// Only wrap network sources. Each failed read gets a bounded recovery budget;
// the delegate must implement thread-safe requestStop to interrupt network I/O.
class ReconnectingSource : public IVideoSource {
public:
    explicit ReconnectingSource(std::unique_ptr<IVideoSource> source,
                                int attempts=5, int delayMs=250)
        : source_(std::move(source)), attempts_(attempts), delayMs_(delayMs) {}
    bool open(const std::string& uri) override {
        uri_=uri;
        return !stopped_ && source_->open(uri);
    }
    bool readFrame(Frame& frame) override {
        if(stopped_) return false;
        if(source_->readFrame(frame)) return !stopped_;
        for(int attempt=0; attempt<attempts_ && !stopped_; ++attempt) {
            source_->close();
            Logger::warn("RTSP", "reconnect attempt="+std::to_string(attempt+1));
            std::unique_lock<std::mutex> lock(mutex_);
            if(wake_.wait_for(lock,std::chrono::milliseconds(std::min(4000,delayMs_*(1<<std::min(attempt,4)))),
                              [this]{return stopped_.load();})) return false;
            lock.unlock();
            if(source_->open(uri_) && !stopped_ && source_->readFrame(frame)) {
                Logger::info("RTSP", "reconnected; frames resumed");
                return !stopped_;
            }
        }
        return false;
    }
    void requestStop() override {
        {std::lock_guard<std::mutex> lock(mutex_);stopped_=true;}
        source_->requestStop();wake_.notify_all();
    }
    void close() override { source_->close(); }
    std::string name() const override {return "Reconnecting("+source_->name()+")";}
private:
    std::unique_ptr<IVideoSource> source_;
    std::string uri_;
    int attempts_,delayMs_;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
};
}
