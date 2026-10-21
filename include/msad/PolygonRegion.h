/*
 * PolygonRegion.h — 自定义多边形警戒区域
 *
 * 职责：
 *   - 保存一个警戒区域的多边形顶点（配置文件中定义）；
 *   - 提供"点是否在多边形内"判断（射线法/奇偶规则）；
 *   - 提供"检测框中心是否落入"快捷判断；
 *   - 附带该区域关注的类别集合与告警冷却时间。
 *
 * 可移植：纯几何算法，无任何平台依赖。
 */
#ifndef MULTI_STREAM_POLYGONREGION_H
#define MULTI_STREAM_POLYGONREGION_H

#include <vector>

#include "msad/Types.h"

namespace msad {

class PolygonRegion {
public:
    PolygonRegion() = default;

    /* 构造：区域 id + 多边形顶点（至少 3 点，逆/顺时针均可） */
    PolygonRegion(int id, std::vector<Point> vertices);

    int id() const { return id_; }

    /* 该区域是否关注某类别（空集合=关注所有类别） */
    bool caresAbout(int classId) const;

    void setClasses(std::vector<int> classes) { classes_ = std::move(classes); }

    /* 告警冷却时间(ms)：同一区域同类目标在冷却期内不重复告警 */
    void setCooldownMs(unsigned ms) { cooldownMs_ = ms; }
    unsigned cooldownMs() const { return cooldownMs_; }

    /* 点是否在多边形内部（射线法，含边界） */
    bool contains(const Point& p) const;

    /* 检测框中心是否落入区域 */
    bool containsCenter(const Box& box) const { return contains(box.center()); }

private:
    int id_ = -1;
    std::vector<Point> vertices_;
    std::vector<int> classes_;      /* 关注类别，空=全部 */
    unsigned cooldownMs_ = 5000;    /* 默认 5 秒冷却 */
};

} // namespace msad

#endif // MULTI_STREAM_POLYGONREGION_H
