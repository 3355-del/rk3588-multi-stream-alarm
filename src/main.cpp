/*
 * main.cpp — x86_64 本地演示入口
 *
 * 功能：加载 JSON 配置 → 启动多路四线程流水线（全 mock 组件）→
 *       运行指定时长（或 Ctrl+C 提前结束）→ 优雅停止。
 *
 * 用法：
 *   ./multi_stream_alarm_demo [config.json]
 *   默认配置路径：config/demo_config.json
 *
 * 提前结束：终端 Ctrl+C（SIGINT），触发优雅停止。
 */
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include "msad/Config.h"
#include "msad/Logger.h"
#include "msad/VideoPipeline.h"
#include "msad/PreviewPublisher.h"
#include <iostream>
#include <memory>

namespace {

/* 全局停止标志：SIGINT/SIGTERM 时置位，主线程据此退出 */
volatile std::sig_atomic_t g_stopRequested = 0;

void onSignal(int /*sig*/) {
    g_stopRequested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace msad;

    Logger::instance().setLevel(LogLevel::DEBUG);

    /* 1. 解析配置路径 */
    std::string configPath = "config/demo_config.json";
    std::string previewDirectory;
    int previewFps = 3;
    bool previewOnly = false;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--preview-only") previewOnly = true;
            else if (arg == "--preview-dir" && i + 1 < argc) previewDirectory = argv[++i];
            else if (arg == "--preview-fps" && i + 1 < argc) previewFps = std::stoi(argv[++i]);
            else if (arg == "--help") {
                std::cout << "Usage: multi_stream_alarm_live [config.json] [--preview-dir DIR] [--preview-fps 1..5] [--preview-only]\n";
                return 0;
            } else if (arg.rfind("--", 0) == 0) throw std::runtime_error("unknown or incomplete option: " + arg);
            else configPath = arg;
        }
        if (previewFps < 1 || previewFps > 5) throw std::runtime_error("preview fps must be 1..5");
    } catch (const std::exception& error) { Logger::error("Main", error.what()); return EXIT_FAILURE; }

    /* 2. 加载配置 */
    AppConfig cfg;
    std::string err;
    if (!AppConfig::load(configPath, cfg, err)) {
        Logger::error("Main", "load config failed: " + err);
        return EXIT_FAILURE;
    }
    Logger::info("Main", "config loaded: " + configPath +
                 ", streams=" + std::to_string(cfg.streams.size()) +
                 ", queue_capacity=" + std::to_string(cfg.queueCapacity) +
                 ", run_seconds=" + std::to_string(cfg.runSeconds));

    /* 3. 注册信号处理（Ctrl+C 优雅退出） */
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    /* 4. 构建并启动流水线 */
    std::unique_ptr<PreviewPublisher> preview;
    try {
        if (!previewDirectory.empty()) preview = std::make_unique<PreviewPublisher>(previewDirectory, previewFps);
    } catch (const std::exception& error) { Logger::error("Preview", error.what()); return EXIT_FAILURE; }
    VideoPipeline pipeline(cfg, {}, preview ? VideoPipeline::ResultObserver([&](const DetectResult& result) {
        preview->submit(result);
    }) : VideoPipeline::ResultObserver{}, previewOnly);
    if (!pipeline.start()) {
        Logger::error("Main", "pipeline start failed");
        return EXIT_FAILURE;
    }

    /* 5. 运行：配置时长到期或收到信号时退出 */
    Logger::info("Main", "running... press Ctrl+C to stop early");
    const int runSeconds = cfg.runSeconds;
    const auto start = std::chrono::steady_clock::now();
    while (!g_stopRequested && pipeline.isRunning()) {
        if (runSeconds > 0 && std::chrono::steady_clock::now() - start >=
            std::chrono::seconds(runSeconds)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_stopRequested) {
        Logger::info("Main", "signal received, stopping...");
    }

    /* 6. 优雅停止并释放资源 */
    pipeline.stop();
    if (preview) preview->stop();
    Logger::info("Main", "demo finished");
    return pipeline.failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
