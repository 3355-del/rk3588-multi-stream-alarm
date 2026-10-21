/*
 * Nms.cpp — 手写 NMS 实现
 *
 * 算法：按类别分组 → 每组按置信度降序 → 贪心选取最高分框，
 *       抑制与已选框 IoU 超过阈值的其余框。
 */
#include "msad/Nms.h"

#include <algorithm>
#include <cmath>

namespace msad {

float computeIoU(const Box& a, const Box& b) {
    /* 计算交集 */
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);

    float interW = std::max(0.0f, x2 - x1);
    float interH = std::max(0.0f, y2 - y1);
    float inter = interW * interH;

    float areaA = a.w * a.h;
    float areaB = b.w * b.h;
    float uni = areaA + areaB - inter;
    if (uni <= 0.0f) return 0.0f;
    return inter / uni;
}

std::vector<Detection> nms(const std::vector<Detection>& dets, float iouThreshold) {
    std::vector<Detection> result;
    if (dets.empty()) return result;

    /* 按置信度降序排序（stable 保持同类相对顺序稳定） */
    std::vector<Detection> sorted;
    for (const auto& d : dets) {
        if (std::isfinite(d.confidence) && d.confidence >= 0.0f && d.confidence <= 1.0f &&
            std::isfinite(d.box.x) && std::isfinite(d.box.y) && std::isfinite(d.box.w) && std::isfinite(d.box.h) &&
            d.box.w > 0 && d.box.h > 0) sorted.push_back(d);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Detection& lhs, const Detection& rhs) {
                         return lhs.confidence > rhs.confidence;
                     });

    /* keep[i] = 是否保留第 i 个框 */
    std::vector<bool> keep(sorted.size(), true);

    for (size_t i = 0; i < sorted.size(); ++i) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < sorted.size(); ++j) {
            if (!keep[j]) continue;
            /* 类别感知：只有同类才做抑制（不同类目标可重叠） */
            if (sorted[j].classId != sorted[i].classId) continue;
            if (computeIoU(sorted[i].box, sorted[j].box) > iouThreshold) {
                keep[j] = false;   /* 抑制与高置信框重叠的框 */
            }
        }
    }

    for (size_t i = 0; i < sorted.size(); ++i) {
        if (keep[i]) result.push_back(sorted[i]);
    }
    return result;
}

} // namespace msad
