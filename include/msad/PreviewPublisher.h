#pragma once
#include "msad/Types.h"
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace msad {
// One pending frame per stream, throttled before copying. Never waits for a browser.
class PreviewPublisher {
public:
    PreviewPublisher(const std::string& directory, int fps);
    ~PreviewPublisher();
    void submit(const DetectResult& result);
    void stop();
private:
    void run();
    std::filesystem::path directory_;
    int fps_;
    bool stopped_ = false;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::map<int, DetectResult> pending_;
    std::map<int, std::chrono::steady_clock::time_point> last_;
    std::map<int, std::vector<std::filesystem::path>> images_;
    uint64_t sequence_ = 0;
    std::thread worker_;
};
}
