/*
 * VideoPipeline.cpp — 四线程流水线实现
 */
#include "msad/VideoPipeline.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>

#include "msad/ReconnectingSource.h"
#include "msad/Logger.h"
#include "msad/MockFrameProcessor.h"
#include "msad/MockInferenceEngine.h"
#include "msad/MockVideoSource.h"
#include "msad/Nms.h"
#include "msad/PngWriter.h"
#include "msad/TcpAlarmNotifier.h"
#include "msad/DurableAlarmNotifier.h"

#if defined(MSAD_ENABLE_RKNN)
#include "msad/RealComponents.h"   /* RK3588 真实 HAL 实现（V4L2 / RGA / RKNN） */
#include "msad/RkMppSource.h"
#endif

namespace msad {

VideoPipeline::VideoPipeline(const AppConfig& cfg, ScreenshotWriter writer, ResultObserver observer, bool previewOnly)
    : cfg_(cfg), screenshotWriter_(writer ? std::move(writer) : ScreenshotWriter(PngWriter::saveFrameAsPng)),
      observer_(std::move(observer)), previewOnly_(previewOnly), tracks_(cfg.streams.size()) {}

VideoPipeline::~VideoPipeline() {
    stop();
}

/* ================= 计数信号量 ================= */

bool VideoPipeline::CountingSemaphore::acquire() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return stopped_ || permits_ > 0; });
    if (stopped_) return false;
    --permits_;
    return true;
}

void VideoPipeline::CountingSemaphore::stop() {
    { std::lock_guard<std::mutex> lock(mtx_); stopped_ = true; }
    cv_.notify_all();
}

void VideoPipeline::CountingSemaphore::release() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ++permits_;
    }
    cv_.notify_one();
}

/* ================= 启动/停止 ================= */

bool VideoPipeline::start() {
    if (running_.load() || !threads_.empty() || ioThread_.joinable()) return false;
    std::string error;
    if (!cfg_.validate(error)) { Logger::error("Pipeline", error); return false; }
    failed_.store(false);
    regions_.clear();
    for (auto& tracks : tracks_) tracks.clear();
    stats_.assign(cfg_.streams.size(), Stats{});
    try {
    if (cfg_.streams.empty()) {
        Logger::error("Pipeline", "no stream configured");
        return false;
    }
    const size_t n = cfg_.streams.size();

    /* 真实硬件模式需要本构建带 RKNN/RGA 支持，提前给出明确错误 */
    if (cfg_.hardware) {
#if !defined(MSAD_ENABLE_RKNN)
        Logger::error("Pipeline", "config requests hardware=true but this build was compiled without "
                                  "MSAD_ENABLE_RKNN (rebuild with -DMSAD_ENABLE_RKNN=ON on RK3588)");
        return false;
#else
        Logger::info("Pipeline", "hardware mode: V4L2 capture + RGA preprocess + RKNN NPU inference");
#endif
    }

    std::error_code dirError;
    std::filesystem::create_directories(cfg_.screenshotDir, dirError);
    if (dirError || !std::filesystem::is_directory(cfg_.screenshotDir)) {
        Logger::error("Pipeline", "cannot create screenshot directory: " + cfg_.screenshotDir);
        return false;
    }

    /* 推理并发限制器（默认 2 路并发，预留 4 路扩展） */
    int inferPermits = cfg_.inferenceConcurrency;
    inferGate_ = std::make_unique<CountingSemaphore>(inferPermits);
    Logger::info("Pipeline", "inference concurrency gate = " + std::to_string(inferPermits));

    /* 告警上报器（全局一个连接） */
    instanceId_ = uniqueEventId();
    if (!previewOnly_) {
        if (cfg_.alarm.reliable) notifier_ = std::make_unique<DurableAlarmNotifier>(cfg_.alarm.outboxDir);
        else notifier_ = std::make_unique<TcpAlarmNotifier>();
    }

    /* 告警任务队列（供独立 IO 线程消费：写截图 + TCP 上报） */
    alarmTasks_ = std::make_unique<RingBuffer<AlarmTask>>(cfg_.alarmQueueCapacity);

    scenes_.resize(n);
    sources_.resize(n);
    processors_.resize(n);
    engines_.resize(n);
    regions_.resize(n);
    qCap2Pre_.resize(n);
    qPre2Inf_.resize(n);
    qInf2Alarm_.resize(n);
    stoppedStreams_.assign(n, false);
    activeStreams_ = n;
#if defined(MSAD_ENABLE_RKNN)
    std::shared_ptr<RknnModelContext> sharedModel;
#endif

    for (size_t i = 0; i < n; ++i) {
        qCap2Pre_[i] = std::make_unique<RingBuffer<Frame>>(cfg_.queueCapacity);
        qPre2Inf_[i] = std::make_unique<RingBuffer<Frame>>(cfg_.queueCapacity);
        qInf2Alarm_[i] = std::make_unique<RingBuffer<DetectResult>>(cfg_.queueCapacity);
#if defined(MSAD_ENABLE_RKNN)
        if (cfg_.hardware) {
            /*
             * RK3588 真实链路：
             *   摄像头(V4L2/YUYV) → RGA 等比缩放+转 RGB(640x640, letterbox) → RKNN NPU
             * 业务层（队列/警戒区/告警/截图）与 mock 模式完全一致，无需改动。
             */
            if(cfg_.streams[i].sourceUri.rfind("/dev/video",0)==0)
                sources_[i]=std::make_unique<V4L2CameraSource>(cfg_.captureWidth,cfg_.captureHeight);
            else sources_[i]=std::make_unique<RkMppSource>();
            if(cfg_.streams[i].sourceUri.rfind("rtsp://",0)==0 || cfg_.streams[i].sourceUri.rfind("rtsps://",0)==0)
                sources_[i]=std::make_unique<ReconnectingSource>(std::move(sources_[i]));
            processors_[i] = std::make_unique<RgaFrameProcessor>(cfg_.frameWidth, cfg_.frameHeight, cfg_.cameraFlip180,cfg_.dmaInput);
            if (!previewOnly_) {
                engines_[i] = std::make_unique<RknnInferenceEngine>(cfg_.modelPath, static_cast<float>(cfg_.confThresh),sharedModel,cfg_.nativeOutputs);
            }
        } else {
#else
        {
#endif
            /* 每路一个模拟场景（目标数 3 个，保证演示效果） */
            auto scene = std::make_shared<SimScene>(cfg_.frameWidth, cfg_.frameHeight, 3, 20260828u ^ static_cast<unsigned>(cfg_.streams[i].id));
            scenes_[i] = scene;

            /* 组装该路 HAL mock 组件 */
            sources_[i]    = std::make_unique<MockVideoSource>(scene, cfg_.fps);
            processors_[i] = std::make_unique<MockFrameProcessor>();
            engines_[i]    = std::make_unique<MockInferenceEngine>(scene);
        }

        const bool opened = sources_[i]->open(cfg_.streams[i].sourceUri);
        if (!opened || !processors_[i]->init() || (!previewOnly_ && !engines_[i]->init())) {
            Logger::error("Pipeline", "stream=" + std::to_string(cfg_.streams[i].id) +
                         (opened ? " init failed" : " open failed"));
            stoppedStreams_[i] = true;
            --activeStreams_;
            sources_[i]->close();
            sources_[i].reset();
            processors_[i].reset();
            engines_[i].reset();
            continue;
        }
#if defined(MSAD_ENABLE_RKNN)
        if (cfg_.hardware && !previewOnly_ && !sharedModel)
            sharedModel = static_cast<RknnInferenceEngine*>(engines_[i].get())->sharedModel();
#endif

        /* 该路警戒区域（配置 → 算法对象） */
        for (const auto& rc : cfg_.streams[i].regions) {
            PolygonRegion region(rc.id, rc.polygon);
            region.setClasses(rc.classes);
            region.setCooldownMs(rc.cooldownMs);
            regions_[i].push_back(std::move(region));
        }

        Logger::info("Pipeline", "stream " + std::to_string(i) + " ready, source=" +
                     sources_[i]->name() + ", regions=" +
                     std::to_string(regions_[i].size()));
    }

    if (activeStreams_ == 0) {
        Logger::error("Pipeline", "no stream could start");
        stop();
        return false;
    }
    const size_t startedStreams = activeStreams_;

    /* 连接尝试最多等待约 500ms；失败后由后续告警重试。 */
    if (notifier_ && !notifier_->connect(cfg_.alarm.host, cfg_.alarm.port)) {
        Logger::warn("Pipeline", "alarm server not reachable; not connected; reliable mode retries persisted alarms in the background");
    }

    /* 每路 4 个线程 + 1 个全局 IO 线程（截图/上报异步执行） */
    running_.store(true);
    startedAt_ = std::chrono::steady_clock::now();
    auto worker = [this](auto fn, int i) {
        try { (this->*fn)(i); }
        catch (const std::exception& e) { stopStream(i, e.what()); }
        catch (...) { stopStream(i, "worker exception"); }
    };
    for (size_t i = 0; i < n; ++i) {
        if (!sources_[i]) continue;
        threads_.emplace_back(worker, &VideoPipeline::captureLoop, static_cast<int>(i));
        threads_.emplace_back(worker, &VideoPipeline::preprocessLoop, static_cast<int>(i));
        threads_.emplace_back(worker, &VideoPipeline::inferLoop, static_cast<int>(i));
        threads_.emplace_back(worker, &VideoPipeline::alarmLoop, static_cast<int>(i));
    }
    ioThread_ = std::thread([this] {
        try { ioLoop(); }
        catch (const std::exception& e) { failed_.store(true); requestStop(); Logger::error("Pipeline", e.what()); }
        catch (...) { failed_.store(true); requestStop(); Logger::error("Pipeline", "io worker exception"); }
    });
    Logger::info("Pipeline", "pipeline started, streams=" + std::to_string(n) +
                 ", threads=" + std::to_string(startedStreams * 4 + 1));
    return true;
    } catch (const std::exception& e) {
        Logger::error("Pipeline", e.what());
        stop();
        return false;
    }
}

void VideoPipeline::requestStop() {
    running_.store(false);
    for (auto& source : sources_) if (source) source->requestStop();
    for (auto& q : qCap2Pre_) if (q) q->stop();
    for (auto& q : qPre2Inf_) if (q) q->stop();
    for (auto& q : qInf2Alarm_) if (q) q->stop();
    if (alarmTasks_) alarmTasks_->stop(cfg_.alarm.reliable);
    if (inferGate_) inferGate_->stop();
}

void VideoPipeline::stopStream(int idx, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(statsMtx_);
        if (!running_.load() || stoppedStreams_[idx]) return;
        stoppedStreams_[idx] = true;
    }
    Logger::error("Pipeline", "stream=" + std::to_string(cfg_.streams[idx].id) +
                 " stopped: " + reason);
    sources_[idx]->requestStop();
    qCap2Pre_[idx]->stop();
    qPre2Inf_[idx]->stop();
    qInf2Alarm_[idx]->stop();
    // Keep the shared notifier and inference gate alive for the remaining streams.
    bool last;
    {
        std::lock_guard<std::mutex> lock(statsMtx_);
        last = --activeStreams_ == 0;
    }
    if (last && running_.load()) {
        failed_.store(true);
        requestStop();
    }
}

void VideoPipeline::stop() {
    std::lock_guard<std::mutex> lock(stopMtx_);
    const bool hadThreads = !threads_.empty() || ioThread_.joinable();
    requestStop();

    /* join 所有流水线线程（幂等） */
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();

    /* join IO 线程：必须先于 notifier_->close()，避免在用的连接被关闭 */
    if (ioThread_.joinable()) ioThread_.join();

    /* 释放资源 */
    for (auto& s : sources_)  if (s) s->close();
    if (notifier_) notifier_->close();
    if (hadThreads && alarmTasks_) {
        Logger::info("AlarmQueue", "rejected_full=" + std::to_string(alarmTasks_->rejectedCount()) +
                     " cancelled_on_stop=" + std::to_string(alarmTasks_->cancelledCount()));
    }
    alarmTasks_.reset();

    if (hadThreads) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count();
        for (size_t i=0; i<stats_.size(); ++i) {
            Stats s;
            {
                std::lock_guard<std::mutex> statsLock(statsMtx_);
                s = stats_[i];
            }
            Logger::info("Stats", "stream=" + std::to_string(cfg_.streams[i].id) +
                " captured=" + std::to_string(s.captured) + " inferred=" + std::to_string(s.inferred) +
                " infer_fps=" + std::to_string(seconds > 0 ? static_cast<double>(s.inferred)/seconds : 0) +
                " dropped=" + std::to_string(qCap2Pre_[i]->droppedCount()+qPre2Inf_[i]->droppedCount()+qInf2Alarm_[i]->droppedCount()) +
                " alarms=" + std::to_string(s.alarms) + std::string(cfg_.alarm.reliable ? " persisted=" : " sent=") + std::to_string(s.sent));
        }
        Logger::info("Pipeline", "pipeline stopped, resources released");
    }
}

/* ================= 四个线程 ================= */

/* 采集线程：读帧 → 采集队列（满丢最旧，不阻塞） */
void VideoPipeline::captureLoop(int idx) {
    Logger::info("Pipeline", "capture thread start, stream=" + std::to_string(idx));
    const auto interval = std::chrono::nanoseconds(1000000000 / cfg_.fps);
    auto nextFrame = std::chrono::steady_clock::now();

    while (running_.load()) {
        Frame f;
        if (!sources_[idx]->readFrame(f)) {
            stopStream(idx, "readFrame failed/EOF");
            break;
        }
        f.streamId = cfg_.streams[idx].id;
        {
            std::lock_guard<std::mutex> statsLock(statsMtx_);
            ++stats_[idx].captured;
        }
        qCap2Pre_[idx]->push(std::move(f));   /* 满丢最旧，生产者永不被阻塞 */

        nextFrame += interval;
        if (nextFrame < std::chrono::steady_clock::now()) nextFrame = std::chrono::steady_clock::now();
        while (running_.load() && std::chrono::steady_clock::now() < nextFrame)
            std::this_thread::sleep_until(std::min(nextFrame, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
    }
    Logger::info("Pipeline", "capture thread exit, stream=" + std::to_string(idx));
}

/* 预处理线程：采集队列 → 预处理 → 预处理队列 */
void VideoPipeline::preprocessLoop(int idx) {
    Logger::info("Pipeline", "preprocess thread start, stream=" + std::to_string(idx));
    while (running_.load()) {
        Frame in;
        if (!qCap2Pre_[idx]->pop(in)) break;   /* 队列 stop 后返回 false */

        Frame out;
        if (!processors_[idx]->process(std::move(in), out)) {
            stopStream(idx, "preprocess failed");
            break;
        }
        qPre2Inf_[idx]->push(std::move(out));
    }
    Logger::info("Pipeline", "preprocess thread exit, stream=" + std::to_string(idx));
}

/* 推理线程：预处理队列 → 推理(受并发门限流) + 手写NMS → 推理队列 */
void VideoPipeline::inferLoop(int idx) {
    Logger::info("Pipeline", "infer thread start, stream=" + std::to_string(idx));
    while (running_.load()) {
        Frame f;
        if (!qPre2Inf_[idx]->pop(f)) break;

        std::vector<Detection> dets;
        int personCandidates = 0;
        if (!previewOnly_) {
        /* 并发门：限制同时推理的路数（默认 2） */
        if (!inferGate_->acquire()) break;
        bool ok;
        try { ok = engines_[idx]->infer(f, dets); }
        catch (...) { inferGate_->release(); throw; }
        inferGate_->release();
        if (!ok) {
            stopStream(idx, "inference failed");
            break;
        }

        /* 手写 NMS 抑制重叠框（阈值可配置，缺省 0.45） */
        personCandidates = static_cast<int>(std::count_if(dets.begin(), dets.end(),
            [](const Detection& d) { return d.classId == 0; }));
        dets = nms(dets, static_cast<float>(cfg_.nmsThresh));

        {
            std::lock_guard<std::mutex> statsLock(statsMtx_);
            ++stats_[idx].inferred;
        }
        }
        DetectResult res;
        res.timestampMs = f.timestampMs;
        res.streamId = f.streamId;
        res.frame = std::move(f);          /* 帧快照（告警截图用） */
        res.detections = std::move(dets);
        res.personCandidates = personCandidates;
        qInf2Alarm_[idx]->push(std::move(res));
    }
    Logger::info("Pipeline", "infer thread exit, stream=" + std::to_string(idx));
}

} // namespace msad
