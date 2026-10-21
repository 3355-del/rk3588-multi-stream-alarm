/*
 * DetectionResult.h — 推理结果数据结构（业务模块专用）
 *
 * 说明：
 *   - 复用公共组件 msad::Frame（宽高/时间戳/模拟图像数据）与 msad::Detection；
 *   - 本文件定义解码+推理模块专用的推理结果结构 DetectionResult，
 *     与已交付告警系统的 DetectResult 相互独立，互不干扰。
 */
#ifndef MODULE_DETECTIONRESULT_H
#define MODULE_DETECTIONRESULT_H

#include <cstdint>
#include <vector>

#include "msad/Types.h"   /* 复用 msad::Detection / msad::Frame */

namespace msad {

/* 一帧画面推理后的结果（目标框列表 + 时间/路号信息） */
struct DetectionResult {
    uint64_t timestampMs = 0;              /* 推理完成时间戳(ms) */
    int streamId = -1;                     /* 所属视频源路号 */
    uint64_t frameIndex = 0;               /* 帧序号（调试用） */
    std::vector<Detection> detections;     /* 检测到的目标框列表 */
};

} // namespace msad

#endif // MODULE_DETECTIONRESULT_H
