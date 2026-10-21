/*
 * SimScene.h — 模拟场景：为 mock 实现提供"可信"的运动目标
 *
 * 职责：
 *   - 维护若干运动目标（位置/速度/类别），目标在画面内匀速移动、碰壁反弹；
 *   - MockVideoSource 用该场景渲染出 YUV420 测试帧；
 *   - 渲染时将目标快照写入 Frame，MockInferenceEngine 仅读取该快照；
 *   - 这样"画面内容"与"检测结果"自洽，可完整演示
 *     采集 → 预处理 → 推理 → 警戒区判断 → 告警 全链路。
 *
 * 线程模型：同一场景仅由"采集线程渲染"与"推理线程读取"，通过互斥锁保护，
 * 保证线程安全；时间推进由外部调用 update() 控制。
 */
#ifndef MULTI_STREAM_SIMSCENE_H
#define MULTI_STREAM_SIMSCENE_H

#include <cstdint>
#include <mutex>
#include <vector>

#include "msad/Types.h"

namespace msad {

class SimScene {
public:
    /* 场景中的一个运动目标 */
    struct Target {
        float x = 0.0f;          /* 左上角 x（像素） */
        float y = 0.0f;          /* 左上角 y（像素） */
        float w = 0.0f;          /* 宽 */
        float h = 0.0f;          /* 高 */
        float vx = 0.0f;         /* x 方向速度（px/s） */
        float vy = 0.0f;         /* y 方向速度（px/s） */
        int classId = 0;         /* 目标类别 */
    };

    SimScene(int width, int height, int numTargets, unsigned seed = 20260828u);

    /* 推进场景时间 dt 秒：更新目标位置，碰壁反弹 */
    void update(float dtSec);

    /* 渲染当前场景到 YUV420 帧（含渐变背景与运动方块目标） */
    void render(Frame& frame) const;

    /* 返回当前目标列表副本（方法内部加锁；推理使用帧内快照） */
    std::vector<Target> targets() const;

    int width() const  { return width_; }
    int height() const { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<Target> targets_;
    mutable std::mutex mtx_;   /* 保护 targets_，渲染/读取并发安全 */
};

} // namespace msad

#endif // MULTI_STREAM_SIMSCENE_H
