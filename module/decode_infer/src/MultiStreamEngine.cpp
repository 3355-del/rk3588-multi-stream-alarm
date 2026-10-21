/*
 * MultiStreamEngine.cpp — 多路"解码 + 推理"业务模块实现
 */
#include "MultiStreamEngine.h"

#include <chrono>
#include <sstream>
#include <iomanip>
#include <set>
#include <exception>
#include <algorithm>

#include "MockAiInfer.h"
#include "MockDecoder.h"
#include "msad/Logger.h"

namespace msad {

MultiStreamEngine::~MultiStreamEngine() {
    stop();
}

bool MultiStreamEngine::start(const std::vector<StreamConfig>& cfgs,
                              size_t queueCapacity) {
    if (running_.load()) {
        Logger::error("Engine", "already running");
        return false;
    }
    if (cfgs.empty()) {
        Logger::error("Engine", "no stream configured");
        return false;
    }

    if (!threads_.empty() || cfgs.size() > 4 || queueCapacity < 1 || queueCapacity > 64) return false;
    std::set<int> ids;
    size_t bytes = 0;
    for (const auto& c : cfgs) {
        Frame f; f.width=c.frameWidth; f.height=c.frameHeight;
        if (!f.validDimensions() || c.fps < 1 || c.fps > 240 || c.targetCount < 0 || c.targetCount > 1000 ||
            c.id < 0 || !ids.insert(c.id).second) return false;
        bytes += f.yuvSize() * (queueCapacity + 4);
    }
    if (bytes > 512ULL * 1024 * 1024) return false;
    try {
    cfgs_ = cfgs;
    const size_t n = cfgs_.size();
    scenes_.resize(n);
    decoders_.resize(n);
    infers_.resize(n);
    queues_.resize(n);

    for (size_t i = 0; i < n; ++i) {
        /* 每路一个共享模拟场景：解码渲染 + 推理读取，画面与结果自洽 */
        auto scene = std::make_shared<SimScene>(
            cfgs_[i].frameWidth, cfgs_[i].frameHeight, cfgs_[i].targetCount, 20260828u ^ static_cast<unsigned>(cfgs_[i].id));
        scenes_[i] = scene;

        /* 组装 mock 组件（RK3588 移植点：替换为 RkMppDecoder / RknnInfer） */
        decoders_[i] = std::make_unique<MockDecoder>(scene, cfgs_[i].fps);
        infers_[i]   = std::make_unique<MockAiInfer>(scene);

        if (!decoders_[i]->open(cfgs_[i].uri)) {
            Logger::error("Engine", "stream " + std::to_string(i) + " decoder open failed");
            return false;
        }
        if (!infers_[i]->init()) {
            Logger::error("Engine", "stream " + std::to_string(i) + " infer init failed");
            return false;
        }

        /* 每路独立帧队列（满丢最旧，解码线程永不阻塞） */
        queues_[i] = std::make_unique<RingBuffer<Frame>>(queueCapacity);

        Logger::info("Engine", "stream " + std::to_string(i) +
                     " ready, uri=" + cfgs_[i].uri +
                     ", fps=" + std::to_string(cfgs_[i].fps) +
                     ", targets=" + std::to_string(cfgs_[i].targetCount));
    }

    /* 每源两条线程：解码 + 推理 */
    running_.store(true);
    for (size_t i = 0; i < n; ++i) {
        threads_.emplace_back([this, i] {
            try { decodeLoop(i); } catch (...) { running_.store(false); for (auto& q : queues_) q->stop(); }
        });
        threads_.emplace_back([this, i] {
            try { inferLoop(i); } catch (...) { running_.store(false); for (auto& q : queues_) q->stop(); }
        });
    }
    Logger::info("Engine", "engine started, streams=" + std::to_string(n) +
                 ", threads=" + std::to_string(n * 2));
    return true;
    } catch (const std::exception& e) {
        Logger::error("Engine", e.what()); stop(); return false;
    }
}

void MultiStreamEngine::wait() {
    /* 阻塞直到 stop() 被调用（main 中由信号或定时触发） */
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void MultiStreamEngine::stop() {
    std::lock_guard<std::mutex> lock(stopMtx_);
    running_.store(false);

    /* 1. 停所有队列：唤醒阻塞中的推理线程（防死锁关键） */
    for (auto& q : queues_) {
        if (q) q->stop();
    }

    /* 2. join 所有线程（幂等） */
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();

    /* 3. 释放资源 */
    for (auto& d : decoders_) {
        if (d) d->close();
    }
    decoders_.clear();
    infers_.clear();
    scenes_.clear();
    queues_.clear();

    Logger::info("Engine", "engine stopped, resources released");
}

/* ================= 解码线程 ================= */

void MultiStreamEngine::decodeLoop(size_t idx) {
    Logger::info("Engine", "decode thread start, stream=" + std::to_string(idx));
    const auto interval = std::chrono::nanoseconds(1000000000 / cfgs_[idx].fps);
    auto nextFrame = std::chrono::steady_clock::now();

    while (running_.load()) {
        Frame frame;
        if (!decoders_[idx]->decodeFrame(frame)) {
            Logger::warn("Engine", "stream " + std::to_string(idx) + " decode EOF/error");
            queues_[idx]->stop();
            break;
        }
        frame.streamId = cfgs_[idx].id;
        /* 入队：满丢最旧，解码线程不被阻塞 */
        queues_[idx]->push(std::move(frame));

        /* 按配置帧率节流 */
        nextFrame += interval;
        if (nextFrame < std::chrono::steady_clock::now()) nextFrame = std::chrono::steady_clock::now();
        while (running_.load() && std::chrono::steady_clock::now() < nextFrame)
            std::this_thread::sleep_until(std::min(nextFrame, std::chrono::steady_clock::now()+std::chrono::milliseconds(20)));
    }
    Logger::info("Engine", "decode thread exit, stream=" + std::to_string(idx));
}

/* ================= 推理线程 ================= */

void MultiStreamEngine::inferLoop(size_t idx) {
    Logger::info("Engine", "infer thread start, stream=" + std::to_string(idx));

    while (running_.load()) {
        Frame frame;
        if (!queues_[idx]->pop(frame)) break;   /* 队列 stop 后返回 false */

        /* 推理（mock：模拟输出检测框） */
        std::vector<Detection> dets;
        if (!infers_[idx]->infer(frame, dets)) continue;

        /* 打印模拟检测结果 */
        std::ostringstream message;
        message << "[stream=" << frame.streamId << "][frame=" << frame.frameIndex
                << "][ts=" << frame.timestampMs << "ms] detections=" << dets.size();
        for (const auto& d : dets) {
            message << " | " << className(d.classId) << '(' << std::fixed << std::setprecision(2)
                    << d.confidence << ") box=(" << d.box.x << ',' << d.box.y << ','
                    << d.box.w << ',' << d.box.h << ')';
        }
        Logger::info("Engine", message.str());

    }
    Logger::info("Engine", "infer thread exit, stream=" + std::to_string(idx));
}

} // namespace msad
