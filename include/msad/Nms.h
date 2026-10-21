/*
 * Nms.h — 手写 NMS（非极大值抑制）
 *
 * 职责：对模型输出的候选框集合执行类别感知的 NMS，
 *       去掉重叠冗余框，输出最终检测结果。
 * 要求：不引入第三方库，纯手写实现（简历亮点）。
 */
#ifndef MULTI_STREAM_NMS_H
#define MULTI_STREAM_NMS_H

#include <vector>

#include "msad/Types.h"

namespace msad {

/*
 * 计算两个矩形框的 IoU（交并比）。
 * @return [0,1]；任一面积为 0 时返回 0。
 */
float computeIoU(const Box& a, const Box& b);

/*
 * 类别感知 NMS。
 * @param dets        输入候选框（已按置信度过滤，如 > 0.25）
 * @param iouThreshold 重叠阈值，默认 0.45（YOLO 常规取值）
 * @return NMS 后的保留框（按置信度降序）
 */
std::vector<Detection> nms(const std::vector<Detection>& dets,
                           float iouThreshold = 0.45f);

} // namespace msad

#endif // MULTI_STREAM_NMS_H
