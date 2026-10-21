/*
 * VoiceMqSim.h — 语音识别文本流转模拟程序（C++17 / x86 Ubuntu）
 *
 * 功能：
 *   1. 两个线程：sim_recv_thread（生产）、sim_display_thread（消费）
 *   2. POSIX 消息队列完成线程间字符串传递
 *   3. sim_recv_thread 定时生成模拟语音识别文本（500ms），发送入队
 *   4. sim_display_thread 阻塞读取消息队列，打印输出模拟显示
 *
 * 约束：
 *   - 单条消息最大 64 字节，队列最多缓存 8 条
 *   - 纯用户态，标准 POSIX 接口，无 RK 硬件依赖
 *
 * 资源与信号处理：
 *   - RAII 封装：析构时关闭并 unlink 消息队列，进程退出后无残留
 *   - 信号退出：SIGINT/SIGTERM 置停止标志，接收线程用带超时的
 *     mq_timedreceive 周期唤醒检查标志，实现线程安全优雅退出
 */
#ifndef MODULE_VOICEMQSIM_H
#define MODULE_VOICEMQSIM_H

#include <atomic>
#include <mutex>
#include <cstddef>
#include <string>
#include <thread>

#include <mqueue.h>

namespace msad {

class VoiceMqSim {
public:
    /* 队列参数（需求约束） */
    static constexpr const char* kQueueName = "/sim_voice_mq";
    static constexpr size_t kMaxMsg = 8;     /* 队列最大 8 条 */
    static constexpr size_t kMsgSize = 64;   /* 单条最大 64 字节 */
    static constexpr int kSendIntervalMs = 500;  /* 发送间隔 500ms */

    VoiceMqSim() = default;
    ~VoiceMqSim();

    /* 禁止拷贝 */
    VoiceMqSim(const VoiceMqSim&) = delete;
    VoiceMqSim& operator=(const VoiceMqSim&) = delete;

    /* 创建消息队列并启动两个线程；返回 true=成功 */
    bool start();

    /* 阻塞等待直到停止标志被置位（由信号处理或外部调用 stop() 触发） */
    void wait();

    /* 置停止标志并等待线程退出、清理队列（幂等） */
    void stop();

    /* 停止标志（信号处理器可置位；由外部信号触发或 stop() 置位） */
    std::atomic<bool> g_stop{false};

private:
    /* 生产线程：定时生成模拟识别文本 */
    void recvThread();
    /* 消费线程：阻塞读取并打印 */
    void displayThread();

    mqd_t mq_ = static_cast<mqd_t>(-1);   /* 消息队列句柄 */
    std::atomic<bool> started_{false};    /* 是否已启动（原子，防并发竞态） */
    std::mutex lifecycleMtx_;
    std::atomic<bool> stopping_{false};   /* stop() 只执行一次（原子交换） */
    std::thread recvThread_;    /* 生产线程句柄（stop 时 join） */
    std::thread displayThread_; /* 消费线程句柄（stop 时 join） */
};

} // namespace msad

#endif // MODULE_VOICEMQSIM_H
