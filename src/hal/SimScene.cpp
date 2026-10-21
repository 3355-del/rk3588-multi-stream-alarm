/*
 * SimScene.cpp — 模拟场景实现
 */
#include "msad/SimScene.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "msad/Logger.h"

namespace msad {

/* 简单确定性伪随机（避免依赖 <random> 状态复杂化） */
static unsigned int nextRand(unsigned int& s_seed) {
    /* xorshift32 */
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}
static float randFloat(unsigned int& seed, float lo, float hi) {
    return lo + (hi - lo) * (static_cast<float>(nextRand(seed) % 10000) / 10000.0f);
}

SimScene::SimScene(int width, int height, int numTargets, unsigned seed)
    : width_(width), height_(height) {
    Frame dimensions;
    dimensions.width = width;
    dimensions.height = height;
    if (!dimensions.validDimensions() || numTargets < 0 || numTargets > 1000)
        throw std::invalid_argument("invalid simulation dimensions or target count");
    if (seed == 0) seed = 20260828u;
    targets_.reserve(static_cast<size_t>(numTargets));
    for (int i = 0; i < numTargets; ++i) {
        Target t;
        /* 目标尺寸：画面宽高的 5%~10% */
        t.w = std::min(static_cast<float>(width), std::max(2.0f, static_cast<float>(width) * randFloat(seed, 0.05f, 0.10f)));
        t.h = std::min(static_cast<float>(height), std::max(2.0f, t.w * randFloat(seed, 0.8f, 1.2f)));
        t.x = randFloat(seed, 0.0f, static_cast<float>(width) - t.w);
        t.y = randFloat(seed, 0.0f, static_cast<float>(height) - t.h);
        /* 速度：每秒 5%~15% 画面宽/高 */
        t.vx = randFloat(seed, 0.05f, 0.15f) * static_cast<float>(width) * (nextRand(seed) % 2 ? 1.0f : -1.0f);
        t.vy = randFloat(seed, 0.05f, 0.15f) * static_cast<float>(height) * (nextRand(seed) % 2 ? 1.0f : -1.0f);
        t.classId = static_cast<int>(nextRand(seed) % kNumClassNames);
        targets_.push_back(t);
    }
}

void SimScene::update(float dtSec) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& t : targets_) {
        t.x += t.vx * dtSec;
        t.y += t.vy * dtSec;

        /* 碰壁反弹 */
        if (t.x < 0.0f) { t.x = 0.0f; t.vx = std::fabs(t.vx); }
        if (t.x + t.w > static_cast<float>(width_)) { t.x = static_cast<float>(width_) - t.w; t.vx = -std::fabs(t.vx); }
        if (t.y < 0.0f) { t.y = 0.0f; t.vy = std::fabs(t.vy); }
        if (t.y + t.h > static_cast<float>(height_)) { t.y = static_cast<float>(height_) - t.h; t.vy = -std::fabs(t.vy); }
    }
}

void SimScene::render(Frame& frame) const {
    std::lock_guard<std::mutex> lock(mtx_);
    frame.width = width_;
    frame.height = height_;
    frame.data.assign(frame.yuvSize(), 0);
    frame.mockDetections.clear();
    for (const auto& t : targets_) {
        frame.mockDetections.push_back(Detection{Box{t.x, t.y, t.w, t.h}, t.classId,
            0.75f + static_cast<float>((t.classId * 7) % 24) / 100.0f});
    }

    const size_t yPlane = static_cast<size_t>(width_) * height_;
    const size_t uPlane = yPlane / 4;
    /* 索引工具：Y(x,y)=y*W+x ; UV(x,y)= (y/2)*(W/2) + (x/2) */
    auto idxY = [this](int x, int y) -> size_t {
        return static_cast<size_t>(y) * width_ + x;
    };
    auto idxU = [this](int x, int y) -> size_t {
        return static_cast<size_t>(y / 2) * (width_ / 2) + (x / 2);
    };

    /* 渐变背景：亮度从左到右 90~170 */
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            frame.data[idxY(x, y)] = static_cast<uint8_t>(90 + x * 80 / width_);
        }
    }
    /* 中性色度（灰） */
    for (int y = 0; y < height_; y += 2) {
        for (int x = 0; x < width_; x += 2) {
            frame.data[yPlane + idxU(x, y)]            = 128;
            frame.data[yPlane + uPlane + idxU(x, y)]   = 128;
        }
    }

    /* 绘制每个目标为纯色方块（按类别给不同颜色） */
    static const uint8_t kClassY[]  = {200, 120, 60, 160, 90};
    static const uint8_t kClassU[]  = {90,  170, 90, 60,  200};
    static const uint8_t kClassV[]  = {240, 60,  200, 160, 80};
    for (const auto& t : targets_) {
        int x0 = std::max(0, static_cast<int>(t.x));
        int y0 = std::max(0, static_cast<int>(t.y));
        int x1 = std::min(width_ - 1, static_cast<int>(t.x + t.w));
        int y1 = std::min(height_ - 1, static_cast<int>(t.y + t.h));
        if (x1 <= x0 || y1 <= y0) continue;

        int cid = t.classId % kNumClassNames;
        uint8_t yv = kClassY[cid];
        uint8_t uv = kClassU[cid];
        uint8_t vv = kClassV[cid];
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                frame.data[idxY(x, y)] = yv;
            }
        }
        for (int y = y0; y <= y1; y += 2) {
            for (int x = x0; x <= x1; x += 2) {
                frame.data[yPlane + idxU(x, y)]          = uv;
                frame.data[yPlane + uPlane + idxU(x, y)] = vv;
            }
        }
    }
}

std::vector<SimScene::Target> SimScene::targets() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return targets_;
}

} // namespace msad
