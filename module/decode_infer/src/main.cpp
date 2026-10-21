/*
 * main.cpp — 多路"解码 + 推理"模块测试示例
 *
 * 功能：开启 2 路模拟视频源（本地文件 URI / RTSP 地址仅作标识），
 *       每路独立"解码线程 + 推理线程"，打印模拟检测结果。
 *
 * 用法：
 *   ./decode_infer_demo [运行秒数]     （默认 15 秒）
 *   Ctrl+C 提前退出
 */
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "MultiStreamEngine.h"
#include "msad/Logger.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int /*sig*/) {
    g_stop = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace msad;

    Logger::instance().setLevel(LogLevel::INFO);

    /* 运行时长：默认 15 秒 */
    int runSeconds = 15;
    if (argc > 1) runSeconds = std::atoi(argv[1]);

    /* 1. 配置 2 路模拟视频源：
     *    路0：模拟"本地视频文件"（URI 为文件路径）
     *    路1：模拟"RTSP 网络流"（URI 为 rtsp:// 地址）
     *    mock 下 URI 仅记录，不真正访问。 */
    std::vector<MultiStreamEngine::StreamConfig> cfgs = {
        {0, "/data/videos/stream0.mp4", 640, 480, 3, 25},   /* 本地文件 */
        {1, "rtsp://YOUR_BOARD_IP:554/ch0", 640, 480, 2, 15} /* RTSP 流 */
    };

    /* 2. 启动多路引擎（每源 2 线程：解码 + 推理，队列容量 8 满丢最旧） */
    MultiStreamEngine engine;
    if (!engine.start(cfgs, 8)) {
        Logger::error("Main", "engine start failed");
        return EXIT_FAILURE;
    }

    /* 3. 注册信号（Ctrl+C 优雅退出） */
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    /* 4. 运行指定秒数或直到信号 */
    Logger::info("Main", "running " + std::to_string(runSeconds) +
                 "s, press Ctrl+C to stop early...");
    for (int i = 0; i < runSeconds; ++i) {
        if (g_stop || !engine.isRunning()) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (g_stop) Logger::info("Main", "signal received, stopping...");

    /* 5. 优雅停止 */
    engine.stop();
    Logger::info("Main", "demo finished");
    return EXIT_SUCCESS;
}
