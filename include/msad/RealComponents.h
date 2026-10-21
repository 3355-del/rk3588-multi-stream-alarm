/*
 * RealComponents.h — RK3588 真实硬件 HAL 组件（替换 Mock 实现）
 *
 * 数据流（单路）：
 *   USB摄像头(V4L2/YUYV) → RGA硬件缩放+格式转换(→640x640 RGB) → RKNN NPU推理
 *   → DFL解码 + 置信度过滤（NMS 由流水线统一执行） → 警戒区判断 → 截图 + TCP告警
 *
 * 三个真实实现：
 *   - V4L2CameraSource   : IVideoSource     真实 USB 摄像头采集（V4L2 MMAP，YUYV 免解码）
 *   - RgaFrameProcessor  : IFrameProcessor  RGA 硬件缩放 + 格式转换（等比缩放 + 居中黑边）
 *   - RknnInferenceEngine: IInferenceEngine RKNN NPU 推理（YOLOv8/v11n INT8，DFL 后处理）
 *
 * 设计要点：
 *   1) 与 Mock 实现接口完全一致，业务逻辑（四线程流水线/环形队列/警戒区/告警）零改动；
 *   2) 本头文件不包含 rknn_api.h / im2d.h 等板卡头文件，仅暴露 msad::Hal 接口，
 *      因此只有 RealComponents.h/.cpp 需要板卡 SDK（见 CMake 的 MSAD_ENABLE_RKNN 开关）；
 *   3) DMA 帧携带显式格式、stride 与 owner；CPU 帧兼容按 data.size() 推断：
 *        YUYV422: w*h*2  /  RGB888: w*h*3  /  YUV420P: w*h*3/2
 *
 * 坐标系约定（重要）：预处理输出即推理输入，尺寸 = 模型输入（默认 640x640），
 *   检测框、警戒区多边形、告警截图全部使用该统一坐标系，无需坐标换算。
 */
#ifndef MULTI_STREAM_REALCOMPONENTS_H
#define MULTI_STREAM_REALCOMPONENTS_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "msad/TensorView.h"

#include "msad/Hal.h"

namespace msad {
struct RknnModelContext;

/*
 * V4L2CameraSource — 真实 USB 摄像头视频源（YUYV 422）
 * open(uri) 的 uri 为设备路径（如 /dev/video0）。
 * 输出帧：width x height，data = YUYV422（w*h*2 字节）。
 */
class V4L2CameraSource : public IVideoSource {
public:
    V4L2CameraSource(int width = 640, int height = 480);
    ~V4L2CameraSource() override;

    bool open(const std::string& uri) override;
    bool readFrame(Frame& frame) override;
    void close() override;
    std::string name() const override { return "V4L2CameraSource"; }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Buffer { void* start = nullptr; size_t length = 0; };

    int         fd_ = -1;
    int         width_ = 640;
    int         height_ = 480;
    size_t      rowStride_ = 0;
    bool        streaming_ = false;
    uint32_t    lastIndex_ = 0;
    uint64_t    frameIndex_ = 0;
    std::string device_;
    std::vector<Buffer> buffers_;
};

/*
 * RgaFrameProcessor — RGA 硬件图像预处理
 * 输入：任意尺寸 YUYV422 / RGB888
 * 输出：outWidth x outHeight（默认 640x640）RGB888，
 *       等比缩放后居中放置，四周补黑边（letterbox，避免长宽比失真导致检测框变形）。
 * flip180：摄像头倒装时把图像旋转 180°（在预处理阶段完成，
 *          因此检测框/警戒区/截图仍共用同一坐标系，无需额外换算）。
 */
class RgaFrameProcessor : public IFrameProcessor {
public:
    RgaFrameProcessor(int outWidth = 640, int outHeight = 640, bool flip180 = false, bool dmaInput = true);
    ~RgaFrameProcessor() override;

    bool init() override;
    bool process(Frame in, Frame& out) override;

private:
    int outW_ = 640;
    int outH_ = 640;
    bool flip180_ = false;
    bool dmaInput_ = true;
    int fitW_ = 0;                 /* 最近一次等比缩放后的有效图像尺寸（含缓存） */
    int fitH_ = 0;
    std::vector<uint8_t> fit_;     /* 等比缩放结果（连续存放，供逐行搬运） */
};

/*
 * RknnInferenceEngine — RKNN NPU 推理引擎（YOLOv8 / YOLOv11 检测头）
 * 输入：模型输入尺寸的 RGB888 帧（由 RgaFrameProcessor 产出）
 * 输出：置信度过滤后的候选框（坐标 = 模型输入坐标系，未做 NMS）
 *
 * 后处理：9 个分离输出（3 尺度 × [DFL 64ch, 类别 N ch, 置信度 1ch]）
 *   → DFL 期望解码框 + 类别分数 → 置信度过滤 → 按分数降序截断
 *   NMS 由 VideoPipeline 调用工程自研 msad::nms 统一执行（保持算法单一来源）。
 */
class RknnInferenceEngine : public IInferenceEngine {
public:
    explicit RknnInferenceEngine(const std::string& modelPath, float confThresh = 0.25f,
        std::shared_ptr<RknnModelContext> sharedModel = {}, bool nativeOutputs = true);
    std::shared_ptr<RknnModelContext> sharedModel() const { return sharedModel_; }
    ~RknnInferenceEngine() override;

    bool init() override;
    bool infer(const Frame& frame, std::vector<Detection>& dets) override;

    /* 模型输入尺寸（供日志/坐标换算说明） */
    int modelWidth() const { return modelW_; }
    int modelHeight() const { return modelH_; }

private:
    void decode(const std::vector<TensorView>& outBufs, std::vector<Detection>& dets) const;
    std::shared_ptr<RknnModelContext> sharedModel_;
    bool nativeOutputs_ = true;
    int inputMode_ = -1; // SDK input APIs must not be mixed on one context.
    struct OutputMeta {size_t count; int type; int qnt; float scale; int zp;};
    std::vector<OutputMeta> outputMeta_;

    std::string modelPath_;
    void*       ctx_ = nullptr;      /* rknn_context（void* 以避免头文件依赖 SDK） */
    int         modelW_ = 640;
    int         modelH_ = 640;
    int         numInput_ = 0;
    int         numOutput_ = 0;
    int         classNum_ = 80;
    float       confThresh_ = 0.25f;
    bool        debugOnce_ = false;

    /* 每个尺度的输出布局：{H, W, dfl 输出下标, 类别输出下标} */
    struct Scale { int h; int w; int dflIdx; int clsIdx; };
    std::vector<Scale> scales_;
};

} // namespace msad

#endif // MULTI_STREAM_REALCOMPONENTS_H
