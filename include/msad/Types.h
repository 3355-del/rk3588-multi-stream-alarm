/*
 * Types.h — 全工程共享的基础数据类型定义
 *
 * 说明：这些类型是硬件无关的纯数据载体，供 HAL 抽象层、流水线、
 *       算法模块共同使用。当前为CPU模拟帧；RK3588接入时需增加格式、stride和DMA缓冲生命周期表达。
 */
#ifndef MULTI_STREAM_TYPES_H
#define MULTI_STREAM_TYPES_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace msad {   // multi-stream alarm demo 命名空间

/* 二维点（像素坐标，浮点便于算法计算） */
struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

/* 矩形框（像素坐标：左上角 + 宽高） */
struct Box {
    float x = 0.0f;   /* 左上角 x */
    float y = 0.0f;   /* 左上角 y */
    float w = 0.0f;   /* 宽 */
    float h = 0.0f;   /* 高 */

    /* 返回中心点（警戒区判断常用） */
    Point center() const { return Point{x + w * 0.5f, y + h * 0.5f}; }
};

struct Detection {
    Box box;
    int classId = -1;
    float confidence = 0.0f;
};

/* 视频帧（YUV420P 平面格式，模拟解码输出；真实板卡为 MPP 解码帧） */
struct Frame {
    // External decoded surfaces retain their decoder/buffer owner until all consumers release them.
    enum class PixelFormat { Unspecified, NV12, RGB888, YUYV422 };
    PixelFormat pixelFormat = PixelFormat::Unspecified;
    int dmaFd = -1;
    int widthStride = 0;
    int heightStride = 0;
    std::shared_ptr<void> bufferOwner;
    uint8_t* mappedData=nullptr;
    size_t dmaBytes=0;
    // CPU access only for screenshots/debugging; synchronizes the DMA buffer.
    std::function<bool(std::vector<uint8_t>&)> copyRgb;
    int      width  = 0;        /* 帧宽 */
    int      height = 0;        /* 帧高 */
    int      streamId = -1;     /* 所属视频路号 */
    uint64_t timestampMs = 0;   /* 时间戳(ms)，演示用 */

    /*
     * 像素数据：YUV420 planar 布局
     *   Y 平面：w*h 字节
     *   U 平面：w/2*h/2 字节
     *   V 平面：w/2*h/2 字节
     */
    std::vector<uint8_t> data;
    // 模拟真值在渲染时冻结，随帧传递，推理不读取更新后的场景。
    std::vector<Detection> mockDetections;
    uint64_t frameIndex = 0;

    /* YUV420 一帧总字节数 */
    bool validDimensions() const {
        return width >= 2 && height >= 2 && width <= 4096 && height <= 4096 &&
               width % 2 == 0 && height % 2 == 0;
    }
    size_t yuvSize() const {
        return validDimensions() ? static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2 : 0;
    }
};

/* 单条检测结果（目标框 + 类别 + 置信度） */

/* 一帧画面经过推理后的完整检测结果（携带帧快照用于告警截图） */
struct DetectResult {
    uint64_t timestampMs = 0;
    int      streamId = -1;
    Frame    frame;                    /* 帧快照（告警截图时使用） */
    std::vector<Detection> detections; /* 该帧的全部目标 */
};

/* 告警信息（TCP 上报告警 JSON 的数据源） */
struct AlarmInfo {
    std::string eventId; // Stable identifier for deduplicated delivery.
    uint64_t timestampMs = 0;
    int      streamId = -1;    /* 哪一路 */
    int      regionId = -1;    /* 哪个警戒区域 */
    int      classId = -1;     /* 哪类目标 */
    float    confidence = 0.0f;/* 置信度 */
    float    centerX = 0.0f;   /* 目标中心 x */
    float    centerY = 0.0f;   /* 目标中心 y */
    std::string screenshotFile;/* 告警截图文件名（相对截图目录） */
};

/* 目标类别名（COCO 前 5 类演示；真实工程加载完整 80 类 names 文件） */
inline const char* kClassNames[] = {
    "person", "bicycle", "car", "motorcycle", "airplane"
};
inline constexpr int kNumClassNames = 5;

/* 根据 classId 取类别名，越界返回 "unknown" */
inline const char* className(int classId) {
    if (classId >= 0 && classId < kNumClassNames) return kClassNames[classId];
    return "unknown";
}

/*
 * 完整 COCO 80 类名称（RK3588 真实模型输出用）。
 * 与 kClassNames 在 0..4 上完全一致，因此模拟模式的输出文本保持不变；
 * 真实推理（YOLOv8n/v11n, COCO）需要 5 以上的类别名，故单独提供，
 * 不改变 kNumClassNames（模拟场景的类别域）语义。
 */
inline const char* kCocoClassNames[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
    "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
    "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
    "tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
    "potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
    "microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
    "hair drier","toothbrush"
};
inline constexpr int kNumCocoClassNames = 80;

/* 真实模型 classId → 类别名（含全部 80 类），越界返回 "unknown" */
inline const char* cocoClassName(int classId) {
    if (classId >= 0 && classId < kNumCocoClassNames) return kCocoClassNames[classId];
    return "unknown";
}

} // namespace msad

#endif // MULTI_STREAM_TYPES_H
