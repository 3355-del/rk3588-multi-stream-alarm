/*
 * MultiStreamEngine.h — 多路视频"解码 + 推理"业务模块（核心）
 *
 * 架构：每个视频源独立两条工作线程，线程间用线程安全队列解耦：
 *
 *   [解码线程 i]   IDecoder::decodeFrame  →  帧队列 i  →  [推理线程 i]
 *                                                        IAiInfer::infer
 *                                                        → 打印检测结果
 *
 * 特性：
 *   - 每源独立线程，互不阻塞；某一路解码慢/出错不影响其他路；
 *   - 队列满丢最旧帧（RingBuffer），生产者（解码线程）永不阻塞，防 OOM；
 *   - 优雅停止：停队列 → join 线程 → 释放资源，无死锁、无泄漏。
 *
 * 移植 RK3588：将本类中 mock 组件的构造处替换为
 *   RkMppDecoder / RknnInfer 即可，本类逻辑无需改动。
 */
#ifndef MODULE_MULTISTREAMENGINE_H
#define MODULE_MULTISTREAMENGINE_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "IDecoder.h"
#include "IAiInfer.h"
#include "msad/RingBuffer.h"
#include "msad/SimScene.h"

namespace msad {

class MultiStreamEngine {
public:
    /* 一路视频源配置 */
    struct StreamConfig {
        int id = 0;                /* 路号 */
        std::string uri;           /* 本地文件路径 或 rtsp:// 地址（mock 仅记录） */
        int frameWidth = 640;      /* 模拟帧宽 */
        int frameHeight = 480;     /* 模拟帧高 */
        int targetCount = 3;       /* 模拟场景目标数 */
        int fps = 25;              /* 解码帧率 */
    };

    MultiStreamEngine() = default;
    ~MultiStreamEngine();

    /* 禁止拷贝（持有线程与队列） */
    MultiStreamEngine(const MultiStreamEngine&) = delete;
    MultiStreamEngine& operator=(const MultiStreamEngine&) = delete;

    /*
     * 启动所有视频源的解码/推理线程。
     * @param cfgs           视频源配置（>=1 路）
     * @param queueCapacity  每路帧队列容量（满丢最旧）
     * @return true=成功
     */
    bool start(const std::vector<StreamConfig>& cfgs, size_t queueCapacity = 8);

    /* 阻塞等待直到 stop() 被调用（用于 main 演示） */
    void wait();

    /* 停止所有线程并释放资源（幂等） */
    void stop();
    bool isRunning() const { return running_.load(); }

private:
    /* 每源两条线程入口 */
    void decodeLoop(size_t idx);
    void inferLoop(size_t idx);

    std::atomic<bool> running_{false};
    std::mutex stopMtx_;

    /* 每路：共享模拟场景（解码渲染 + 推理读取，保证画面/结果自洽） */
    std::vector<std::shared_ptr<SimScene>> scenes_;
    std::vector<std::unique_ptr<IDecoder>> decoders_;
    std::vector<std::unique_ptr<IAiInfer>> infers_;
    std::vector<std::unique_ptr<RingBuffer<Frame>>> queues_;
    std::vector<StreamConfig> cfgs_;

    std::vector<std::thread> threads_;
};

} // namespace msad

#endif // MODULE_MULTISTREAMENGINE_H
