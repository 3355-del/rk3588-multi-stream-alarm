/*
 * Hal.h — 硬件抽象层（HAL）：四类硬件/板卡能力接口
 *
 * 设计目标：
 *   - 业务流水线（VideoPipeline）只依赖这四个抽象接口，不感知具体硬件；
 *   - 当前提供模拟桩(Mock)实现，x86_64 上直接编译运行、返回正常模拟数据；
 *   - 移植到 RK3588 时：继承接口实现子类即可，例如：
 *       RkMppVideoSource   : IVideoSource   （MPP 硬解 RTSP/本地文件）
 *       RgaFrameProcessor  : IFrameProcessor（RGA 缩放/格式转换，DMA-BUF 零拷贝）
 *       RknnInferenceEngine: IInferenceEngine（rknn_api 加载 YOLOv8n INT8 .rknn）
 *       TcpAlarmNotifier   : IAlarmNotifier （告警 TCP 上报，无需改动）
 *   - 所有接口方法均为纯虚函数；析构函数为虚函数保证子类正确释放。
 *
 * 注意：本文件禁止包含任何板卡专属头文件（rknn_api.h / mpp / rga 等）。
 */
#ifndef MULTI_STREAM_HAL_H
#define MULTI_STREAM_HAL_H

#include <string>
#include <vector>

#include "msad/Types.h"

namespace msad {

/*
 * IVideoSource — 视频采集源抽象
 * mock 实现：MockVideoSource（程序生成运动测试帧）
 * RK3588 实现：MPP 解码 RTSP/本地文件 / V4L2 摄像头
 */
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    /* 打开视频源（URI：rtsp://… /dev/video0 / 本地文件路径） */
    virtual bool open(const std::string& uri) = 0;

    /*
     * 读取一帧。
     * @param frame 输出帧（YUV420P）
     * @return true=成功读到一帧；false=流结束/出错
     */
    virtual bool readFrame(Frame& frame) = 0;

    // Called from the controller thread; must not destroy resources in use.
    virtual void requestStop() {}

    /* 关闭并释放资源（幂等） */
    virtual void close() = 0;

    /* 描述信息（日志用） */
    virtual std::string name() const = 0;
};

/*
 * IFrameProcessor — 图像预处理抽象
 * 流水线中：YUV420 帧 →（缩放/格式转换/归一化）→ 推理输入
 * mock 实现：MockFrameProcessor（透传，模拟处理耗时）
 * RK3588 实现：RGA 硬件加速，尽量走 DMA-BUF 零拷贝
 */
class IFrameProcessor {
public:
    virtual ~IFrameProcessor() = default;

    /* 初始化预处理资源（如 RGA 会话） */
    virtual bool init() = 0;

    /*
     * 处理一帧。
     * @param in  输入帧
     * @param out 输出帧（可能改变尺寸/格式，如 RGB888 640x640）
     */
    virtual bool process(Frame in, Frame& out) = 0;
};

/*
 * IInferenceEngine — AI 推理引擎抽象
 * mock 实现：MockInferenceEngine（根据模拟场景生成目标框）
 * RK3588 实现：rknn_api 加载 YOLOv8n INT8 .rknn，输出原始张量后经 NMS
 */
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;

    /* 初始化模型（加载权重/分配 NPU 上下文） */
    virtual bool init() = 0;

    /*
     * 对一帧执行目标检测。
     * @param frame 输入帧（通常已预处理）
     * @param dets  输出：NMS 之后的最终目标框（YOLO 后处理已完成）
     * @return true=推理成功（dets 可能为空）
     */
    virtual bool infer(const Frame& frame, std::vector<Detection>& dets) = 0;
};

/*
 * IAlarmNotifier — 告警上报抽象
 * 实现：TcpAlarmNotifier（TCP 客户端，长度头+JSON 上报告警）
 * 移植 RK3588 无需改动；如需 MQTT/HTTP 上报，实现新子类即可。
 */
class IAlarmNotifier {
public:
    virtual ~IAlarmNotifier() = default;

    /* 连接告警服务器（可重连） */
    virtual bool connect(const std::string& host, int port) = 0;

    /* 上报一条告警（实现内部负责 JSON 序列化与发送） */
    virtual bool sendAlarm(const AlarmInfo& alarm) = 0;

    /* 断开连接并释放资源 */
    virtual void close() = 0;
};

} // namespace msad

#endif // MULTI_STREAM_HAL_H
