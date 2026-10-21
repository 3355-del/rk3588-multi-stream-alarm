/*
 * VoiceMqSim.cpp — 语音识别文本流转模拟实现
 */
#include "VoiceMqSim.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace msad {

VoiceMqSim::~VoiceMqSim() {
    stop();
}

bool VoiceMqSim::start() {
    std::lock_guard<std::mutex> lock(lifecycleMtx_);
    if (started_.load()) return false;
    g_stop.store(false);
    stopping_.store(false);

    /* 1. 配置队列属性：最多 8 条，每条最大 64 字节 */
    struct mq_attr attr{};
    attr.mq_maxmsg = static_cast<long>(kMaxMsg);
    attr.mq_msgsize = static_cast<long>(kMsgSize);

    /* 2. 创建/打开命名消息队列（不存在则创建） */
    static std::atomic<unsigned> sequence{0};
    const std::string name = std::string(kQueueName) + "_" + std::to_string(getpid()) + "_" + std::to_string(sequence.fetch_add(1));
    mq_ = mq_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    if (mq_ == static_cast<mqd_t>(-1)) {
        std::perror("mq_open");
        return false;
    }

    /* 3. 打开成功后立即从命名空间删除（unlink）：句柄仍有效，
     *    进程退出后队列自动清理，避免 /dev/mqueue 残留 */
    if (mq_unlink(name.c_str()) != 0) {
        std::perror("mq_unlink");
        mq_close(mq_); mq_ = static_cast<mqd_t>(-1); return false;
    }

    /* 4. 启动两个线程（保存句柄，stop() 时 join 保证线程安全退出）。
     *    注意：若线程构造抛异常（资源耗尽），started_ 已为 true，
     *    析构/stop() 会通过 joinable() 正确兜底清理已启动的线程。 */
    started_.store(true);
    try {
        recvThread_ = std::thread([this] { recvThread(); });
        displayThread_ = std::thread([this] { displayThread(); });
    } catch (...) {
        g_stop.store(true);
        if (recvThread_.joinable()) recvThread_.join();
        if (displayThread_.joinable()) displayThread_.join();
        started_.store(false);
        mq_close(mq_);
        mq_ = static_cast<mqd_t>(-1);
        std::fprintf(stderr, "thread creation failed\n");
        return false;
    }
    return true;
}

void VoiceMqSim::recvThread() {
    unsigned int seq = 0;
    char buf[kMsgSize];

    while (!g_stop.load()) {
        /* 生成模拟识别文本（序号递增，便于验证顺序/丢包） */
        std::snprintf(buf, sizeof(buf), "识别结果[%03u]: 你好，RK3588", seq);

        /* 发送：整条字符串（含 '\0'，共 strlen+1 字节，<= 64） */
        ssize_t len = static_cast<ssize_t>(std::strlen(buf) + 1);
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
        if (mq_timedsend(mq_, buf, static_cast<size_t>(len), 0, &deadline) == -1) {
            /* 超时或中断：丢弃本条，下轮检查停止标志，避免退出阻塞。 */
            std::fprintf(stderr, "[recv] mq_send failed: %s\n", std::strerror(errno));
        } else {
            std::printf("[recv] -> %s\n", buf);
            std::fflush(stdout);
            ++seq;
        }

        /* 定时 500ms，分段休眠以便及时响应停止标志 */
        for (int i = 0; i < kSendIntervalMs / 100 && !g_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::printf("[recv] thread exit\n");
    std::fflush(stdout);
}

void VoiceMqSim::displayThread() {
    char buf[kMsgSize];

    while (!g_stop.load()) {
        std::memset(buf, 0, sizeof(buf));

        /* 阻塞读取，但用超时版本周期唤醒检查停止标志（线程安全退出） */
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000 * 1000;   /* +100ms */
        if (ts.tv_nsec >= 1000 * 1000 * 1000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000 * 1000 * 1000;
        }

        ssize_t n = mq_timedreceive(mq_, buf, sizeof(buf), nullptr, &ts);
        if (n < 0) {
            if (errno == ETIMEDOUT) continue;   /* 超时：继续检查停止标志 */
            if (errno == EINTR) continue;       /* 被信号打断：继续 */
            std::fprintf(stderr, "[disp] mq_receive failed: %s\n", std::strerror(errno));
            g_stop.store(true);
            break;
        }

        /* 模拟屏幕显示 */
        std::printf("[display] %.*s\n", static_cast<int>(n), buf);
        std::fflush(stdout);
    }
    std::printf("[disp] thread exit\n");
    std::fflush(stdout);
}

void VoiceMqSim::wait() {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void VoiceMqSim::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMtx_);
    if (!started_.load()) return;

    /* 原子交换保证 stop() 只真正执行一次（并发调用安全，防双重 join） */
    if (stopping_.exchange(true)) return;

    /* 置停止标志：接收线程最迟 500ms 内、显示线程最迟 100ms 内退出 */
    g_stop.store(true);

    /* 两个线程观察停止标志后退出；停止时不保证排空待消费消息。 */
    if (recvThread_.joinable()) recvThread_.join();
    if (displayThread_.joinable()) displayThread_.join();

    /* 关闭队列句柄（unlink 在 start() 已做，进程退出后无残留） */
    if (mq_ != static_cast<mqd_t>(-1)) {
        mq_close(mq_);
        mq_ = static_cast<mqd_t>(-1);
    }
    started_.store(false);
    std::printf("[sys] threads joined, message queue closed\n");
    std::fflush(stdout);
}

} // namespace msad
