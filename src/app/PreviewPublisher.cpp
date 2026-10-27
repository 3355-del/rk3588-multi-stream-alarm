#include "msad/PreviewPublisher.h"
#include "msad/Json.h"
#include "msad/Logger.h"
#include "msad/PngWriter.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace msad {
PreviewPublisher::PreviewPublisher(const std::string& directory, int fps)
    : directory_(std::filesystem::u8path(directory)), fps_(fps) {
    if (fps < 1 || fps > 5) throw std::runtime_error("preview fps must be 1..5");
    std::filesystem::create_directories(directory_);
    worker_ = std::thread(&PreviewPublisher::run, this);
}
PreviewPublisher::~PreviewPublisher() { stop(); }
void PreviewPublisher::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; pending_.clear(); }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void PreviewPublisher::submit(const DetectResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    const auto now = std::chrono::steady_clock::now();
    auto& last = last_[result.streamId];
    if (now - last < std::chrono::milliseconds(1000 / fps_)) return;
    // Bound optional preview memory independently of the pipeline's queue budget.
    if (static_cast<int64_t>(result.frame.width) * result.frame.height > 1280 * 1280)
        throw std::runtime_error("preview supports at most 1280*1280 pixels per stream");
    last = now;
    pending_.insert_or_assign(result.streamId, result);
    ready_.notify_one();
}
void PreviewPublisher::run() {
    for (;;) {
        DetectResult result;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
            if (stopped_) return;
            auto item = pending_.begin();
            result = std::move(item->second); pending_.erase(item);
        }
        const std::string prefix = "s" + std::to_string(result.streamId);
        const auto image = directory_ / (prefix + "_" + std::to_string(++sequence_) + ".png");
        const auto temporary = directory_ / (prefix + ".tmp");
        bool published = false;
        try {
            if (!PngWriter::saveFrameAsPng(result.frame, image.u8string(), {}))
                throw std::runtime_error("cannot publish preview image");
            Json data(Json::Type::Object);
            data["stream_id"] = result.streamId;
            data["sequence"] = static_cast<double>(sequence_);
            data["frame_index"] = static_cast<double>(result.frame.frameIndex);
            data["timestamp_ms"] = static_cast<double>(result.timestampMs);
            data["width"] = result.frame.width; data["height"] = result.frame.height;
            data["image"] = image.filename().u8string();
            data["detections"] = Json(Json::Type::Array);
            for (const auto& detection : result.detections) {
                Json box(Json::Type::Object);
                box["class_id"] = detection.classId; box["name"] = cocoClassName(detection.classId);
                box["confidence"] = static_cast<double>(detection.confidence);
                box["x"] = static_cast<double>(detection.box.x); box["y"] = static_cast<double>(detection.box.y);
                box["w"] = static_cast<double>(detection.box.w); box["h"] = static_cast<double>(detection.box.h);
                data["detections"].array().push_back(box);
            }
            const auto target = directory_ / (prefix + ".json");
            { std::ofstream file(temporary, std::ios::binary); file << data.dump(); file.close();
              if (!file) throw std::runtime_error("cannot write preview metadata"); }
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::runtime_error("cannot replace preview metadata");
#else
            std::filesystem::rename(temporary, target);
#endif
            published = true;
            auto& previous = images_[result.streamId]; previous.push_back(image);
            while (previous.size() > 3) {
                std::error_code ignored; std::filesystem::remove(previous.front(), ignored);
                previous.erase(previous.begin());
            }
        } catch (const std::exception& error) {
            // A metadata failure must not accumulate orphan preview images.
            std::error_code ignored;
            if (!published) std::filesystem::remove(image, ignored);
            std::filesystem::remove(temporary, ignored);
            // Preview failure is visible in logs; detection and durable alarms continue.
            Logger::warn("Preview", error.what());
        }
    }
}
}
