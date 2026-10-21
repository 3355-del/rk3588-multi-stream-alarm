/*
 * IAiInfer.h — AI 推理抽象基类
 *
 * 职责：对一帧图像执行目标检测，输出检测框 + 置信度。
 *
 * 当前 x86 实现：MockAiInfer（模拟桩，不做真实图像检测，模拟输出检测框/置信度）；
 * 后续 RK3588 实现：RknnInfer（rknn_api 加载 YOLOv8n INT8 .rknn，
 *                 前向推理 + YOLO 后处理 + 手写 NMS）。
 *
 * 约定：init() 成功后才可调用 infer()。
 */
#ifndef MODULE_IAIINFER_H
#define MODULE_IAIINFER_H

#include <vector>

#include "msad/Types.h"   /* msad::Frame / msad::Detection */

namespace msad {

class IAiInfer {
public:
    virtual ~IAiInfer() = default;

    /* 初始化推理引擎（加载模型/分配资源） */
    virtual bool init() = 0;

    /*
     * 对一帧执行目标检测。
     * @param frame 输入帧
     * @param dets  输出目标框列表（可能为空）
     * @return true=推理成功
     */
    virtual bool infer(const Frame& frame, std::vector<Detection>& dets) = 0;
};

} // namespace msad

#endif // MODULE_IAIINFER_H
