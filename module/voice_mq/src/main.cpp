/*
 * main.cpp — 语音识别文本流转模拟程序入口
 *
 * 编译运行：
 *   cmake -B build && cmake --build build -j
 *   ./build/voice_mq_demo            # 运行直到 Ctrl+C
 *   ./build/voice_mq_demo 5          # 运行 5 秒后自动退出
 *
 * 信号处理：SIGINT/SIGTERM 置停止标志（异步信号安全），
 *           主线程唤醒 → stop() → join 线程 → 关闭队列，优雅退出。
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "VoiceMqSim.h"

namespace {

/*
 * 信号处理器：只置全局原子标志，绝不触碰对象指针（异步信号安全）。
 * 主线程循环轮询该标志后调用 sim.stop()，消除"信号处理器解引用
 * 悬空指针"的竞态（原实现 g_sim 指针与主线程 nullptr 赋值构成 UB）。
 */
volatile std::sig_atomic_t g_stopSignal = 0;

void onSignal(int /*sig*/) {
    g_stopSignal = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace msad;

    /* 运行秒数：默认 0=无限（Ctrl+C 退出） */
    int runSeconds = 0;
    if (argc > 1) runSeconds = std::atoi(argv[1]);

    VoiceMqSim sim;

    /* 注册信号：Ctrl+C / kill 优雅退出（处理器仅置原子标志） */
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!sim.start()) {
        std::fprintf(stderr, "start failed\n");
        return EXIT_FAILURE;
    }
    std::printf("[sys] running (queue_prefix=%s maxmsg=%zu msgsize=%zu)...\n",
                VoiceMqSim::kQueueName, VoiceMqSim::kMaxMsg, VoiceMqSim::kMsgSize);

    /* 定时退出：runSeconds>0 到时停止；否则等待信号后停止。
     * 轮询检查：信号标志 或 内部停止标志（stop() 由其他线程触发时） */
    if (runSeconds > 0) {
        for (int i = 0; i < runSeconds; ++i) {
            if (g_stopSignal || sim.g_stop.load()) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } else {
        while (!g_stopSignal && !sim.g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    sim.stop();       /* 优雅停止：join 线程 + 关闭队列（幂等，可并发安全调用） */

    std::printf("[sys] demo finished\n");
    return EXIT_SUCCESS;
}
