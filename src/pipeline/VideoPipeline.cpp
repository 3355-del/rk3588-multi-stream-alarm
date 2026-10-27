/*
 * VideoPipeline.cpp — 四线程流水线实现
 */
#include "msad/VideoPipeline.h"
#include "msad/ReconnectingSource.h"

#include <chrono>
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <exception>
#include <algorithm>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

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
      observer_(std::move(observer)), previewOnly_(previewOnly) {}

VideoPipeline::~VideoPipeline() {
    stop();
}

uint64_t VideoPipeline::nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
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
    lastAlarmMs_.clear();
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

    for (size_t i = 0; i < n; ++i) {
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
                auto sharedModel=i?static_cast<RknnInferenceEngine*>(engines_[0].get())->sharedModel():std::shared_ptr<RknnModelContext>{};
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

        if (!sources_[i]->open(cfg_.streams[i].sourceUri)) {
            Logger::error("Pipeline", "stream " + std::to_string(i) + " open failed");
            stop(); return false;
        }
        if (!processors_[i]->init() || (!previewOnly_ && !engines_[i]->init())) {
            Logger::error("Pipeline", "stream " + std::to_string(i) + " init failed");
            stop(); return false;
        }

        /* 该路警戒区域（配置 → 算法对象） */
        for (const auto& rc : cfg_.streams[i].regions) {
            PolygonRegion region(rc.id, rc.polygon);
            region.setClasses(rc.classes);
            region.setCooldownMs(rc.cooldownMs);
            regions_[i].push_back(std::move(region));
        }

        /* 三级队列，容量=queue_capacity（满丢最旧） */
        qCap2Pre_[i]  = std::make_unique<RingBuffer<Frame>>(cfg_.queueCapacity);
        qPre2Inf_[i]  = std::make_unique<RingBuffer<Frame>>(cfg_.queueCapacity);
        qInf2Alarm_[i] = std::make_unique<RingBuffer<DetectResult>>(cfg_.queueCapacity);

        Logger::info("Pipeline", "stream " + std::to_string(i) + " ready, source=" +
                     sources_[i]->name() + ", regions=" +
                     std::to_string(regions_[i].size()));
    }

    /* 连接尝试最多等待约 500ms；失败后由后续告警重试。 */
    if (notifier_ && !notifier_->connect(cfg_.alarm.host, cfg_.alarm.port)) {
        Logger::warn("Pipeline", "alarm server not reachable; not connected; reliable mode retries persisted alarms in the background");
    }

    /* 每路 4 个线程 + 1 个全局 IO 线程（截图/上报异步执行） */
    running_.store(true);
    startedAt_ = std::chrono::steady_clock::now();
    auto worker = [this](auto fn, int i) {
        try { (this->*fn)(i); }
        catch (const std::exception& e) { failed_.store(true); Logger::error("Pipeline", e.what()); requestStop(); }
        catch (...) { failed_.store(true); Logger::error("Pipeline", "worker exception"); requestStop(); }
    };
    for (size_t i = 0; i < n; ++i) {
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
                 ", threads=" + std::to_string(n * 4 + 1));
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
    if (alarmTasks_) alarmTasks_->stop(cfg_.alarm.reliable && !failed_.load()); // Drain accepted tasks on normal reliable shutdown.
    if (inferGate_) inferGate_->stop();
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
            if(!running_.load())break;
            failed_.store(true);
            Logger::warn("Pipeline", "stream " + std::to_string(idx) + " readFrame failed/EOF");
            requestStop();
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
            Logger::error("Pipeline", "preprocess failed, stream=" + std::to_string(idx));
            failed_.store(true);requestStop();break;
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
        if (!previewOnly_) {
        /* 并发门：限制同时推理的路数（默认 2） */
        if (!inferGate_->acquire()) break;
        bool ok;
        try { ok = engines_[idx]->infer(f, dets); }
        catch (...) { inferGate_->release(); throw; }
        inferGate_->release();
        if (!ok) {failed_.store(true);Logger::error("Pipeline","inference failed, stream="+std::to_string(idx));requestStop();break;}

        /* 手写 NMS 抑制重叠框（阈值可配置，缺省 0.45） */
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
        qInf2Alarm_[idx]->push(std::move(res));
    }
    Logger::info("Pipeline", "infer thread exit, stream=" + std::to_string(idx));
}

/* 告警线程：消费检测结果 → 警戒区判断 → 冷却去重 → 截图 + TCP 上报 */
void VideoPipeline::alarmLoop(int idx) {
    Logger::info("Pipeline", "alarm thread start, stream=" + std::to_string(idx));
    while (running_.load()) {
        DetectResult res;
        if (!qInf2Alarm_[idx]->pop(res)) break;
        if (observer_) observer_(res); // Before alarm handling can move the frame into a screenshot task.
        if (!previewOnly_) checkAndRaiseAlarm(idx, res);
    }
    Logger::info("Pipeline", "alarm thread exit, stream=" + std::to_string(idx));
}

void VideoPipeline::checkAndRaiseAlarm(int idx, DetectResult& res) {
    /* 该路没有警戒区域 → 无告警逻辑 */
    if (regions_[idx].empty()) return;

    std::shared_ptr<const Frame> snapshot;
    for (const auto& region : regions_[idx]) {
        for (const auto& det : res.detections) {
            /* 1. 类别过滤 */
            if (!region.caresAbout(det.classId)) continue;
            /* 2. 闯入判断：目标中心落入多边形 */
            if (!region.containsCenter(det.box)) continue;

            /* 3. 冷却去重：同一(区域,类别)在冷却期内不重复告警 */
            auto key = std::make_tuple(idx, region.id(), det.classId);
            uint64_t now = nowMs();
            const auto cooldownNow = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            {
                std::lock_guard<std::mutex> lock(cooldownMtx_);
                auto it = lastAlarmMs_.find(key);
                if (it != lastAlarmMs_.end() &&
                    cooldownNow - it->second < region.cooldownMs()) {
                    continue;   /* 冷却期内，跳过 */
                }
                // Commit cooldown only after the queue accepts this task.
            }

            /* 4. 触发告警：构造任务投递给 IO 线程（本线程不做磁盘/网络 IO，
             *    避免慢速写盘阻塞告警线程、进而反压整条流水线）；
             *    队列满时拒绝新任务且不更新冷却，下个检测帧可再次尝试。 */
            if (!snapshot) snapshot = std::make_shared<const Frame>(std::move(res.frame));
            const uint64_t sequence = alarmSequence_.fetch_add(1);
            std::string name = "alarm_s" + std::to_string(cfg_.streams[idx].id) +
                "_r" + std::to_string(region.id()) + "_c" + std::to_string(det.classId) +
                "_" + std::to_string(now) + "_f" + std::to_string(snapshot->frameIndex) +
                "_a" + std::to_string(sequence) + "_" + instanceId_ + ".png";

            AlarmTask task;
            task.frame = snapshot;                 /* 帧快照（截图用） */
            task.boxes.push_back(det);              /* 截图上画该目标框 */
            task.shotPath = cfg_.screenshotDir + "/" + name;
            task.info.eventId = instanceId_ + "-" + std::to_string(sequence);
            task.info.timestampMs = now;
            task.info.streamId = res.streamId;
            task.info.regionId = region.id();
            task.info.classId = det.classId;
            task.info.confidence = det.confidence;
            task.info.centerX = det.box.center().x;
            task.info.centerY = det.box.center().y;
            task.info.screenshotFile.clear();        /* 由 IO 线程写盘成功后回填 */

            if (!alarmTasks_) return;
            const auto result = alarmTasks_->tryPush(std::move(task));
            if (result == RingBuffer<AlarmTask>::PushResult::Stopped) return;
            if (result == RingBuffer<AlarmTask>::PushResult::Full) continue;
            {
                std::lock_guard<std::mutex> lock(cooldownMtx_);
                lastAlarmMs_[key] = cooldownNow;
            }
            {
                std::lock_guard<std::mutex> statsLock(statsMtx_);
                ++stats_[idx].alarms; // Accepted tasks only; rejected tasks do not enter cooldown.
            }
        }
    }
}

/*
 * IO 线程：串行消费告警任务 → 写截图 PNG → TCP 上报。
 * 与告警线程解耦：即使写盘/网络变慢，也只影响本线程与新任务的排队，
 * 不会阻塞警戒区判定与整条推理流水线。
 */
void VideoPipeline::ioLoop() {
    Logger::info("Pipeline", "io thread start (screenshot + tcp report)");
    while (true) {
        AlarmTask task;
        if (!alarmTasks_ || !alarmTasks_->pop(task)) break;

        /* 1. 写截图（失败则不上报文件名，避免接收端取到不存在的文件） */
        const auto freeBytes = std::filesystem::space(cfg_.screenshotDir).available;
        const uint64_t reserve = 256ULL*1024*1024 + static_cast<uint64_t>(cfg_.frameWidth)*static_cast<uint64_t>(cfg_.frameHeight)*4;
        if (freeBytes < reserve) throw std::runtime_error("screenshot disk space below reserve; stopping before disk fills");
        const bool saved = screenshotWriter_(*task.frame, task.shotPath, task.boxes);
        AlarmInfo info = task.info;
        info.screenshotFile = saved ? std::filesystem::path(task.shotPath).filename().string() : "";

        /* 2. TCP 上报（TcpAlarmNotifier 内部保证断线重连与发送互斥） */
        const bool sent = notifier_ ? notifier_->sendAlarm(info) : false;
        if (sent) {
            const int sid = info.streamId;
            std::lock_guard<std::mutex> statsLock(statsMtx_);
            for (size_t i = 0; i < cfg_.streams.size(); ++i) {
                if (cfg_.streams[i].id == sid) { ++stats_[i].sent; break; }
            }
        }

        Logger::info("Pipeline",
                     "ALARM stream=" + std::to_string(info.streamId) +
                     " region=" + std::to_string(info.regionId) +
                     " class=" + std::string(cocoClassName(info.classId)) +
                     " conf=" + std::to_string(info.confidence) +
                     std::string(cfg_.alarm.reliable ? " persisted=" : " tcp_sent=") + (sent ? "yes" : "no") +
                     " shot=" + (saved ? task.shotPath : "FAILED"));
    }
    Logger::info("Pipeline", "io thread exit");
}

} // namespace msad
