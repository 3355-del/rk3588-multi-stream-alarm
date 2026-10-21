/*
 * PolygonRegion.cpp — 多边形警戒区域实现
 */
#include "msad/PolygonRegion.h"

#include <algorithm>
#include <cmath>

namespace msad {

PolygonRegion::PolygonRegion(int id, std::vector<Point> vertices)
    : id_(id), vertices_(std::move(vertices)) {}

bool PolygonRegion::caresAbout(int classId) const {
    if (classes_.empty()) return true;   /* 未指定类别 = 关注所有 */
    return std::find(classes_.begin(), classes_.end(), classId) != classes_.end();
}

bool PolygonRegion::contains(const Point& p) const {
    /* 射线法（奇偶规则）：向右发射水平射线，统计与边的交点个数，
     * 奇数在内部，偶数在外部。经典且无需三角剖分。 */
    size_t n = vertices_.size();
    if (n < 3) return false;

    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& vi = vertices_[i];
        const Point& vj = vertices_[j];

        const double cross = (static_cast<double>(p.x)-vi.x)*(static_cast<double>(vj.y)-vi.y) -
                             (static_cast<double>(p.y)-vi.y)*(static_cast<double>(vj.x)-vi.x);
        if (std::fabs(cross) <= 1e-6 && p.x >= std::min(vi.x,vj.x) && p.x <= std::max(vi.x,vj.x) &&
            p.y >= std::min(vi.y,vj.y) && p.y <= std::max(vi.y,vj.y)) return true; // boundary is inside

        /* 边跨过 p.y（即 y 在两点 y 之间）且交点 x > p.x 时翻转状态 */
        bool crosses = ((vi.y > p.y) != (vj.y > p.y));
        if (!crosses) continue;

        /* 直线插值求射线与边的交点 x 坐标 */
        float xCross = (vj.x - vi.x) * (p.y - vi.y) / (vj.y - vi.y) + vi.x;
        if (p.x < xCross) inside = !inside;
    }
    return inside;
}

} // namespace msad
