/*
 * Config.h — 系统配置结构与 JSON 配置加载
 *
 * 配置示例（demo_config.json）见工程根目录 config/ 下文件。
 * 解析失败时返回错误信息，便于排错。
 */
#ifndef MULTI_STREAM_CONFIG_H
#define MULTI_STREAM_CONFIG_H

#include <string>
#include <vector>

#include "msad/Types.h"

namespace msad {

/* 单个警戒区域配置 */
struct RegionConfig {
    int id = -1;
    std::vector<Point> polygon;   /* 多边形顶点（像素坐标） */
    std::vector<int> classes;     /* 关注的类别（空=全部） */
    unsigned cooldownMs = 5000;   /* 冷却时间(ms) */
};

/* 一路视频流配置 */
struct StreamConfig {
    int id = 0;
    std::string sourceUri;        /* 视频源 URI（mock 模式下可为任意串） */
    std::vector<RegionConfig> regions;  /* 该路警戒区域 */
};

/* 告警上报配置 */
struct AlarmConfig {
    std::string host = "127.0.0.1";
    int port = 9000;
    bool reliable = false; // Legacy protocol remains available.
    std::string outboxDir = "alarm_outbox";
};

/* 系统总配置 */
struct AppConfig {
    int frameWidth = 1280;        /* 模拟帧宽（真实模式=预处理/推理帧宽，也是警戒区与截图坐标系） */
    int frameHeight = 720;        /* 模拟帧高（同上） */
    int queueCapacity = 8;        /* 每级流水线队列容量（满丢最旧） */
    int alarmQueueCapacity = 8;   // Independent IO task queue; reject new tasks when full.
    int fps = 30;                 /* 目标帧率 */
    int runSeconds = 30;          /* 演示运行时长（秒），0=无限 */
    std::string screenshotDir = "screenshots";  /* 截图目录（相对运行目录） */
    AlarmConfig alarm;
    std::vector<StreamConfig> streams;

    /*
     * ---- RK3588 真实硬件模式（可选） ----
     * hardware=false（默认）：全部使用 mock 组件，任何平台可跑。
     * hardware=true ：采集=V4L2 摄像头、预处理=RGA 硬件缩放/转换、推理=RKNN NPU。
     *   需要以 -DMSAD_ENABLE_RKNN=ON 构建（aarch64 自动开启），并链接 -lrknnrt -lrga。
     */
    bool hardware = false;
    std::string modelPath;        /* RKNN 模型路径（hardware=true 时必填） */
    int captureWidth = 640;       /* 摄像头采集宽（仅 hardware=true 使用） */
    int captureHeight = 480;      /* 摄像头采集高（仅 hardware=true 使用） */
    double confThresh = 0.25;     /* 推理置信度阈值（hardware=true 生效；mock 不使用） */
    double nmsThresh = 0.45;      /* NMS IoU 阈值（流水线统一使用） */
    bool cameraFlip180 = false;   /* 摄像头倒装时旋转 180°（仅 hardware=true） */
    int inferenceConcurrency = 2; // 1..2 active NPU submissions; 1..4 source streams.
    bool dmaInput = true;
    bool nativeOutputs = true;    // false retains want_float=1 for paired benchmarks.

    /*
     * 从 JSON 文件加载配置。
     * @return true=成功；false=失败（err 带原因）
     */
    static bool load(const std::string& jsonPath, AppConfig& out, std::string& err);
    bool validate(std::string& err) const;
    uint64_t estimatedBufferBytes() const;
};

} // namespace msad

#endif // MULTI_STREAM_CONFIG_H
