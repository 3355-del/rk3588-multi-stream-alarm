/*
 * VideoPipeline.h — 多路视频四线程流水线（核心架构）
 *
 * 每路视频独立运行四条线程：
 *   [采集线程]   IVideoSource::readFrame   → 采集队列
 *   [预处理线程] IFrameProcessor::process  → 预处理队列
 *   [推理线程]   IInferenceEngine::infer + 手写 NMS → 推理队列
 *   [告警线程]   警戒区判断 + 冷却去重 → 截图 + TCP 上报
 *
 * 队列：RingBuffer<Frame> 等（满丢最旧，生产者不阻塞，防 OOM）。
 *
 * 并发控制：全局 CountingSemaphore 限制同时进行的推理路数，
 *   当前模拟默认值为 2，四路流时为 4；与真实 NPU 核数无绑定。
 *
 * 线程安全：所有共享状态（冷却表）由互斥锁保护；析构时先停队列唤醒
 *   阻塞线程，再 join，保证无死锁、无泄漏。
 */
#ifndef MULTI_STREAM_VIDEOPIPELINE_H
#define MULTI_STREAM_VIDEOPIPELINE_H

#include <atomic>
#include <functional>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include "msad/Config.h"
#include "msad/Hal.h"
#include "msad/PolygonRegion.h"
#include "msad/RingBuffer.h"
#include "msad/SimScene.h"

namespace msad {

class VideoPipeline {
public:
    using ScreenshotWriter = std::function<bool(const Frame&, const std::string&, const std::vector<Detection>&)>;
    using ResultObserver = std::function<void(const DetectResult&)>;
    explicit VideoPipeline(const AppConfig& cfg, ScreenshotWriter writer = {},
                           ResultObserver observer = {}, bool previewOnly = false);
    ~VideoPipeline();

    /* 创建所有线程并启动流水线 */
    bool start();

    /* 停止流水线：停队列 → join 全部线程 → 释放资源（幂等） */
    void stop();
    bool isRunning() const { return running_.load(); }
    bool failed() const { return failed_.load(); }

private:
    friend struct VideoPipelineTestAccess;
    std::string instanceId_;
    /* ---- 四个线程入口（每路各一组） ---- */
    void requestStop();
    void captureLoop(int idx);
    void preprocessLoop(int idx);
    void inferLoop(int idx);
    void alarmLoop(int idx);

    /*
     * 告警派发任务：告警线程只做判定与任务投递，
     * 真正的截图写盘 + TCP 上报由独立的 IO 线程串行执行，
     * 避免慢速磁盘/网络阻塞告警线程（进而阻塞整条流水线）。
     */
    struct AlarmTask {
        std::shared_ptr<const Frame> frame;                        /* 触发告警的这一帧（截图用） */
        std::vector<Detection> boxes;       /* 需在截图上标注的目标框 */
        AlarmInfo info;                     /* 告警内容（截图文件名由 IO 线程回填） */
        std::string shotPath;               /* 截图完整路径 */
    };

    /* IO 线程：从告警任务队列取任务 → 写截图 → TCP 上报 */
    void ioLoop();

    /* 警戒区判断与告警触发（只投递任务，不做阻塞 IO） */
    void checkAndRaiseAlarm(int idx, DetectResult& res);

    /* 简单计数信号量：限制推理并发路数 */
    class CountingSemaphore {
    public:
        explicit CountingSemaphore(int permits) : permits_(permits) {}
        bool acquire();
        void stop();
        void release();
    private:
        std::mutex mtx_;
        std::condition_variable cv_;
        int permits_;
        bool stopped_ = false;
    };

    /* 当前毫秒时间戳（epoch） */
    static uint64_t nowMs();

    AppConfig cfg_;
    ScreenshotWriter screenshotWriter_;
    ResultObserver observer_;
    bool previewOnly_ = false;
    std::atomic<uint64_t> alarmSequence_{0}; // Not reset between starts of this instance.
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};

    /* HAL 组件（每路一套） */
    std::vector<std::shared_ptr<SimScene>> scenes_;
    std::vector<std::unique_ptr<IVideoSource>> sources_;
    std::vector<std::unique_ptr<IFrameProcessor>> processors_;
    std::vector<std::unique_ptr<IInferenceEngine>> engines_;
    std::unique_ptr<IAlarmNotifier> notifier_;   /* 全局一个告警连接 */

    /* 每路三级流水线队列 */
    std::vector<std::unique_ptr<RingBuffer<Frame>>> qCap2Pre_;
    std::vector<std::unique_ptr<RingBuffer<Frame>>> qPre2Inf_;
    std::vector<std::unique_ptr<RingBuffer<DetectResult>>> qInf2Alarm_;

    /* 每路警戒区域 */
    std::vector<std::vector<PolygonRegion>> regions_;

    /* 告警冷却表：key=(streamId, regionId, classId) → 上次告警时间(ms) */
    std::mutex cooldownMtx_;
    std::map<std::tuple<int, int, int>, uint64_t> lastAlarmMs_;

    /* 推理并发限制器（默认 2 路并发，预留 4 路扩展） */
    std::unique_ptr<CountingSemaphore> inferGate_;

    /* 告警任务队列 + 独立 IO 线程（截图写盘 / TCP 上报） */
    std::unique_ptr<RingBuffer<AlarmTask>> alarmTasks_;
    std::thread ioThread_;

    std::vector<std::thread> threads_;
    std::mutex stopMtx_;
    struct Stats { uint64_t captured=0, inferred=0, alarms=0, sent=0; };
    std::vector<Stats> stats_;
    mutable std::mutex statsMtx_;   /* 保护 stats_（告警线程与 IO 线程并发更新） */
    std::chrono::steady_clock::time_point startedAt_;
};

} // namespace msad

#endif // MULTI_STREAM_VIDEOPIPELINE_H
